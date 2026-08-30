/* _XOPEN_SOURCE exposes ucontext (getcontext/makecontext/swapcontext), but on
 * macOS it also HIDES the BSD extensions — including MAP_ANON, which the
 * coroutine stack's guard page needs. _DARWIN_C_SOURCE puts them back without
 * giving up the XSI namespace. */
#define _XOPEN_SOURCE 600
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "actraiser_rtl.h"
#include "actraiser_game.h"
#include "actraiser_action_bg.h"
#include "actraiser_hle_fatal.h"
#include "action/action_bg_tuner.h"
#include "action/action_effects.h"
#include "action/action_load_pacing.h"
#include "actraiser_ws_gap.h"
#include "cpu_65816_math.h"
#include "diorama/diorama_capture_blend.h"
#include "diorama/diorama.h"
#include "diorama/diorama_layer_order.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_planes.h"
#include "deterministic_hash.h"
#include "host/host_display.h"   /* kHostDisplayFramebufferHeight */
#include "settings.h"
#include "session_fatal.h"
#include "audio_presentation_policy.h"
#include "hd_replacement_host.h"
#include "hd_replacements.h"
#include "music_replacements.h"
#include "native_audio_extension.h"
#include "randomizer.h"
#include "dev/native_audio_trace.h"
#include "dev/hd_tile_census.h"
#include "dev/sfx_census.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim3d.h"
#include "sim/sim_visual_patches.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "snesrecomp/game/trace.h"
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <sys/mman.h>   /* mmap: guard page below the coroutine stack */
#include <unistd.h>
/* _XOPEN_SOURCE (needed for ucontext) hides MAP_ANONYMOUS on some libcs;
 * macOS spells it MAP_ANON. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#include <stdlib.h>
#include <string.h>

/* The game runs as a ucontext coroutine so a VBlank wait can yield mid-frame
 * (see docs/rendering-engine.md). macOS deprecated get/make/swapcontext in
 * 10.6 and offers no replacement with the same semantics; the functions still
 * work and the model depends on them, so the deprecation is acknowledged here
 * rather than repeated at four call sites. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

extern int snes_frame_counter;

enum {
  kGameCoroutineStackReserveBytes = 2 * 1024 * 1024,
  kGameCoroutineStackCommitBytes = 64 * 1024,
  kRdnmiRepeatedReadWarningThreshold = 4096,
};

static ActRaiserRomSetupResult s_rom_setup_result;
static SrRunnerHandle *s_runner;
static const SnesRunnerApi *s_runner_api;

void ActRaiser_BindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
  s_runner_api = runner
      ? sr_runner_get_api(SR_RUNNER_ABI_VERSION)
      : NULL;
}

static bool ActRaiser_QueryPpuState(SrPpuStateSnapshot *state) {
  if (!state || !s_runner || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !s_runner_api->query_ppu_state)
    return false;
  *state = (SrPpuStateSnapshot){
    .struct_size = sizeof(*state),
  };
  return s_runner_api->query_ppu_state(s_runner, state) == SR_RESULT_OK;
}

static uint8_t ActRaiser_QueryHdmaActiveMask(void) {
  SrDmaStateSnapshot state = {
    .struct_size = sizeof(state),
  };
  uint8_t mask = 0u;
  if (!s_runner || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_DMA_STATE_SIZE ||
      (s_runner_api->capabilities & SR_RUNNER_CAP_DMA_STATE) == 0u ||
      !s_runner_api->query_dma_state ||
      s_runner_api->query_dma_state(s_runner, &state) != SR_RESULT_OK)
    return 0u;
  for (uint32_t channel = 0u;
       channel < state.channel_count && channel < SR_DMA_CHANNEL_COUNT;
       ++channel) {
    if ((state.channels[channel].flags & SR_DMA_CHANNEL_HDMA_ACTIVE) != 0u)
      mask |= (uint8_t)(1u << channel);
  }
  return mask;
}

static bool ActRaiser_ResetPpuFrameCaptures(void) {
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu) || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_FRAME_RESET_SIZE ||
      !s_runner_api->reset_ppu_frame_state)
    return false;
  const SrPpuFrameResetRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = ppu.lifetime_generation,
  };
  return s_runner_api->reset_ppu_frame_state(
             s_runner, &request) == SR_RESULT_OK;
}

static bool ActRaiser_ClearPpuObjMetadata(void) {
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu) || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE ||
      !s_runner_api->update_ppu_obj_metadata)
    return false;
  const SrPpuObjMetadataRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_OBJ_METADATA_CLEAR_POSITIONS |
             SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE,
    .lifetime_generation = ppu.lifetime_generation,
  };
  return s_runner_api->update_ppu_obj_metadata(
             s_runner, &request) == SR_RESULT_OK;
}

bool ActRaiser_InitializeGame(
    const RtlGameInitializeContext *context) {
  s_rom_setup_result = (ActRaiserRomSetupResult){0};
  if (!context ||
      context->struct_size < RTL_GAME_INITIALIZE_CONTEXT_V1_SIZE)
    return false;
  if ((context->flags & RTL_GAME_INITIALIZE_HAS_ROM) == 0)
    return true;
  if (!context->rom_data || !context->rom_byte_size ||
      context->rom_byte_size > SIZE_MAX)
    return false;

  s_rom_setup_result.visual_patches_applied = SimVisualPatches_Apply(
      context->rom_data, (size_t)context->rom_byte_size);
  s_rom_setup_result.randomizer_initialized = Randomizer_Init(
      context->rom_data, (size_t)context->rom_byte_size);
  if (s_rom_setup_result.randomizer_initialized) Randomizer_Apply();
  return true;
}

ActRaiserRomSetupResult ActRaiser_LastRomSetupResult(void) {
  return s_rom_setup_result;
}

/* Developer-only environment controls are immutable for a game process. Keep
 * their historical presence-based syntax, but snapshot them before the first
 * emulated frame so diagnostics do not repeatedly traverse the host
 * environment from vblank, object, or presentation paths. */
typedef enum ActRaiserDeveloperFlag {
  kActRaiserDeveloperFlag_DisableActionLoadPacing,
  kActRaiserDeveloperFlag_LoadPacingLog,
  kActRaiserDeveloperFlag_VblankLog,
  kActRaiserDeveloperFlag_CopLog,
  kActRaiserDeveloperFlag_YieldLog,
  kActRaiserDeveloperFlag_FrameLog,
  kActRaiserDeveloperFlag_ObjectLog,
  kActRaiserDeveloperFlag_PpuLog,
  kActRaiserDeveloperFlag_WidescreenLayerLog,
  kActRaiserDeveloperFlag_VerticalExtensionTileLog,
  kActRaiserDeveloperFlag_VerticalExtensionLog,
  kActRaiserDeveloperFlag_HudIconLog,
  kActRaiserDeveloperFlag_ApronLog,
  kActRaiserDeveloperFlag_TitleLog,
  kActRaiserDeveloperFlag_Count,
} ActRaiserDeveloperFlag;

typedef struct ActRaiserDeveloperEnvironment {
  bool initialized;
  bool flags[kActRaiserDeveloperFlag_Count];
  bool widescreen_only_bg_present;
  int widescreen_only_bg_layer;
  bool widescreen_clamp_present;
  uint8_t widescreen_clamp_mask;
} ActRaiserDeveloperEnvironment;

static const char *const kActRaiserDeveloperFlagNames[] = {
  [kActRaiserDeveloperFlag_DisableActionLoadPacing] =
      "AR_NO_ACTION_LOAD_PACING",
  [kActRaiserDeveloperFlag_LoadPacingLog] = "AR_LOADPACELOG",
  [kActRaiserDeveloperFlag_VblankLog] = "AR_VBLOG",
  [kActRaiserDeveloperFlag_CopLog] = "AR_COPLOG",
  [kActRaiserDeveloperFlag_YieldLog] = "AR_YIELDLOG",
  [kActRaiserDeveloperFlag_FrameLog] = "AR_FRAMELOG",
  [kActRaiserDeveloperFlag_ObjectLog] = "AR_OBJLOG",
  [kActRaiserDeveloperFlag_PpuLog] = "AR_PPULOG",
  [kActRaiserDeveloperFlag_WidescreenLayerLog] = "AR_WS_LAYERS",
  [kActRaiserDeveloperFlag_VerticalExtensionTileLog] = "AR_VEXT_TILES",
  [kActRaiserDeveloperFlag_VerticalExtensionLog] = "AR_VEXT_LOG",
  [kActRaiserDeveloperFlag_HudIconLog] = "AR_HUDICON",
  [kActRaiserDeveloperFlag_ApronLog] = "AR_APRONLOG",
  [kActRaiserDeveloperFlag_TitleLog] = "AR_TITLELOG",
};

_Static_assert(
    sizeof(kActRaiserDeveloperFlagNames) /
        sizeof(kActRaiserDeveloperFlagNames[0]) ==
        kActRaiserDeveloperFlag_Count,
    "developer environment flag table is incomplete");

static ActRaiserDeveloperEnvironment s_developer_environment;

static const ActRaiserDeveloperEnvironment *
ActRaiser_GetDeveloperEnvironment(void) {
  if (s_developer_environment.initialized)
    return &s_developer_environment;

  for (unsigned flag = 0; flag < kActRaiserDeveloperFlag_Count; flag++)
    s_developer_environment.flags[flag] =
        getenv(kActRaiserDeveloperFlagNames[flag]) != NULL;

  const char *value = getenv("AR_WS_ONLYBG");
  s_developer_environment.widescreen_only_bg_present =
      value && value[0];
  s_developer_environment.widescreen_only_bg_layer =
      s_developer_environment.widescreen_only_bg_present
          ? atoi(value) - 1
          : -1;

  value = getenv("AR_WS_CLAMP");
  s_developer_environment.widescreen_clamp_present = value && value[0];
  s_developer_environment.widescreen_clamp_mask =
      s_developer_environment.widescreen_clamp_present
          ? (uint8_t)strtoul(value, NULL, 16)
          : 0;
  s_developer_environment.initialized = true;
  return &s_developer_environment;
}

static bool ActRaiser_DeveloperFlagEnabled(ActRaiserDeveloperFlag flag) {
  return ActRaiser_GetDeveloperEnvironment()->flags[flag];
}

/* Bounded behavioral-oracle trace for renderer parity work.  This deliberately
 * lives above both PPU implementations: the same game-side call site records
 * the registers presented to scanout, the resulting visible-row hashes, and
 * the registers left by HDMA for the following line.  That makes the first
 * divergent contract visible without teaching either renderer about its
 * comparison peer.
 *
 * AR_PPU_SHAPE_TRACE=<path> enables CSV output.  AR_PPU_SHAPE_GF selects one
 * game frame, while AR_PPU_SHAPE_GF_LO/HI select an inclusive range.  The
 * record ceiling defaults to 4096 and is hard-capped so a stuck frame cannot
 * produce another unbounded diagnostic file. */
enum {
  kActRaiserPpuShapeDefaultRecords = 4096,
  kActRaiserPpuShapeMaximumRecords = 65536,
};

typedef struct ActRaiserPpuShapeRegisters {
  uint8_t inidisp, bgmode, mosaic, m7sel, setini;
  uint8_t bg_xsc[4];
  uint16_t bg_tile_adr;
  uint16_t hscroll[4], vscroll[4];
  int16_t m7matrix[8];
  uint8_t screen_enabled[2], screen_windowed[2];
  uint32_t windowsel;
  uint16_t wbgobjlog, fixed_color;
  uint8_t cgwsel, cgadsub;
} ActRaiserPpuShapeRegisters;

typedef struct ActRaiserPpuShapeTrace {
  bool initialized;
  bool limit_reported;
  FILE *file;
  unsigned gf_lo, gf_hi;
  unsigned maximum_records, records;
} ActRaiserPpuShapeTrace;

static ActRaiserPpuShapeTrace s_ppu_shape_trace;

static unsigned ActRaiser_PpuShapeUnsignedEnvironment(
    const char *name, unsigned fallback, unsigned maximum) {
  const char *text = getenv(name);
  char *end = NULL;
  unsigned long value;
  if (!text || !text[0])
    return fallback;
  value = strtoul(text, &end, 0);
  if (end == text || *end != '\0')
    return fallback;
  return value > maximum ? maximum : (unsigned)value;
}

static bool ActRaiser_PpuShapeTraceActive(unsigned gf) {
  ActRaiserPpuShapeTrace *trace = &s_ppu_shape_trace;
  if (!trace->initialized) {
    const char *path = getenv("AR_PPU_SHAPE_TRACE");
    const unsigned any_gf = kActRaiserPpuShapeMaximumRecords;
    unsigned exact_gf = ActRaiser_PpuShapeUnsignedEnvironment(
        "AR_PPU_SHAPE_GF", any_gf, 0xffffu);
    trace->initialized = true;
    trace->gf_lo = exact_gf != any_gf
        ? exact_gf
        : ActRaiser_PpuShapeUnsignedEnvironment(
              "AR_PPU_SHAPE_GF_LO", 0u, 0xffffu);
    trace->gf_hi = exact_gf != any_gf
        ? exact_gf
        : ActRaiser_PpuShapeUnsignedEnvironment(
              "AR_PPU_SHAPE_GF_HI", 0xffffu, 0xffffu);
    trace->maximum_records = ActRaiser_PpuShapeUnsignedEnvironment(
        "AR_PPU_SHAPE_MAX", kActRaiserPpuShapeDefaultRecords,
        kActRaiserPpuShapeMaximumRecords);
    if (path && path[0]) {
      trace->file = fopen(path, "wb");
      if (!trace->file) {
        fprintf(stderr, "[ppu-shape] unable to open %s: %s\n", path,
                strerror(errno));
      } else {
        fprintf(trace->file,
                "gf,host_frame,line,pre_inidisp,pre_bgmode,pre_mosaic,"
                "pre_m7sel,pre_setini,pre_bg1sc,pre_bg2sc,pre_bg3sc,"
                "pre_bg4sc,pre_bgtile,pre_h1,pre_h2,pre_h3,pre_h4,"
                "pre_v1,pre_v2,pre_v3,pre_v4,pre_m7a,pre_m7b,pre_m7c,"
                "pre_m7d,pre_m7x,pre_m7y,pre_m7h,pre_m7v,pre_tm,pre_ts,"
                "pre_tmw,pre_tsw,pre_winsel,pre_wbgobj,pre_fixed,"
                "pre_cgwsel,pre_cgadsub,visible_hash,visible_left_hash,"
                "visible_center_hash,visible_right_hash,authentic_hash,"
                "post_inidisp,post_bgmode,post_mosaic,post_m7sel,"
                "post_setini,post_bg1sc,post_bg2sc,post_bg3sc,post_bg4sc,"
                "post_bgtile,post_h1,post_h2,post_h3,post_h4,post_v1,"
                "post_v2,post_v3,post_v4,post_m7a,post_m7b,post_m7c,"
                "post_m7d,post_m7x,post_m7y,post_m7h,post_m7v,post_tm,"
                "post_ts,post_tmw,post_tsw,post_winsel,post_wbgobj,"
                "post_fixed,post_cgwsel,post_cgadsub,hdma_rep0,hdma_rep1,"
                "hdma_rep2,hdma_rep3,hdma_rep4,hdma_rep5,hdma_rep6,"
                "hdma_rep7,hdma_live\n");
        fprintf(stderr,
                "[ppu-shape] tracing gf=%u..%u, maximum %u records -> %s\n",
                trace->gf_lo, trace->gf_hi, trace->maximum_records, path);
      }
    }
  }
  if (!trace->file || gf < trace->gf_lo || gf > trace->gf_hi)
    return false;
  if (trace->records < trace->maximum_records)
    return true;
  if (!trace->limit_reported) {
    trace->limit_reported = true;
    fflush(trace->file);
    fprintf(stderr, "[ppu-shape] stopped at the %u-record safety limit\n",
            trace->maximum_records);
  }
  return false;
}

static void ActRaiser_PpuShapeCaptureRegisters(
    const SrPpuStateSnapshot *ppu, ActRaiserPpuShapeRegisters *output) {
  memset(output, 0, sizeof(*output));
  output->inidisp = ppu->display_control;
  output->bgmode = ppu->bg_mode_control;
  output->mosaic = ppu->mosaic_control;
  output->m7sel = ppu->mode7_select;
  output->setini =
      ((ppu->flags & SR_PPU_STATE_INTERLACE) ? 0x01u : 0u) |
      ((ppu->flags & SR_PPU_STATE_OBJ_INTERLACE) ? 0x02u : 0u) |
      ((ppu->flags & SR_PPU_STATE_OVERSCAN) ? 0x04u : 0u) |
      ((ppu->flags & SR_PPU_STATE_PSEUDO_HIRES) ? 0x08u : 0u) |
      ((ppu->flags & SR_PPU_STATE_MODE7_EXT_BG) ? 0x40u : 0u);
  memcpy(output->bg_xsc, ppu->background_tilemap_control,
         sizeof(output->bg_xsc));
  output->bg_tile_adr = ppu->background_tile_base_control;
  for (unsigned layer = 0; layer < 4; layer++) {
    output->hscroll[layer] = ppu->backgrounds[layer].h_scroll;
    output->vscroll[layer] = ppu->backgrounds[layer].v_scroll;
  }
  memcpy(output->m7matrix, ppu->mode7_matrix, sizeof(output->m7matrix));
  output->screen_enabled[0] = ppu->main_screen;
  output->screen_enabled[1] = ppu->sub_screen;
  output->screen_windowed[0] = ppu->main_windowed;
  output->screen_windowed[1] = ppu->sub_windowed;
  output->windowsel = ppu->window_select;
  output->wbgobjlog = ppu->window_logic;
  output->fixed_color = ppu->fixed_color;
  output->cgwsel = ppu->color_math_control;
  output->cgadsub = ppu->color_math_designation;
}

static uint64_t ActRaiser_PpuShapeHash(const void *data, size_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t ActRaiser_PpuShapeRenderRangeHash(
    const SrPpuSurfaceView *surface, int line, int left, int right) {
  if (!surface) return 0;
  const int64_t row = (int64_t)line - 1 + surface->origin_y;
  const int64_t column = (int64_t)surface->origin_x + left;
  const int64_t end = (int64_t)surface->origin_x + right;
  if ((surface->flags & SR_PPU_SURFACE_BOUND) == 0u ||
      surface->pixel_format != SR_PPU_PIXEL_FORMAT_ARGB8888_U32 ||
      !surface->data || !surface->pitch_bytes || line <= 0 ||
      row < 0 || row >= surface->height_pixels ||
      column < 0 || end > surface->width_pixels || right <= left)
    return 0;
  return ActRaiser_PpuShapeHash(
      surface->data + (uint64_t)row * surface->pitch_bytes +
          (uint64_t)column * sizeof(uint32_t),
      (size_t)(right - left) * sizeof(uint32_t));
}

static uint64_t ActRaiser_PpuShapeRenderHash(
    const SrPpuScanoutLineContext *context, int line, bool authentic) {
  const SrPpuSurfaceView *surface = authentic
      ? &context->authentic_surface : &context->main_surface;
  return ActRaiser_PpuShapeRenderRangeHash(
      surface, line, authentic ? 0 : -(int)context->state.margin_left,
      authentic ? SR_PPU_NATIVE_WIDTH
                  : SR_PPU_NATIVE_WIDTH + (int)context->state.margin_right);
}

static void ActRaiser_PpuShapeWriteRegisters(
    FILE *file, const ActRaiserPpuShapeRegisters *state) {
  fprintf(file,
          "%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%04x,"
          "%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,"
          "%04x,%04x,%04x,%04x,%04x,%04x,%02x,%02x,%02x,%02x,"
          "%08x,%04x,%04x,%02x,%02x",
          state->inidisp, state->bgmode, state->mosaic, state->m7sel,
          state->setini, state->bg_xsc[0], state->bg_xsc[1],
          state->bg_xsc[2], state->bg_xsc[3], state->bg_tile_adr,
          state->hscroll[0], state->hscroll[1], state->hscroll[2],
          state->hscroll[3], state->vscroll[0], state->vscroll[1],
          state->vscroll[2], state->vscroll[3],
          (uint16_t)state->m7matrix[0], (uint16_t)state->m7matrix[1],
          (uint16_t)state->m7matrix[2], (uint16_t)state->m7matrix[3],
          (uint16_t)state->m7matrix[4], (uint16_t)state->m7matrix[5],
          (uint16_t)state->m7matrix[6], (uint16_t)state->m7matrix[7],
          state->screen_enabled[0], state->screen_enabled[1],
          state->screen_windowed[0], state->screen_windowed[1],
          state->windowsel, state->wbgobjlog, state->fixed_color,
          state->cgwsel, state->cgadsub);
}

static void ActRaiser_PpuShapeTraceLine(
    unsigned gf, int line, const ActRaiserPpuShapeRegisters *before,
    const SrPpuScanoutLineContext *context) {
  ActRaiserPpuShapeTrace *trace = &s_ppu_shape_trace;
  ActRaiserPpuShapeRegisters after;
  unsigned live = 0;
  if (!trace->file || trace->records >= trace->maximum_records)
    return;
  ActRaiser_PpuShapeCaptureRegisters(&context->state, &after);
  fprintf(trace->file, "%u,%d,%d,", gf, snes_frame_counter, line);
  ActRaiser_PpuShapeWriteRegisters(trace->file, before);
  fprintf(trace->file, ",%016llx,%016llx,%016llx,%016llx,%016llx,",
          (unsigned long long)ActRaiser_PpuShapeRenderHash(
              context, line, false),
          (unsigned long long)ActRaiser_PpuShapeRenderRangeHash(
              &context->main_surface, line,
              -(int)context->state.margin_left, 0),
          (unsigned long long)ActRaiser_PpuShapeRenderRangeHash(
              &context->main_surface, line, 0, SR_PPU_NATIVE_WIDTH),
          (unsigned long long)ActRaiser_PpuShapeRenderRangeHash(
              &context->main_surface, line, SR_PPU_NATIVE_WIDTH,
              SR_PPU_NATIVE_WIDTH + (int)context->state.margin_right),
          (unsigned long long)ActRaiser_PpuShapeRenderHash(
              context, line, true));
  ActRaiser_PpuShapeWriteRegisters(trace->file, &after);
  for (unsigned ch = 0; ch < context->channel_count; ch++) {
    if ((context->channels[ch].flags & SR_PPU_SCANOUT_HDMA_ACTIVE) != 0u)
      live |= 1u << ch;
    fprintf(trace->file, ",%02x", context->channels[ch].repeat_count);
  }
  fprintf(trace->file, ",%02x\n", live);
  trace->records++;
  if (line == kActRaiserAuthenticHeight)
    fflush(trace->file);
}

#ifdef _WIN32
static void *g_host_fiber;   /* ConvertThreadToFiber result (driver thread) */
static void *g_game_fiber;   /* CreateFiber result (game coroutine) */
#else
static ucontext_t g_host_ctx;
static ucontext_t g_game_ctx;
static char *g_game_stack;        /* usable stack (guard page excluded) */
static void  *g_game_stack_map;   /* mmap base, including the guard page */
static size_t g_game_stack_map_len;
#endif
static bool g_game_started;
static bool g_game_coroutine_executing;

void ActRaiser_YieldToHost(void) {
#ifdef _WIN32
  SwitchToFiber(g_host_fiber);
#else
  /* swapcontext can fail with ENOMEM ("Insufficient stack space left"). An
   * unchecked failure would silently return and keep running on a stack the
   * host believes it owns; there is no recovery, so abort loudly instead. */
  if (swapcontext(&g_game_ctx, &g_host_ctx) != 0) {
    fprintf(stderr, "FATAL: swapcontext (game -> host) failed\n");
    abort();
  }
#endif
}

/* HLE failures cannot return through a partly executed emulated routine. The
 * fatal module formats the invariant at its owner, then this registered seam
 * latches it and suspends the complete game coroutine. RunOneFrameOfGame sees
 * the latch immediately after the context switch and returns without running
 * NMI or executing any more game state. If the host ever resumes this
 * coroutine, ActRaiserHleFatal aborts rather than returning into invalid CPU
 * state. */
static void ActRaiser_HleFatalEscapeToHost(const char *message) {
  SessionFatal_Request(
      "The emulated game encountered an internal runtime error: %s. Your "
      "latest battery save will be flushed before exit. Restart the game; "
      "if the same event repeats, report the room and active gameplay "
      "settings.",
      message && message[0] ? message : "unspecified HLE invariant failure");
  if (!g_game_coroutine_executing) {
    /* This escape is valid only while RunOneFrameOfGame is blocked in the
     * matching SwitchToFiber/swapcontext. Calling it from host-side NMI/IRQ or
     * a standalone helper would overwrite/switch to the wrong context. */
    fprintf(stderr,
            "FATAL: HLE invariant failed outside the game coroutine\n");
    abort();
  }
  ActRaiser_YieldToHost();
}

/* The recompiler executes the action loader's decompression and bulk graphics
 * copies as one host call. Hardware spends hundreds of display frames doing
 * that CPU work with NMI disabled, leaving the already-started Advent cue
 * playing over a forced-black screen. The inserted time is presentation/audio
 * pacing only: NMI, $0088, timers, and gameplay remain stopped. Pace only the
 * verified $00:843E force-blank write for a non-action -> action transition;
 * ordinary fades, action restarts, and non-action loads keep their existing
 * cadence. */
static unsigned g_action_load_armed_frames;
static unsigned g_action_load_hold_frames;
static uint64_t g_action_load_one_shot_token;
extern volatile int g_sr_in_interrupt;
/* diagnostic.h declares the 2- and 3-argument variants but not this one;
 * both callers below need it, and one of them precedes the local extern
 * that used to be the only declaration. */
extern int sr_block_history(uint32 *out, int max);

void ActRaiser_OnInidispWrite(uint8_t value) {
  uint32_t block = 0;
  (void)sr_block_history(&block, 1);
  unsigned frames = ActionLoadPacing_ForceBlankHoldFrames(
      g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_DestinationMapGroup], block, value);
  if (!frames || ActRaiser_DeveloperFlagEnabled(
                     kActRaiserDeveloperFlag_DisableActionLoadPacing))
    return;

  g_action_load_armed_frames = frames;
  /* The loader's many APU-port polls are statically collapsed into this one
   * host call. Converting their synthetic touch credit into SPC cycles fills
   * the DSP ring and drops roughly five seconds of authentic Advent audio
   * before the host can present it. Begin the calibrated hold at force blank,
   * before those polls: the audio thread advances the sequencer and makes its
   * acknowledgement ready while the black frame is being presented. The
   * loader then resumes without double-advancing the SPC. */
  RtlSetApuCatchupSuppressed(true);
  bool one_shot_completed = false;
  const uint64_t one_shot_token =
      MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
  if (ActionLoadPacing_ShouldReleaseForOneShot(
          frames, one_shot_token, one_shot_token, one_shot_completed)) {
    g_action_load_armed_frames = 0;
    g_action_load_one_shot_token = 0;
    RtlSetApuCatchupSuppressed(false);
    if (ActRaiser_DeveloperFlagEnabled(
            kActRaiserDeveloperFlag_LoadPacingLog)) {
      fprintf(stderr,
              "[load-pace] f=%d block=$%06X dest-group=$%02X: HD "
              "one-shot already complete; skipped %u-frame hold\n",
              snes_frame_counter, block,
              g_ram[kActRaiserWram_DestinationMapGroup], frames);
    }
    return;
  }
  g_action_load_one_shot_token = one_shot_token;
  g_action_load_hold_frames = frames - 1;
  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_LoadPacingLog)) {
    fprintf(stderr,
            "[load-pace] f=%d block=$%06X dest-group=$%02X: holding "
            "forced blank for %u frames; suppressing collapsed APU catch-up\n",
            snes_frame_counter, block,
            g_ram[kActRaiserWram_DestinationMapGroup], frames);
  }
  ActRaiser_YieldToHost();
}

/* Match the action script's $F0 halt command after the force-blank hold has
 * already elapsed. RtlApuWrite invokes this seam before taking the APU lock,
 * so releasing the collapsed-touch gate cannot race the audio callback. */
void ActRaiser_OnApuPortPace(uint8_t port, uint8_t value) {
  const uint32_t ppu_display = RtlGamePpuDisplayState();
  const uint8_t display_control =
      RTL_GAME_PPU_DISPLAY_CONTROL(ppu_display);
  const ActionLoadPacingTriggerDecision decision =
      ActionLoadPacing_EvaluateTrigger(
          g_action_load_armed_frames,
          g_ram[kActRaiserWram_MapGroup], display_control,
          port, value, g_sr_in_interrupt);
  if (decision == kActionLoadPacingTrigger_Ignore)
    return;

  /* The arm and trigger are deliberately two different hardware writes. If
   * the display or game mode moved on between them, reject the now-obviously
   * stale arm instead of turning a later $F0 into a five-second pause. */
  if (decision == kActionLoadPacingTrigger_Discard) {
    if (ActRaiser_DeveloperFlagEnabled(
            kActRaiserDeveloperFlag_LoadPacingLog)) {
      fprintf(stderr,
              "[load-pace] f=%d mode=$%02X/$%02X inidisp=$%02X: "
              "discarded stale arm before APU halt $F0\n",
              snes_frame_counter, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap],
              display_control);
    }
    g_action_load_armed_frames = 0;
    g_action_load_one_shot_token = 0;
    RtlSetApuCatchupSuppressed(false);
    return;
  }

  const unsigned frames = g_action_load_armed_frames;
  g_action_load_armed_frames = 0;
  RtlSetApuCatchupSuppressed(false);
  g_action_load_one_shot_token = 0;
  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_LoadPacingLog)) {
    fprintf(stderr,
            "[load-pace] f=%d action mode=$%02X/$%02X: completed %u-frame "
            "forced-blank hold; releasing APU halt $F0\n",
            snes_frame_counter, g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap], frames);
  }
}

/* Keep ActRaiser's data-driven object-loop recovery policy out of the shared
 * runtime. The shared dispatcher owns the generic BRA/BRL-follow mechanism;
 * this project opts in only at the two ROM sites whose stack contract has
 * been verified. */
bool ActRaiser_RecoverDispatchMiss(uint32 source_pc24, uint32 target_pc24) {
  (void)target_pc24;
  return source_pc24 == 0x008965u || source_pc24 == 0x008966u;
}

/* ActRaiser's inline RDNMI waits need coroutine pacing at a ROM-specific set
 * of basic blocks. Returning -1 delegates ordinary reads to the shared SNES
 * hardware model; a nonnegative result overrides the $4210 byte. */
int ActRaiser_ReadRdnmi(const RtlRdnmiReadContext *context) {
  extern uint32_t g_sr_block_ring[];
  extern unsigned g_sr_block_index;
  static bool yielding;
  if (!context || context->struct_size < RTL_RDNMI_READ_CONTEXT_V2_SIZE)
    return -1;
  const bool force_nmi =
      (context->flags & RTL_RDNMI_FORCE_NMI) != 0u;
  const bool in_nmi = (context->flags & RTL_RDNMI_IN_NMI) != 0u;
  const bool nmi_available =
      (context->flags & RTL_RDNMI_AVAILABLE) != 0u;

  /* If the same block reads $4210 thousands of times without another traced
   * block between reads, print the gate state once instead of leaving only a
   * generic watchdog failure. */
  {
    static uint32_t wedge_blk, wedge_n;
    static unsigned wedge_idx;
    unsigned idx = g_sr_block_index;
    uint32_t block = g_sr_block_ring[
        (g_sr_block_index - 1) & kRuntimeBlockTraceRingMask];
    if (block == wedge_blk && (idx == wedge_idx || idx == wedge_idx + 1)) {
      if (++wedge_n == kRdnmiRepeatedReadWarningThreshold) {
        fprintf(stderr,
                "[4210-wedge] blk=$%06X f=%d x%u consecutive reads; "
                "forceNmi=%d yielding=%d inNmi=%d nmiAvail=%d\n",
                block, snes_frame_counter,
                (unsigned)kRdnmiRepeatedReadWarningThreshold,
                force_nmi ? 1 : 0,
                yielding ? 1 : 0, in_nmi ? 1 : 0,
                nmi_available ? 1 : 0);
        fflush(stderr);
      }
    } else {
      wedge_blk = block;
      wedge_n = 1;
    }
    wedge_idx = idx;
  }

  /* These verified spin blocks can also execute from an interrupt context,
   * where yielding is impossible. Report vblank immediately in that case so
   * the emulated handler cannot deadlock inside its own wait. */
  if (!(force_nmi && !yielding)) {
    static const uint32_t kSpinBlocksNoYield[] = {
      0x019293, 0x0192AA, 0x0287F3, 0x029AC4,
      0x02BEBF, 0x03B013, 0x03E535,
    };
    uint32_t block = g_sr_block_ring[
        (g_sr_block_index - 1) & kRuntimeBlockTraceRingMask];
    for (unsigned i = 0;
         i < sizeof(kSpinBlocksNoYield) / sizeof(kSpinBlocksNoYield[0]); i++) {
      if (block == kSpinBlocksNoYield[i]) {
        static int warned;
        if (!warned) {
          warned = 1;
          fprintf(stderr,
                  "[4210] non-yieldable-context spin at $%06X f=%d "
                  "-> fast-exit (bit7=1, unpaced)\n",
                  block, snes_frame_counter);
        }
        return 0x82;
      }
    }
  }

  /* Latched: ActRaiser_ReadRdnmi runs on EVERY $4210 read, and the game polls
   * that register inside spin loops -- potentially thousands of times a frame,
   * not once. getenv is ~150ns here, so an unlatched read costs up to a few
   * percent of the frame budget at spin-loop rates for a switch that cannot
   * change mid-run. Recurring developer diagnostics are likewise snapshotted
   * by ActRaiser_GetDeveloperEnvironment before the first emulated frame. */
  static int no_4210_yield = -1;
  if (no_4210_yield < 0) no_4210_yield = getenv("AR_NO4210YIELD") ? 1 : 0;
  if (force_nmi && !yielding && !no_4210_yield) {
    static const uint32_t kSpinBlocks[] = {
      0x019293, /* intro/menu/effect wait */
      0x0192AA, /* effect-loop wait */
      0x0287F3, /* fade/transition helper */
      0x029AC4, /* boot sound-init wait */
      0x02BEBF, /* sound-code wait */
      0x03B013, /* long-form wait */
      0x03E535, /* sound-upload bracket wait */
    };
    uint32_t block = g_sr_block_ring[
        (g_sr_block_index - 1) & kRuntimeBlockTraceRingMask];
    for (unsigned i = 0; i < sizeof(kSpinBlocks) / sizeof(kSpinBlocks[0]); i++) {
      if (block != kSpinBlocks[i])
        continue;
      if (ActRaiser_DeveloperFlagEnabled(
              kActRaiserDeveloperFlag_VblankLog)) {
        static int last_frame = -1;
        if (snes_frame_counter != last_frame) {
          last_frame = snes_frame_counter;
          extern uint16 sr_cpu_stack_pointer(void);
          extern uint8 sr_cpu_program_bank(void);
          const uint32_t ppu_display = RtlGamePpuDisplayState();
          const uint8_t display_control =
              RTL_GAME_PPU_DISPLAY_CONTROL(ppu_display);
          fprintf(stderr,
                  "[vbl] f=%d bright=%d fblank=%d bgmode=%02x main=%02x "
                  "$18=%02x $19=%02x time$E6=%02x%02x HP=%02x PB=%02x "
                  "S=%04x blk=%06X\n",
                  snes_frame_counter, display_control & 0xf,
                  (display_control & 0x80) ? 1 : 0,
                  RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display),
                  RTL_GAME_PPU_MAIN_SCREEN(ppu_display),
                  g_ram[kActRaiserWram_MapGroup],
                  g_ram[kActRaiserWram_CurrentMap],
                  g_ram[kActRaiserWram_ActionTimerHigh],
                  g_ram[kActRaiserWram_ActionTimerLow],
                  g_ram[kActRaiserWram_PlayerHp], sr_cpu_program_bank(),
                  sr_cpu_stack_pointer(), block);
        }
      }
      yielding = true;
      ActRaiser_YieldToHost();
      yielding = false;
      return 0x82;
    }
    /* Clear/post/ack reads do not yield and report no vblank. */
    return 0x02;
  }

  return -1;
}

/* A 65816 hardware interrupt is register-transparent to the interrupted
 * code: RTI restores P/PC/PB and a well-behaved handler save/restores
 * A/X/Y/D/DB. We invoke the recompiled NMI/IRQ handlers as plain host-C
 * calls on the shared g_cpu; if a handler body has an internal stack
 * imbalance (e.g. an x-width mismatch) its terminal RTI can pop the
 * wrong byte as P and corrupt the interrupted code's M/X width flags.
 * Snapshot the CPU register frame before the handler and restore it
 * after — the handler's RAM/PPU side effects (the point of the IRQ)
 * persist in g_ram/g_ppu, only the CPU registers are made transparent. */
typedef struct { uint16 A, X, Y, S, D; uint8 DB, PB, P, m_flag, x_flag,
  emulation, host_return_valid, fN, fV, fZ, fC, fI, fD; } CpuRegSnapshot;

static void ActRaiser_SaveRegs(CpuState *c, CpuRegSnapshot *s) {
  s->A = c->A; s->X = c->X; s->Y = c->Y; s->S = c->S; s->D = c->D;
  s->DB = c->DB; s->PB = c->PB; s->P = c->P; s->m_flag = c->m_flag;
  s->x_flag = c->x_flag; s->emulation = c->emulation;
  s->host_return_valid = c->host_return_valid;
  s->fN = c->_flag_N; s->fV = c->_flag_V; s->fZ = c->_flag_Z;
  s->fC = c->_flag_C; s->fI = c->_flag_I; s->fD = c->_flag_D;
}

static void ActRaiser_RestoreRegs(CpuState *c, const CpuRegSnapshot *s) {
  c->A = s->A; c->X = s->X; c->Y = s->Y; c->S = s->S; c->D = s->D;
  c->DB = s->DB; c->PB = s->PB; c->P = s->P; c->m_flag = s->m_flag;
  c->x_flag = s->x_flag; c->emulation = s->emulation;
  c->host_return_valid = s->host_return_valid;
  c->_flag_N = s->fN; c->_flag_V = s->fV; c->_flag_Z = s->fZ;
  c->_flag_C = s->fC; c->_flag_I = s->fI; c->_flag_D = s->fD;
}

/* Set while an NMI/IRQ handler is executing on the host stack (the calls
 * below are bracketed by SaveRegs/RestoreRegs, so cpu->S is restored after).
 * The stack-drift tripwire reads this to ignore handler-internal imbalance. */
volatile int g_sr_in_interrupt = 0;

static uint32 ActRaiser_LastBlockPc(void);

static void ActRaiser_EmitInterrupt(SrInterruptKind kind, uint32 flags,
                                    uint32 pc24, uint16 vector,
                                    int32 scanline, const char *label) {
  RtlGameEmitInterrupt(kind, flags, pc24, vector, scanline, label);
}

/* ActRaiser BRK syscall. The ROM's BRK vector ($00:852F) is:
 *   PHP; SEP #$20; STA $00035B; PLP; RTI
 * i.e. it stores A's low byte to $035B (the sound-effect request port) and
 * resumes at PC+2 — registers/flags otherwise unchanged. The game uses
 * `LDA #id; BRK` as a compact "play sound id" call throughout (e.g. enemy-death
 * SFX in the object/OAM loops). Generated code invokes this at every BRK site
 * via g_cpu_brk_hook, then falls through to the next instruction. */
static void ActRaiser_BrkHook(CpuState *cpu) {
  const uint8 id = (uint8)(cpu->A & 0xFF);
  extern const char *g_last_recomp_func;
  const uint32 site = ActRaiser_LastBlockPc();
  const uint16 vector = cpu->emulation ? 0xfffeu : 0xffe6u;
  const bool observe_interrupt =
      RtlGameEventEnabled(SR_EVENT_MASK_INTERRUPT);
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_BRK, SR_EVENT_INTERRUPT_ENTER, site, vector,
        SR_INTERRUPT_SCANLINE_UNKNOWN, "brk");
  }
  const uint32 game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  const uint64_t trace_serial = NativeAudioTrace_OnCpuRequest(
      kNativeAudioRequest_Sfx, id, true, g_last_recomp_func,
      site, game_frame,
      (uint16_t)cpu->X, (uint16_t)cpu->Y);
  const bool extended = NativeAudioExtension_QueueRequest(
      false, id, site, game_frame, (uint16_t)cpu->X, (uint16_t)cpu->Y,
      trace_serial);
  if (!extended)
    cpu_write8(cpu, 0x00, kActRaiserWram_BrkSoundRequest, id);
  /* AR_SFXCENSUS=1: record the request with its caller and the index registers
   * that identify the requesting actor, so the census can join it to whatever
   * sample the SPC driver ends up keying. No-op when disabled. */
  {
    SfxCensus_OnRequest(id, g_last_recomp_func,
                        ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                        (uint16_t)cpu->X, (uint16_t)cpu->Y);
  }
  /* AR_COPLOG=1: also log BRK (sound-request) posts, for contrast against COP
   * event posts below -- lets a stuck-state capture show whether the game is
   * still alive and posting routine SFX while a specific event id never posts. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_CopLog)) {
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    fprintf(stderr, "[brk] gf=%u fn=%s id=%02x $18=%02x $19=%02x\n",
            game_frame, g_last_recomp_func ? g_last_recomp_func : "?",
            id, g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap]);
  }
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_BRK, SR_EVENT_INTERRUPT_EXIT, site, vector,
        SR_INTERRUPT_SCANLINE_UNKNOWN, "brk");
  }
}

/* Return the most recent recompiled block PC. The always-on ring is also used
 * by crash diagnostics; consulting it here lets a user-facing sound toggle
 * distinguish the dialogue composer's COP #$07 from unrelated uses of id 07. */
static uint32 ActRaiser_LastBlockPc(void) {
  uint32 pc = 0;
  return sr_block_history(&pc, 1) == 1 ? pc : 0;
}

/* ActRaiser COP syscall — the SECOND software interrupt, structurally identical
 * to BRK. The ROM's COP vector ($00:FFE4 -> $8526) is:
 *   PHP; SEP #$20; STA $00035A; PLP; RTI
 * i.e. it stores A's low byte to $035A (a request port distinct from BRK's
 * $035B) and resumes at PC+2. The game posts events via `LDA #id; COP` — e.g.
 * the post-miniboss platform/event trigger does `LDA #$07; COP`. Without this
 * hook g_cpu_cop_hook stayed NULL, so every COP was an effect-free continue and
 * $035A was never written → the event/platform never fired. Symmetric to the
 * BRK hook; found via the oracle writing $035A 90x while the recomp wrote it 0x. */
static void ActRaiser_CopHook(CpuState *cpu) {
  const uint8 id = (uint8)(cpu->A & 0xFF);
  const uint32 site = ActRaiser_LastBlockPc();
  const uint16 vector = cpu->emulation ? 0xfff4u : 0xffe4u;
  const bool observe_interrupt =
      RtlGameEventEnabled(SR_EVENT_MASK_INTERRUPT);
  extern const char *g_last_recomp_func;
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_COP, SR_EVENT_INTERRUPT_ENTER, site, vector,
        SR_INTERRUPT_SCANLINE_UNKNOWN, "cop");
  }
  /* $01:901C is the message composer's per-glyph pacing helper. Its
   * non-space path at $01:902D posts COP #$07 after drawing each character.
   * Suppress only this exact site: id 07 also drives unrelated game events. */
  const bool suppress_dialog_blip =
      !AudioPresentationPolicy_ShouldEmitDialogBlip(
          g_settings.audio_dialog_blip) &&
      id == 0x07 && site == 0x01902D;
  const uint64_t trace_serial = NativeAudioTrace_OnCpuRequest(
      kNativeAudioRequest_Event, id, !suppress_dialog_blip,
      g_last_recomp_func, site,
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
      (uint16_t)cpu->X, (uint16_t)cpu->Y);
  const bool extended = !suppress_dialog_blip &&
      NativeAudioExtension_QueueRequest(
          true, id, site,
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
          (uint16_t)cpu->X, (uint16_t)cpu->Y, trace_serial);
  if (!suppress_dialog_blip && !extended)
    cpu_write8(cpu, 0x00, kActRaiserWram_CopRequest, id);
  /* AR_COPLOG=1: log every COP-posted event id + game-frame + calling recomp
   * function, so a Death-Heim stuck-state capture shows whether the
   * boss-defeat/next-encounter event ever posts at all, vs posting an id whose
   * consumer is unreached (see [[cop-syscall-hook-fix]] -- $C3DA consumer was
   * previously suspected still-unreached for a different event id). */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_CopLog)) {
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    fprintf(stderr, "[cop] gf=%u fn=%s site=%06x id=%02x%s $18=%02x $19=%02x\n",
            game_frame, g_last_recomp_func ? g_last_recomp_func : "?",
            site, id, suppress_dialog_blip ? " suppressed-dialog-blip" : "",
            g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap]);
  }
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_COP, SR_EVENT_INTERRUPT_EXIT, site, vector,
        SR_INTERRUPT_SCANLINE_UNKNOWN, "cop");
  }
}

static void ActRaiser_WritePpuSnapshotMetadata(
    const char *prefix, const SrPpuStateSnapshot *ppu) {
  if (!prefix || !ppu) return;
  char path[384];
  snprintf(path, sizeof path, "%s.ppu.json", prefix);
  FILE *file = fopen(path, "w");
  if (!file) return;
  fprintf(file,
          "{\n"
          "  \"format\": 1,\n"
          "  \"inidisp\": %u, \"bgmode\": %u, \"mosaic\": %u,\n"
          "  \"bg_sc\": [%u, %u, %u, %u],\n"
          "  \"bg_tile_adr\": %u,\n"
          "  \"hscroll\": [%u, %u, %u, %u],\n"
          "  \"vscroll\": [%u, %u, %u, %u],\n"
          "  \"screen_main\": %u, \"screen_sub\": %u,\n"
          "  \"window_main\": %u, \"window_sub\": %u,\n"
          "  \"windowsel\": %u, \"wbgobjlog\": %u,\n"
          "  \"window_edges\": [%u, %u, %u, %u],\n"
          "  \"cgwsel\": %u, \"cgadsub\": %u, \"setini\": %u,\n"
          "  \"widescreen\": {\"left\": %u, \"right\": %u, "
          "\"top\": %u, \"bottom\": %u, \"clamp\": %u, "
          "\"mirror\": %u, \"repeat\": %u}\n"
          "}\n",
          (unsigned)ppu->display_control,
          (unsigned)ppu->bg_mode_control,
          (unsigned)ppu->mosaic_control,
          (unsigned)ppu->background_tilemap_control[0],
          (unsigned)ppu->background_tilemap_control[1],
          (unsigned)ppu->background_tilemap_control[2],
          (unsigned)ppu->background_tilemap_control[3],
          (unsigned)ppu->background_tile_base_control,
          (unsigned)ppu->backgrounds[0].h_scroll,
          (unsigned)ppu->backgrounds[1].h_scroll,
          (unsigned)ppu->backgrounds[2].h_scroll,
          (unsigned)ppu->backgrounds[3].h_scroll,
          (unsigned)ppu->backgrounds[0].v_scroll,
          (unsigned)ppu->backgrounds[1].v_scroll,
          (unsigned)ppu->backgrounds[2].v_scroll,
          (unsigned)ppu->backgrounds[3].v_scroll,
          (unsigned)ppu->main_screen,
          (unsigned)ppu->sub_screen,
          (unsigned)ppu->main_windowed,
          (unsigned)ppu->sub_windowed,
          (unsigned)ppu->window_select, (unsigned)ppu->window_logic,
          (unsigned)ppu->window1_left, (unsigned)ppu->window1_right,
          (unsigned)ppu->window2_left, (unsigned)ppu->window2_right,
          (unsigned)ppu->color_math_control,
          (unsigned)ppu->color_math_designation,
          (unsigned)ppu->setini_control,
          (unsigned)ppu->margin_left, (unsigned)ppu->margin_right,
          (unsigned)ppu->margin_top, (unsigned)ppu->margin_bottom,
          (unsigned)ppu->layer_clamp_mask,
          (unsigned)ppu->layer_mirror_mask,
          (unsigned)ppu->layer_repeat_mask);
  fclose(file);
}

static bool ActRaiser_QueryInputState(SrInputStateSnapshot *state) {
  if (!state || !s_runner || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_INPUT_STATE_SIZE ||
      (s_runner_api->capabilities & SR_RUNNER_CAP_INPUT_STATE) == 0u ||
      !s_runner_api->query_input_state)
    return false;
  *state = (SrInputStateSnapshot){
    .struct_size = sizeof(*state),
  };
  return s_runner_api->query_input_state(s_runner, state) ==
      SR_RESULT_OK;
}

/* Dump the full internal state (everything but the framebuffer, which the
 * caller writes as a .ppm) so an on-demand snapshot captures both the picture
 * AND the internals: WRAM, plus the PPU memory the WRAM dump can't see — VRAM
 * (BG tilemaps + tiles), CGRAM (palette), OAM (sprites). Critical for the
 * bridge bug, whose tiles live in VRAM, invisible to any WRAM-only diff.
 * Writes <prefix>.{wram,vram,cgram,oam}.bin plus PPU register metadata. */
void ActRaiser_FullSnapshot(const char *prefix) {
  SrPpuStateSnapshot ppu_state;
  SrBorrowedU16Span vram = {.struct_size = sizeof(vram)};
  SrBorrowedU16Span cgram = {.struct_size = sizeof(cgram)};
  SrBorrowedU16Span oam = {.struct_size = sizeof(oam)};
  SrBorrowedSpan high_oam = {.struct_size = sizeof(high_oam)};
  char path[384];
  FILE *f;
  snprintf(path, sizeof path, "%s.wram.bin", prefix);
  f = fopen(path, "wb");
  if (f) { fwrite(g_ram, 1, kActRaiserWramSize, f); fclose(f); }
  if (s_runner && s_runner_api &&
      ActRaiser_QueryPpuState(&ppu_state) &&
      s_runner_api->borrow_u16_memory &&
      s_runner_api->borrow_memory &&
      s_runner_api->borrow_u16_memory(
          s_runner, SR_MEMORY_VRAM, &vram) == SR_RESULT_OK &&
      s_runner_api->borrow_u16_memory(
          s_runner, SR_MEMORY_CGRAM, &cgram) == SR_RESULT_OK &&
      s_runner_api->borrow_u16_memory(
          s_runner, SR_MEMORY_OAM, &oam) == SR_RESULT_OK &&
      s_runner_api->borrow_memory(
          s_runner, SR_MEMORY_HIGH_OAM, &high_oam) == SR_RESULT_OK) {
    snprintf(path, sizeof path, "%s.vram.bin", prefix);
    f = fopen(path, "wb");
    if (f) { fwrite(vram.data, sizeof(*vram.data),
                    vram.element_count, f); fclose(f); }
    snprintf(path, sizeof path, "%s.cgram.bin", prefix);
    f = fopen(path, "wb");
    if (f) { fwrite(cgram.data, sizeof(*cgram.data),
                    cgram.element_count, f); fclose(f); }
    snprintf(path, sizeof path, "%s.oam.bin", prefix);
    f = fopen(path, "wb");
    if (f) { fwrite(oam.data, sizeof(*oam.data),
                    oam.element_count, f); fclose(f); }
    /* The HIGH table, as its own file so the 512-byte .oam.bin layout every
     * existing parser assumes stays exactly that. Without it a snapshot cannot
     * place or size a sprite at all: the high table carries each slot's X bit 8
     * and its size-large bit, so an OAM-only dump silently reads every sprite as
     * small and as x = (x & 0xff). That is a 256-pixel ambiguity — a sprite
     * staged off the RIGHT edge decodes as one sitting mid-screen. Cost a
     * diagnosis on 2026-08-05 (an off-screen staged sprite revealed by the
     * diorama vertical band could not be located from its snapshot). */
    snprintf(path, sizeof path, "%s.highoam.bin", prefix);
    f = fopen(path, "wb");
    if (f) { fwrite(high_oam.data, 1, high_oam.byte_size, f); fclose(f); }
    ActRaiser_WritePpuSnapshotMetadata(prefix, &ppu_state);
  }
}

static void game_coroutine(void) {
  g_action_load_armed_frames = 0;
  g_action_load_hold_frames = 0;
  g_action_load_one_shot_token = 0;
  RtlSetApuCatchupSuppressed(false);
  cpu_state_init(&g_cpu, g_ram);
  g_cpu_brk_hook = ActRaiser_BrkHook;
  g_cpu_cop_hook = ActRaiser_CopHook;
  ResetHandler_M1X1(&g_cpu);
  for (;;)
    ActRaiser_YieldToHost();
}

#ifdef _WIN32
static VOID CALLBACK game_coroutine_fiber(LPVOID param) {
  (void)param;
  game_coroutine();   /* never returns: for(;;) ActRaiser_YieldToHost() */
}
#endif

RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {
  /* A85E (and the identical $00:8418) are HLE'd to this function. The real ROM
   * routine is PHP / SEP #$20 / PHA / {spin on $4210 bit 7} / PLA / PLP / RTS —
   * internally stack-neutral, and its terminating RTS pops the 2-byte return
   * frame the caller's JSR pushed. This HLE replaces the whole routine with a
   * host yield, so unless we emulate that RTS the caller's frame is orphaned on
   * the SNES stack: a 2-byte/call leak that, over a long wait loop, marches S
   * down out of page 1 into zero-page and clobbers game variables (the old
   * AF86/$2100 open-bus crash). So pop the frame here to keep S balanced, which
   * is exactly what the hardware routine does. */
  /* Latched for the same reason: this runs on every vblank yield. */
  static int no_pop = -1;
  if (no_pop < 0) no_pop = getenv("AR_NOPOP") ? 1 : 0;
  if (!no_pop) cpu->S = (uint16)(cpu->S + 2);

  /* AR_YIELDLOG=1: dump the recomp call stack + SNES return address at each
   * vblank yield to see what the main thread is doing frame to frame. Read the
   * return frame from the PRE-pop S (sp-2, since we already added 2 above). */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_YieldLog)) {
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    int top = g_recomp_stack_top;
    fprintf(stderr, "[yield] f=%d S=%04x A=%04x P=%02x depth=%d:",
            snes_frame_counter, cpu->S, cpu->A, cpu->P, top);
    for (int i = top - 1; i >= 0 && i >= top - 6; i--)
      fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    uint16 sp = no_pop ? cpu->S : (uint16)(cpu->S - 2);
    uint16 rlo = g_ram[(uint16)(sp + 1)];
    uint16 rhi = g_ram[(uint16)(sp + 2)];
    fprintf(stderr, " ret~%02x:%04x\n", cpu->PB, (uint16)(((rhi << 8) | rlo) + 1));
  }

  /* AR_FORCE18=<hex>: experimentally pin $7E0018 (game-mode byte) before the
   * next NMI's ABF0 branch, to test whether a non-zero game-mode unsticks the
   * frozen title (state machine + menu decompression). Diagnostic only. */
  {
    static int forced_map_group = -2;
    if (forced_map_group == -2) {
      const char *value = getenv("AR_FORCE18");
      forced_map_group = value ? (int)strtoul(value, NULL, 0) : -1;
    }
    if (forced_map_group >= 0)
      g_ram[kActRaiserWram_MapGroup] = (uint8)forced_map_group;
  }

  /* AR_FRAMELOG=1: at each vblank yield, report how much game code ran since the
   * previous yield (push delta) plus the key action-engine RAM bytes. A large,
   * steady push delta with $E6 (time) ticking = engine running. A tiny push delta
   * = the main loop is spinning on the vblank wait WITHOUT running per-frame logic
   * (dispatch/gate problem). $E6 frozen while pushes are large = logic runs but a
   * pause/timer gate is suppressing advancement. Action fields also expose the
   * actual movement result: position delta, velocity, current player handler/
   * flags, and the walking-cycle Crest/Boost counters ($08BC/$08C4). */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_FrameLog)) {
    extern unsigned long g_recomp_push_count;
    static unsigned long last_push;
    /* return frame is at pre-pop S (we already did S+=2 above) */
    uint16 sp = (uint16)(cpu->S - 2);
    uint16 ret = (uint16)(((g_ram[(uint16)(sp + 2)] << 8) | g_ram[(uint16)(sp + 1)]) + 1);
    /* joypad raw + SwapInputBits'd (same order AR_MOONJUMP reads) -- added
     * 2026-07-01 for the sim-mode freeze investigation: correlates whether
     * input is even reaching the frame against which per-frame path fires
     * (see AR_SIMTRACE in snesrecomp/game/trace.h). */
    SrInputStateSnapshot input;
    const bool input_valid = ActRaiser_QueryInputState(&input);
    uint16 joy_raw = input_valid ? input.packed_buttons[0] : 0u;
    uint16 joy = input_valid ? input.auto_joypad[0] : 0u;
    uint16 game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    uint16 player_x = ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionX);
    uint16 player_y = ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY);
    int16 player_velocity_x = (int16)ActRaiser_ReadWram16(
        kActRaiserWram_PlayerVelocityX);
    int16 player_velocity_y = (int16)ActRaiser_ReadWram16(
        kActRaiserWram_PlayerVelocityY);
    uint16 player_handler = ActRaiser_ReadWram16(
        kActRaiserWram_PlayerHandler);
    uint16 player_flags = ActRaiser_ReadWram16(kActRaiserWram_PlayerFlags);
    const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
    const uint8 current_map = g_ram[kActRaiserWram_CurrentMap];
    static uint16 last_player_x, last_player_y;
    static uint8 last_map_group, last_map;
    int delta_x = 0, delta_y = 0;
    if (ActRaiser_IsActionMapGroup(map_group) &&
        last_map_group == map_group && last_map == current_map) {
      delta_x = (int16)(player_x - last_player_x);
      delta_y = (int16)(player_y - last_player_y);
    }
    fprintf(stderr,
      "[frame] f=%d gf=%u push+%lu callsite=%02x:%04x A=%04x m=%d $18=%02x $19=%02x $1A=%02x $1B=%02x $F4=%02x $F5=%02x $FB=%02x time$E6=%02x%02x HP$1D=%02x joy=%04x(raw=%04x) pos=%04x,%04x d=%+d,%+d vel=%+d,%+d h=%04x state=%04x boost=%02x crest=%02x\n",
      snes_frame_counter, game_frame, g_recomp_push_count - last_push,
      cpu->PB, ret,
      cpu->A, cpu->m_flag,
      map_group, current_map, g_ram[kActRaiserWram_DestinationMap],
      g_ram[kActRaiserWram_DestinationMapGroup],
      g_ram[kActRaiserWram_InputEnableMask],
      g_ram[kActRaiserWram_InputEnableMask + 1],
      g_ram[kActRaiserWram_TransitionRequest],
      g_ram[kActRaiserWram_ActionTimerHigh],
      g_ram[kActRaiserWram_ActionTimerLow],
      g_ram[kActRaiserWram_PlayerHp], joy, joy_raw, player_x, player_y,
      delta_x, delta_y, player_velocity_x, player_velocity_y,
      player_handler, player_flags,
      g_ram[kActRaiserWram_PlayerBoost],
      g_ram[kActRaiserWram_PlayerCrest]);
    last_player_x = player_x;
    last_player_y = player_y;
    last_map_group = map_group;
    last_map = current_map;
    last_push = g_recomp_push_count;
  }

  /* AR_OBJLOG=1: per-frame action-stage object-table + timer health. Logs the
   * game-frame, timer ($E6/$E7), player HP ($1D), and the first few object
   * slots' status word ($06A0 stride $40) + handler ptr ($12). Reveals the
   * exact frame the object table is wiped / timer goes non-BCD (the "sprites
   * vanish + timer '?'" corruption). */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_ObjectLog)) {
    if (ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
      enum {
        kObjectLogSampleCount = 24,
        kActionObjectHandlerOffset = 0x12,
        kActionObjectDisabled = 0x4000,
      };
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      int active_objects = 0;
      for (int i = 0; i < kObjectLogSampleCount; i++) {
        uint16 object_address = (uint16)(
            kActRaiserWram_ActionObjectTable +
            i * kActRaiserActionObjectStride);
        uint16 status = ActRaiser_ReadWram16(object_address);
        if (!(status & kActRaiserObjectStatus_End) &&
            !(status & kActionObjectDisabled)) {
          active_objects++;
        }
      }
      unsigned first_status = ActRaiser_ReadWram16(
          kActRaiserWram_ActionObjectTable);
      unsigned first_handler = ActRaiser_ReadWram16((uint16)(
          kActRaiserWram_ActionObjectTable + kActionObjectHandlerOffset));
      fprintf(stderr, "[obj] gf=%u timer=%02x%02x HP=%02x active=%d obj0.sw=%04x obj0.h=%04x\n",
              game_frame, g_ram[kActRaiserWram_ActionTimerHigh],
              g_ram[kActRaiserWram_ActionTimerLow],
              g_ram[kActRaiserWram_PlayerHp], active_objects,
              first_status, first_handler);
    }
  }

  /* AR_PPULOG=1: per-frame display state — INIDISP (brightness + forced-blank),
   * BG mode, and main/sub screen layer-enable masks. A black screen with the
   * game running (no freeze) is usually forced-blank set, brightness 0, or all
   * main-screen layers disabled. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_PpuLog)) {
    static int lf = -1;
    if (snes_frame_counter != lf) {
      lf = snes_frame_counter;
      const uint32_t ppu_display = RtlGamePpuDisplayState();
      const uint8_t display_control =
          RTL_GAME_PPU_DISPLAY_CONTROL(ppu_display);
      fprintf(stderr, "[ppu] f=%d inidisp=%02x bright=%d fblank=%d bgmode=%02x main=%02x sub=%02x hdmaen=%02x\n",
              snes_frame_counter, display_control, display_control & 0xf,
              (display_control & 0x80) ? 1 : 0,
              RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display),
              RTL_GAME_PPU_MAIN_SCREEN(ppu_display),
              RTL_GAME_PPU_SUB_SCREEN(ppu_display),
              ActRaiser_QueryHdmaActiveMask());
    }
  }

  ActRaiser_YieldToHost();
  return RECOMP_RETURN_NORMAL;
}

/* $02:BC56 selects the next animated BG-character frame and arms NMI DMA
 * descriptor 1. The native game assumes a force-blanked graphics load will
 * finish before another animation tick. In the recomp, an SPC command ack can
 * keep the main coroutine in $02:B63B for several host frames while NMI keeps
 * running. A tick in that window uploads the not-yet-captured $7F:B800 frame
 * over VRAM $0000; $02:BAF5 then captures that blank page and makes the
 * corruption self-perpetuating.
 *
 * Preserve BC56's native behavior and register/flag contract, except that an
 * invisible tick is deferred while INIDISP force-blank is active. Once the
 * loader clears force-blank, animation resumes from the same phase with all
 * four frames already captured. */
static void ActRaiser_TileAnimSetNz(CpuState *cpu, uint16 value,
                                    unsigned bits) {
  const uint16 sign = bits == 8 ? 0x0080 : 0x8000;
  const uint16 mask = bits == 8 ? 0x00FF : 0xFFFF;
  value &= mask;
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & sign) != 0;
}

static uint16 ActRaiser_TileAnimAdc16(CpuState *cpu, uint16 left,
                                     uint16 right) {
  const Cpu65816Add16Result addition = Cpu65816_Add16(
      left, right, cpu->_flag_C != 0, cpu->_flag_D != 0);
  const uint16 result = addition.value;
  cpu->_flag_C = addition.carry;
  cpu->_flag_V = addition.overflow;
  ActRaiser_TileAnimSetNz(cpu, result, 16);
  cpu_write_a16(cpu, result);
  return result;
}

RecompReturn ActRaiser_TileAnimationTick(CpuState *cpu) {
  const unsigned entry_m_bits = cpu->m_flag ? 8 : 16;
  const uint16 entry_m_mask = cpu->m_flag ? 0x00FF : 0xFFFF;
  const uint16 frame = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x0088))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x0088));
  const uint16 period = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00DE))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00DE));
  const uint16 due = (frame & period) & entry_m_mask;
  cpu_write_a_m(cpu, due);
  ActRaiser_TileAnimSetNz(cpu, due, entry_m_bits);

  if (due != 0 ||
      (RTL_GAME_PPU_DISPLAY_CONTROL(RtlGamePpuDisplayState()) & 0x80u) != 0u) {
    cpu_mirrors_to_p(cpu);
    cpu->S = (uint16)(cpu->S + 3);  /* replaced RTL */
    return RECOMP_RETURN_NORMAL;
  }

  const uint16 phase_word = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E0));
  const uint16 phase_mask = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00DF))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00DF));
  const uint16 phase_plus_one =
      (uint16)(((phase_word & phase_mask) + 1) & entry_m_mask);
  cpu_write_a_m(cpu, phase_plus_one);
  ActRaiser_TileAnimSetNz(cpu, phase_plus_one, entry_m_bits);
  cpu_write_x_x(cpu, phase_plus_one);
  ActRaiser_TileAnimSetNz(cpu, cpu_read_x_x(cpu), cpu->x_flag ? 8 : 16);

  cpu_mirrors_to_p(cpu);
  cpu->P |= CPU_P_X;               /* SEP #$10 */
  cpu_p_to_mirrors(cpu);
  cpu->X &= 0x00FF;
  cpu_mirrors_to_p(cpu);
  cpu->P &= (uint8)~CPU_P_M;       /* REP #$20 */
  cpu_p_to_mirrors(cpu);

  cpu_write_a16(cpu, 0);
  ActRaiser_TileAnimSetNz(cpu, 0, 16);
  for (;;) {
    cpu_write_x8(cpu, (uint8)(cpu_read_x8(cpu) - 1));
    ActRaiser_TileAnimSetNz(cpu, cpu_read_x8(cpu), 8);
    if (cpu_read_x8(cpu) == 0) break;
    cpu->_flag_C = 0;
    ActRaiser_TileAnimAdc16(
        cpu, cpu_read_a16(cpu),
        cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E1)));
  }

  cpu->_flag_C = 0;
  const uint16 source =
      ActRaiser_TileAnimAdc16(cpu, cpu_read_a16(cpu), 0xB800);
  cpu_write16(cpu, 0x7E, (uint16)(cpu->D + 0x00D7), source);

  cpu_mirrors_to_p(cpu);
  cpu->P |= CPU_P_M;               /* SEP #$20 */
  cpu_p_to_mirrors(cpu);
  cpu_mirrors_to_p(cpu);
  cpu->P &= (uint8)~CPU_P_X;       /* REP #$10 */
  cpu_p_to_mirrors(cpu);

  cpu_write_x16(cpu,
                cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E1)));
  ActRaiser_TileAnimSetNz(cpu, cpu_read_x16(cpu), 16);
  cpu_write16(cpu, 0x7E, (uint16)(cpu->D + 0x00DC), cpu_read_x16(cpu));

  const uint8 next_phase =
      (uint8)(cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0)) + 1);
  cpu_write8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0), next_phase);
  ActRaiser_TileAnimSetNz(cpu, next_phase, 8);
  cpu_mirrors_to_p(cpu);

  cpu->S = (uint16)(cpu->S + 3);  /* replaced RTL */
  return RECOMP_RETURN_NORMAL;
}

/* Per-frame widescreen policy — the single seam where game mode decides how
 * much of the extra-column budget (g_ws_extra, set at startup from
 * ExtendedAspectRatio) is visible this frame. Phase 1: pillarbox everywhere
 * (authentic 256 columns centered); later phases widen per mode via
 * $18/$19 and clamp per camera/level bounds in the frame-policy transaction.
 * Must run every frame: ppu_reset zeroes the PPU margin fields.
 * AR_WS_SURVEY=1 forces raw symmetric margins in EVERY mode — the Phase-2
 * artifact-survey knob (stale tiles/pop-in expected; not for normal play). */
enum {
  kNoActionBgPlanSource = -1,
};

/* ApplyWidescreenPolicy resolves these before scanout; the draw tail promotes
 * them into the live latch beside the exact margins after the pixels exist.
 * Keeping pending and live values separate prevents a surface rebind between
 * scanout and FrameSlot_Capture from describing the next policy state. */
static ActionBgPlan s_pending_action_bg_plan;
static bool s_pending_bg_capture_pad_to_budget;

static ActionBgPlan ActRaiser_NativeBgPresentationPlan(void) {
  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  return plan;
}

/* Project an explicit final PPU policy into a plan without inspecting PPU
 * masks after the fact. This is used for non-action scenes and deliberate
 * global/debug overrides. The normal action path retains the canonical plan
 * verbatim, so map-specific action classification has only one owner. */
static void ActRaiser_ProjectBgPresentationPolicy(
    ActionBgPlan *plan, uint8 clamp, uint8 mirror, uint8 repeat,
    bool bound_canvas_to_world) {
  static bool reported_invalid_policy;
  if (!plan) return;
  ActionBgPresentationPolicy policy = {
    .clamp_layers = clamp,
    .mirror_layers = mirror,
    .repeat_layers = repeat,
    .bound_canvas_to_world = bound_canvas_to_world,
  };
  if (ActionBgPlan_ApplyPresentationPolicy(plan, &policy)) return;

  /* All callers use production masks/bands, so rejection is an invariant
   * failure. Publish a deterministic native plan rather than leaving stale
   * source/edge metadata in FrameSlot, and report it once for diagnosis. */
  ActionBgPlan_InitNative(plan);
  if (!reported_invalid_policy) {
    reported_invalid_policy = true;
    fprintf(stderr,
            "ERROR: invalid background presentation policy; using native plan\n");
  }
}

static SrPpuBackgroundFill ActRaiser_PpuBandFill(
    ActionBgEdgeMode edge) {
  switch (edge) {
    case kActionBgEdge_Transparent:
      return SR_PPU_BACKGROUND_FILL_TRANSPARENT;
    case kActionBgEdge_LiveWorld:
      return SR_PPU_BACKGROUND_FILL_LIVE_WORLD;
    case kActionBgEdge_Clamp:
      return SR_PPU_BACKGROUND_FILL_CLAMP;
    case kActionBgEdge_Mirror:
      return SR_PPU_BACKGROUND_FILL_MIRROR;
    case kActionBgEdge_Repeat:
      return SR_PPU_BACKGROUND_FILL_REPEAT;
    case kActionBgEdge_RawWrap:
    default:
      return SR_PPU_BACKGROUND_FILL_RAW_WRAP;
  }
}

static bool ActRaiser_ProjectBgPresentationBands(
    const ActionBgPresentationPolicy *policy,
    SrPpuFramePolicy *frame_policy,
    SrPpuFramePolicyBand *bands, size_t band_capacity) {
  if (!policy || !frame_policy ||
      policy->band_count > band_capacity)
    return false;
  frame_policy->layer_normal_scroll_mask =
      policy->normal_scroll_layers;
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *band = &policy->bands[i];
    bands[i] = (SrPpuFramePolicyBand) {
      .layer = band->layer,
      .y0 = band->y0,
      .y1 = band->y1,
      .fill = ActRaiser_PpuBandFill(band->edge),
      .motion = band->motion == kActionBgMotion_NormalScroll
          ? SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL
          : SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE,
    };
  }
  frame_policy->bands = policy->band_count ? bands : NULL;
  frame_policy->band_count = policy->band_count;
  return true;
}

static uint32 ActRaiser_BgBandSignature(
    const ActionBgPresentationPolicy *policy) {
  if (!policy) return 0;
  uint32 signature = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (unsigned i = 0; i < policy->band_count; i++) {
    const ActionBgPresentationBand *band = &policy->bands[i];
    const uint8 bytes[] = {
      band->layer, band->y0, band->y1,
      (uint8)band->edge, (uint8)band->motion,
    };
    for (unsigned byte = 0; byte < sizeof(bytes); byte++)
      signature = DeterministicHash_Fnv1a32Byte(
          signature, bytes[byte]);
  }
  return signature;
}

/* Build and publish the one canonical action-background decision before any
 * generic/debug presentation override runs. Draft application is explicit:
 * native 4:3 still observes the room for authoring but cannot apply synthetic
 * margin policy, while the normal wide path may A/B its session draft. */
static bool ActRaiser_ResolveActionBgPlan(
    uint8 map_group, uint8 map_number, bool apply_tuner_draft,
    ActionBgPlan *plan, ActionBgPresentationPolicy *presentation) {
  static bool reported_rejected_draft;
  if (!plan || !presentation || !ActRaiserActionBg_BuildCurrentPlan(
          g_ram, kActRaiserWramSize,
          g_settings.ws_bg2_padding, plan, presentation) ||
      !ActionBgTuner_ObservePlan(
          map_group, map_number, plan,
          (ActionBgTunerLimits) {
            SR_PPU_HORIZONTAL_MARGIN_MAX, SR_PPU_HORIZONTAL_MARGIN_MAX,
            SR_PPU_VERTICAL_MARGIN_MAX, SR_PPU_VERTICAL_MARGIN_MAX,
          }))
    return false;
  if (apply_tuner_draft && !ActionBgTuner_ApplyDraft(plan) &&
      !reported_rejected_draft) {
    reported_rejected_draft = true;
    fprintf(stderr,
            "[action-bg-tuner] rejected stale/invalid draft; using canonical "
            "room policy\n");
  }
  return ActionBgPlan_CompilePresentation(plan, presentation);
}

/* Resolve the finite playfield's remaining horizontal world space without
 * applying it. Keeping this calculation separate leaves the ordered PPU
 * setters and final immutable plan handoff together in the caller. */
static bool ActRaiser_CalculateCanvasMargins(
    uint8 map_group, uint8 map_number, int canvas_layer, int budget,
    int *margin_left, int *margin_right) {
  if (!margin_left || !margin_right || canvas_layer < 0 ||
      canvas_layer >= kActionBgPlanLayerCount || budget < 0)
    return false;
  const int layer_offset = canvas_layer * kActRaiserBgLayerStateStride;
  const int camera_x = ActRaiser_ReadWram16(
      kActRaiserWram_Bg1CameraX + layer_offset);
  const int world_width = ActRaiser_IsSimulationTown(map_group, map_number)
      ? kActRaiserTownWorldWidth
      : ActRaiser_ReadWram16(kActRaiserWram_Bg1Width + layer_offset);
  int available_left = camera_x;
  int available_right =
      world_width - kActRaiserAuthenticWidth - camera_x;
  if (available_left < 0) available_left = 0;
  if (available_right < 0) available_right = 0;
  *margin_left = available_left < budget ? available_left : budget;
  *margin_right = available_right < budget ? available_right : budget;
  return true;
}

static void ActRaiser_LogWidescreenLayers(void) {
  if (!ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_WidescreenLayerLog))
    return;
  static int last_frame = -1;
  const unsigned game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if ((int)game_frame == last_frame) return;
  last_frame = (int)game_frame;
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu)) return;
  fprintf(stderr,
          "[ws-layers] gf=%u mode=%d main=%02x sub=%02x "
          "wsel=%06x cgwsel=%02x cgadsub=%02x",
          game_frame, ppu.bg_mode,
          ppu.main_screen, ppu.sub_screen,
          ppu.window_select, ppu.color_math_control,
          ppu.color_math_designation);
  for (int layer = 0; layer < 4; layer++)
    fprintf(stderr, " BG%d[w%d h%02x hs=%d]", layer + 1,
            ppu.background_tilemap_control[layer] & 1,
            ppu.background_tilemap_control[layer] & 0xfc,
            ppu.backgrounds[layer].h_scroll);
  fprintf(stderr, " win1=[%d,%d] win2=[%d,%d]\n",
          ppu.window1_left, ppu.window1_right,
          ppu.window2_left, ppu.window2_right);
}

/* The compositor writes only the active window. Finite action/town worlds can
 * leave steady strips at the framebuffer edges, so clear them every frame;
 * other modes retain the change-triggered full clear. */
static void ActRaiser_ClearWidescreenMarginGaps(
    bool bounded_world_margins,
    const SrPpuFrameTransactionContext *context) {
  static int last_left = -1, last_right = -1;
  if (!context) return;
  const int left = context->state.margin_left;
  const int right = context->state.margin_right;
  if (bounded_world_margins && context->main.data) {
    /* In flat mode these strips are intentional black pillarbox. In Diorama
     * the framebuffer is an opaque backdrop plane, so use its own backdrop
     * colour when the accepted margin repair is active. */
    const uint32 gap_fill =
        (g_settings.diorama_mode && g_settings.diorama_margin_fix)
            ? ActRaiser_BackdropArgb(
                  context->cgram.data[0], context->state.brightness)
            : 0u;
    ActRaiserFillMarginGaps(
        context->main.data, (size_t)context->main.pitch_bytes,
        kActRaiserAuthenticHeight, context->frame.margin_budget,
        left, right, gap_fill);
    last_left = left;
    last_right = right;
  } else if (left != last_left || right != last_right) {
    last_left = left;
    last_right = right;
    if (context->main.data)
      memset(context->main.data, 0,
             (size_t)context->main.pitch_bytes * kActRaiserAuthenticHeight);
  }
}

/* Per-frame VERTICAL margin policy — the transpose of the bounded-world side
 * margin clamp in ActRaiser_ApplyWidescreenPolicy, and deliberately built the
 * same way: ask the game's own camera and layer-dimension state how much world
 * actually exists past the viewport edge, and never request more than that.
 *
 * The camera routine at $02:B091 clamps V to [0, $30 - $E1] with $E1 = 225 --
 * the hardcoded viewport height, exactly as the H clamp's $100 is the
 * hardcoded 256 width (rendering-engine.md §6). So `camera_y` IS the number of
 * world rows above the viewport, and `height - 225 - camera_y` the number
 * below. At the top of a level both the camera and the available margin are 0,
 * which is what stops the band from showing the void the level ends at.
 *
 * Action stages only. Simulation towns get their 3D treatment from sim3d.c and
 * the world map is Mode 7, whose per-scanline matrix cannot be extrapolated
 * past the visible band (see PpuDrawBackground_mode7). */
static void ActRaiser_ResolveVerticalMarginPolicy(
    uint8_t map_group, uint8_t map_number,
    SrPpuFramePolicy *frame_policy) {
  extern int g_ws_extra_top;
  extern int g_ws_extra_bottom;
  extern bool Diorama_IsActiveThisFrame(void);

  g_ws_extra_top = 0;
  g_ws_extra_bottom = 0;
  if (!frame_policy) return;

  int budget = g_settings.diorama_vertical_extend;
  const int primary_layer =
      ActionBgPlan_PrimaryLayer(&s_pending_action_bg_plan);
  if (budget > 0 && Diorama_IsActiveThisFrame() &&
      ActRaiser_IsActionMapGroup(map_group) &&
      !ActRaiser_IsSimulationTown(map_group, map_number) &&
      primary_layer >= 0) {
    if (budget > (int)SR_PPU_VERTICAL_MARGIN_MAX)
      budget = (int)SR_PPU_VERTICAL_MARGIN_MAX;
    const int layer_offset =
        primary_layer * kActRaiserBgLayerStateStride;
    ActRaiserActionBg_ResolveVerticalMargins(
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY + layer_offset),
        ActRaiser_ReadWram16(kActRaiserWram_Bg1Height + layer_offset),
        budget, &g_ws_extra_top, &g_ws_extra_bottom);
  }
  frame_policy->margin_top_pixels = (uint32_t)g_ws_extra_top;
  frame_policy->margin_bottom_pixels = (uint32_t)g_ws_extra_bottom;
  /* The role catalog chooses the primary plane that governs capture height,
   * but every layer has its OWN camera. Clip each layer independently
   * before its camera reaches row 0: otherwise a BG2 at Y=0 wraps negative
   * synthetic lines to the bottom of its tilemap while the playfield
   * legitimately extends above the viewport. Fillmore act 2 exposed that as
   * red BG2 geometry half-added over its grey BG1 castle wall. */
  if (g_ws_extra_top > 0 || g_ws_extra_bottom > 0) {
    for (int layer = 0; layer < kActionBgPlanLayerCount; layer++) {
      const int offset = layer * kActRaiserBgLayerStateStride;
      int top_rows = 0, bottom_rows = 0;
      ActRaiserActionBg_ResolveVerticalMargins(
          ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY + offset),
          ActRaiser_ReadWram16(kActRaiserWram_Bg1Height + offset),
          budget, &top_rows, &bottom_rows);
      frame_policy->vertical_clip_layer_mask |= 1u << layer;
      frame_policy->vertical_clip_top_rows[layer] = (uint32_t)top_rows;
      frame_policy->vertical_clip_bottom_rows[layer] =
          (uint32_t)bottom_rows;
    }
  }

  /* AR_VEXT_TILES=1: dump the primary tilemap ids the band reads next to the
   * first visible row. A filler row is one whose ids do not belong to the
   * surrounding content; merely being uniform is insufficient because a real
   * all-sky row is uniform too (rendering-engine.md §4). */
  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionTileLog) &&
      g_ws_extra_top > 0) {
    SrPpuStateSnapshot ppu;
    SrBorrowedU16Span vram = {.struct_size = sizeof(vram)};
    if (!ActRaiser_QueryPpuState(&ppu) || !s_runner_api ||
        !s_runner_api->borrow_u16_memory ||
        s_runner_api->borrow_u16_memory(
            s_runner, SR_MEMORY_VRAM, &vram) != SR_RESULT_OK)
      return;
    int base = ppu.backgrounds[primary_layer].tilemap_base_word;
    bool wider = ppu.backgrounds[primary_layer].tilemap_width_tiles == 64u;
    bool higher = ppu.backgrounds[primary_layer].tilemap_height_tiles == 64u;
    const int layer_offset =
        primary_layer * kActRaiserBgLayerStateStride;
    int cam_y = ActRaiser_ReadWram16(
        kActRaiserWram_Bg1CameraY + layer_offset);
    char buf[512]; int n = 0;
    for (int py = cam_y - g_ws_extra_top; py < cam_y + 16; py += 8) {
      int off = base + (((py >> 3) & 0x1f) << 5);
      if ((py & 0x100) && higher) off += wider ? 0x800 : 0x400;
      unsigned ids[4];
      for (int k = 0; k < 4; k++)
        ids[k] = vram.data[(off + k * 7) & 0x7fff] & 0x3ff;
      n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%03X,%03X,%03X,%03X",
                    py == cam_y ? " | vis:" : (n ? " " : ""),
                    ids[0], ids[1], ids[2], ids[3]);
      if (n >= (int)sizeof(buf) - 32) break;
    }
    fprintf(stderr, "[vext-tiles] gf=%u camY=%4d phase=%3d band: %s\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame), cam_y,
            cam_y & 0xFF, buf);
  }

  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionLog) &&
      primary_layer >= 0) {
    static unsigned last;
    unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last) {
      SrPpuStateSnapshot ppu;
      if (!ActRaiser_QueryPpuState(&ppu)) return;
      last = gf;
      fprintf(stderr,
              "[vext] gf=%u top=%d layer=%d camY=%d worldH=%d "
              "vscroll=%d/%d screenEn=$%02x tilemap=$%04x\n",
              gf, g_ws_extra_top, primary_layer + 1,
              ActRaiser_ReadWram16(
                  kActRaiserWram_Bg1CameraY +
                  primary_layer * kActRaiserBgLayerStateStride),
              ActRaiser_ReadWram16(
                  kActRaiserWram_Bg1Height +
                  primary_layer * kActRaiserBgLayerStateStride),
              ppu.backgrounds[primary_layer].v_scroll,
              ppu.backgrounds[1].v_scroll,
              ppu.main_screen,
              ppu.backgrounds[primary_layer].tilemap_base_word);
    }
  }
}

static const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void) {
  if (!g_settings.diorama_mode) return NULL;
  const DioramaRoomOverride *room = DioramaLayerOrder_Find(
      Diorama_LayerOverrides(), g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_CurrentMap]);
  /* Raw map 0701 is reused after the final boss. The face scene owns BG2SC
   * $70; the sky/cloud return scene switches it to $74. The manifest's virtual
   * face band must not make that later sky foreground-sharp merely because the
   * two scenes share map bytes. */
  SrPpuStateSnapshot ppu;
  if (room && g_ram[kActRaiserWram_MapGroup] == kActRaiserMapGroup_DeathHeim &&
      g_ram[kActRaiserWram_CurrentMap] == kActRaiserDeathHeimMap_Hub &&
      (!ActRaiser_QueryPpuState(&ppu) ||
       ppu.backgrounds[1].tilemap_base_word != 0x7000))
    return NULL;
  return room;
}

static bool ActRaiser_CommitPpuFramePolicy(
    const SrPpuFramePolicy *policy) {
  const SrResult result = RtlGameApplyPpuFramePolicy(policy);
  if (result == SR_RESULT_OK) return true;
  SessionFatal_Request(
      "The runner rejected ActRaiser's frame presentation policy "
      "(error %u).", (unsigned)result);
  return false;
}

static bool ActRaiser_ClaimOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags) {
  SrPpuStateSnapshot ppu;
  if (!ActRaiser_QueryPpuState(&ppu) || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE ||
      !s_runner_api->claim_ppu_overlay_capture)
    return false;
  const SrPpuOverlayCaptureRequest request = {
    .struct_size = sizeof(request),
    .flags = flags,
    .lifetime_generation = ppu.lifetime_generation,
    .source = source,
    .x = x,
    .y = y,
    .width = width,
    .height = height,
  };
  return s_runner_api->claim_ppu_overlay_capture(
             s_runner, &request) == SR_RESULT_OK;
}

static SrPpuOverlayCaptureState ActRaiser_OverlayCaptureState(
    const SrPpuOverlayState *overlay) {
  if (!overlay) return (SrPpuOverlayCaptureState){0};
  return (SrPpuOverlayCaptureState) {
    .x0 = overlay->x0,
    .x1 = overlay->x1,
    .y0 = overlay->y0,
    .y1 = overlay->y1,
    .flags = overlay->flags,
    .transparent_fill_configured = overlay->transparent_fill_configured,
    .transparent_fill_mode = overlay->transparent_fill_mode,
    .transparent_fill_cgram = overlay->transparent_fill_cgram,
    .oam_first = overlay->oam_first,
    .oam_count = overlay->oam_count,
  };
}

static bool ActRaiser_ExchangeOverlayCapture(
    uint32_t source, uint64_t generation,
    const SrPpuOverlayCaptureState *expected,
    const SrPpuOverlayCaptureState *replacement) {
  if (source >= SR_PPU_OVERLAY_SOURCE_COUNT || !expected || !replacement ||
      !s_runner || !s_runner_api ||
      s_runner_api->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !s_runner_api->compare_exchange_ppu_overlay_captures)
    return false;
  SrPpuOverlayCaptureExchangeRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = generation,
    .source_mask = UINT32_C(1) << source,
  };
  request.expected[source] = *expected;
  request.replacement[source] = *replacement;
  return s_runner_api->compare_exchange_ppu_overlay_captures(
             s_runner, &request) == SR_RESULT_OK;
}

typedef struct ActRaiserPpuFrameAccess {
  const SrPpuFrameTransactionContext *context;
  SrPpuOverlayCaptureState captures[SR_PPU_OVERLAY_SOURCE_COUNT];
} ActRaiserPpuFrameAccess;

/* ActRaiser is a singleton linked game and frame transactions are synchronous.
 * This callback-scoped adapter keeps the many legacy enhancement helpers on a
 * compact call surface while preventing any concrete PPU pointer from leaking
 * back into them. It is cleared before the callback returns. */
static ActRaiserPpuFrameAccess *s_ppu_frame_access;

static const SrPpuFrameTransactionContext *ActRaiser_PpuFrame(void) {
  return s_ppu_frame_access ? s_ppu_frame_access->context : NULL;
}

static const SrPpuOverlayCaptureState *ActRaiser_PpuCapture(
    uint32_t source) {
  return s_ppu_frame_access && source < SR_PPU_OVERLAY_SOURCE_COUNT
      ? &s_ppu_frame_access->captures[source]
      : NULL;
}

static bool ActRaiser_SetPpuCaptureState(
    uint32_t source, const SrPpuOverlayCaptureState *replacement) {
  if (!s_ppu_frame_access || source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
      !replacement)
    return false;
  SrPpuOverlayCaptureState *current =
      &s_ppu_frame_access->captures[source];
  if (!ActRaiser_ExchangeOverlayCapture(
          source, s_ppu_frame_access->context->lifetime_generation,
          current, replacement))
    return false;
  *current = *replacement;
  return true;
}

static bool ActRaiser_SetPpuOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags) {
  const SrPpuOverlayCaptureState *current = ActRaiser_PpuCapture(source);
  if (!current || width <= 0 || height <= 0) return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.x0 = (int16_t)x;
  replacement.x1 = (int16_t)(x + width);
  replacement.y0 = (int16_t)y;
  replacement.y1 = (int16_t)(y + height);
  replacement.flags = flags;
  replacement.oam_first = 0u;
  replacement.oam_count = 0u;
  return ActRaiser_SetPpuCaptureState(source, &replacement);
}

static bool ActRaiser_SetPpuOverlayFill(
    uint32_t source, SrPpuTransparentFillMode mode, uint8_t cgram_index) {
  const SrPpuOverlayCaptureState *current = ActRaiser_PpuCapture(source);
  if (!current) return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.transparent_fill_configured = 1u;
  replacement.transparent_fill_mode = (uint8_t)mode;
  replacement.transparent_fill_cgram = cgram_index;
  return ActRaiser_SetPpuCaptureState(source, &replacement);
}

static bool ActRaiser_SetPpuOverlayOamRange(
    uint8_t first, uint8_t count) {
  const SrPpuOverlayCaptureState *current =
      ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
  if (!current || count == 0u || first >= 128u || count > 128u - first)
    return false;
  SrPpuOverlayCaptureState replacement = *current;
  replacement.oam_first = first;
  replacement.oam_count = count;
  return ActRaiser_SetPpuCaptureState(
      SR_PPU_OVERLAY_OBJ, &replacement);
}

static bool ActRaiser_BindPpuOutput(
    SrPpuOutputKind kind, uint32_t source, uint32_t band,
    uint8_t *pixels, size_t pitch, uint32_t height) {
  if (!s_runner || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE ||
      !s_runner_api->bind_ppu_output_surface || pitch > UINT64_MAX / height)
    return false;
  const SrPpuOutputBindingRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = s_ppu_frame_access
        ? s_ppu_frame_access->context->lifetime_generation : 0u,
    .kind = kind,
    .source = source,
    .band = band,
    .pixels = pixels,
    .pixel_byte_size = (uint64_t)pitch * height,
    .pitch_bytes = pitch,
    .height_pixels = height,
  };
  return s_runner_api->bind_ppu_output_surface(
             s_runner, &request) == SR_RESULT_OK;
}

static bool ActRaiser_ResolvePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    SrPpuObjResolveResult *result) {
  SrPpuObjPart parts[128];
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !result || count == 0u || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_OBJ_RESOLVE_SIZE ||
      !s_runner_api->resolve_ppu_obj_range)
    return false;
  const SrPpuObjResolveRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .first_sprite = first,
    .sprite_count = count,
    .priority = priority,
    .part_capacity = 128u,
    .parts = parts,
  };
  *result = (SrPpuObjResolveResult){
    .struct_size = sizeof(*result),
  };
  return s_runner_api->resolve_ppu_obj_range(
             s_runner, &request, result) == SR_RESULT_OK;
}

static bool ActRaiser_RasterizePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    uint32_t *pixels, size_t pitch, size_t byte_size,
    SrPpuObjRasterResult *result) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !pixels || !result || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE ||
      !s_runner_api->rasterize_ppu_obj_range)
    return false;
  const SrPpuObjRasterRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .first_sprite = first,
    .sprite_count = count,
    .priority = priority,
    .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
    .pixels = pixels,
    .pixel_byte_size = byte_size,
    .pitch_bytes = pitch,
  };
  *result = (SrPpuObjRasterResult){
    .struct_size = sizeof(*result),
  };
  return s_runner_api->rasterize_ppu_obj_range(
             s_runner, &request, result) == SR_RESULT_OK;
}

static bool ActRaiser_RasterizePpuObjParts(
    const SrPpuObjPart *parts, size_t part_count,
    int x0, int y0, int x1, int y1,
    uint32_t *pixels, size_t pitch, size_t byte_size) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  SrPpuObjRasterResult result = {.struct_size = sizeof(result)};
  if (!frame || !parts || !part_count || !pixels || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE ||
      !s_runner_api->rasterize_ppu_obj_parts)
    return false;
  const SrPpuObjPartsRasterRequest request = {
    .struct_size = sizeof(request),
    .lifetime_generation = frame->lifetime_generation,
    .parts = parts,
    .part_count = part_count,
    .x0 = x0,
    .y0 = y0,
    .x1 = x1,
    .y1 = y1,
    .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
    .pixels = pixels,
    .pixel_byte_size = byte_size,
    .pitch_bytes = pitch,
  };
  return s_runner_api->rasterize_ppu_obj_parts(
             s_runner, &request, &result) == SR_RESULT_OK;
}

static bool ActRaiser_ConfigurePpuObjCapture(
    const SrPpuObjCaptureRequest *request) {
  return request && s_runner && s_runner_api &&
      s_runner_api->struct_size >= SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE &&
      s_runner_api->configure_ppu_obj_capture &&
      s_runner_api->configure_ppu_obj_capture(
          s_runner, request) == SR_RESULT_OK;
}

static void ActRaiser_ApplyWidescreenPolicy(void) {
  extern bool g_ws_active;
  extern int g_ws_extra;
  SrPpuFramePolicyBand frame_bands[kActionBgPresentationBandMax];
  SrPpuFramePolicy frame_policy = {
    .struct_size = sizeof(frame_policy),
    .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
    .margin_budget_pixels = (uint32_t)g_ws_extra,
  };
  static int survey = -1;
  if (survey < 0) {
    const char *e = getenv("AR_WS_SURVEY");
    survey = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  uint8 hud_split_height = 0;
  uint8 hud_split_left_end = 0;
  uint8 hud_split_right_start = 0;
  uint8 hud_player_row_y = 0;
  uint8 hud_left_only_y = 0;

  /* Host-overlay HUD layout is independent of the world having side margins.
   * BG3 rows 0-3 are extracted into a transparent surface, then the host
   * scales and anchors the status groups after the framebuffer is presented.
   * Keeping this policy ahead of the no-widescreen-budget return is what makes
   * HUD scale work in authentic 4:3 as well as 16:x. Wide Raw deliberately
   * remains the unsplit comparison mode. */
  if (!survey && g_settings.display_mode != kDisplayMode_WideRaw) {
    if (ActRaiser_IsActionMapGroup(map_group)) {
      /* y=0-19 ACT/TIME/SCORE; y=20-27 player; y=28-39 enemy. */
      hud_split_height = kActRaiserActionHudHeight;
      hud_split_left_end = kActRaiserActionHudLeftEnd;
      hud_split_right_start = kActRaiserActionHudRightStart;
      hud_player_row_y = kActRaiserActionHudPlayerRowY;
      hud_left_only_y = kActRaiserActionHudEnemyRowY;
    } else if (map_group == kActRaiserMapGroup_NonAction &&
               map_number >= kActRaiserSimulationTown_First &&
               map_number <= kActRaiserNonActionMap_SkyPalace) {
      hud_split_height = kActRaiserSimulationHudHeight;
      hud_split_left_end = kActRaiserSimulationHudSplit;
      hud_split_right_start = kActRaiserSimulationHudSplit;
      hud_player_row_y = kActRaiserSimulationHudHeight;
      hud_left_only_y = kActRaiserSimulationHudHeight;
    }
  }
  frame_policy.hud_split_height = hud_split_height;
  frame_policy.hud_left_end_x = hud_split_left_end;
  frame_policy.hud_right_start_x = hud_split_right_start;
  frame_policy.hud_player_row_y = hud_player_row_y;
  frame_policy.hud_left_only_y = hud_left_only_y;

  ActionBgTuner_BeginFrame();
  s_pending_action_bg_plan = ActRaiser_NativeBgPresentationPlan();
  s_pending_bg_capture_pad_to_budget = false;
  if (!g_ws_active) {
    /* BH5 owns eligible authentic world layers independently of presentation
     * width. Keep the native 4:3 path in the same default-off provider census;
     * decorative policy is irrelevant without margins, but its source
     * classification remains the authority for which layers may bind. */
    bool bind_plan = false;
    ActionBgPlan plan;
    if (ActRaiser_IsActionMapGroup(map_group)) {
      ActionBgPresentationPolicy presentation;
      if (ActRaiser_ResolveActionBgPlan(
              map_group, map_number, false,
              &plan, &presentation)) {
        s_pending_action_bg_plan = plan;
        /* No side columns are rendered in this mode. Preserve source ownership
         * for the authentic provider, but describe the executed presentation
         * as raw/live so a mirror decision cannot imply nonexistent padding. */
        ActRaiser_ProjectBgPresentationPolicy(
            &s_pending_action_bg_plan, 0, 0, 0, false);
        bind_plan = true;
      }
    }
    /* Beginning the policy transaction clears any prior frame's virtual maps
     * and layer extents before the optional authentic provider is republished. */
    ActRaiser_ResolveVerticalMarginPolicy(
        map_group, map_number, &frame_policy);
    if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
    if (hud_split_height)
      ActRaiser_ClaimOverlayCapture(
          SR_PPU_OVERLAY_BG3, 0, 0, kActRaiserAuthenticWidth,
          hud_split_height, SR_PPU_OVERLAY_REMOVE_FROM_GAME);
    if (bind_plan)
      ActRaiserActionBg_BindPlanWithVirtualLayers(
          g_ram, kActRaiserWramSize, &plan,
          ActRaiser_CurrentVirtualLayerRoom());
    return;
  }
  /* Per-mode widescreen policy (docs/rendering-engine.md section 13). Two knobs per
   * mode: (1) does it use the wide view at all, and (2) a per-layer clamp
   * mask (bit L keeps BG(L+1) at 256) for scenes that mix wide world layers
   * with 256-wide UI/dialog layers whose offscreen tilemap data must not tile
   * into the margins. We keep the classification explicit here (not an
   * auto-heuristic) so we don't touch game code while proving the base recomp
   * accurate — a mis-widened layer is a policy line, not a decode bug.
   * BG3 (layer 2, the HUD) is already margin-clamped by the engine default. */
  int wide = survey;
  uint8 clamp = 0;
  uint8 mirror = 0;
  uint8 repeat = 0;
  ActionBgPresentationPolicy bg_presentation = { 0 };
  int bg_plan_valid = 0;
  int bg_plan_source_bg1 = kNoActionBgPlanSource;
  int bg_plan_source_bg2 = kNoActionBgPlanSource;
  int bg_hle_allowed = 0;
  uint8 bg_hle_bindings = 0;
  ActionBgPlan bg_plan = { 0 };
  bool project_final_bg_policy = false;
  /* True when the current wide world has finite horizontal bounds. The PPU
   * still owns the fixed centering budget; this policy narrows the live left
   * and right margins as the camera approaches either world edge. */
  int bounded_world_margins = 0;
  int canvas_layer = -1;
  if (!survey && map_group == kActRaiserMapGroup_NonAction) {
    switch (map_number) {
      case kActRaiserNonActionMap_SkyPalace:
                            /* Sky Palace hub. BG1 = sky/clouds (wide, clean).
                               BG2 = pillars plus game-owned offscreen dialog
                               staging farther around its 64x64 tilemap. Keep
                               BG2 raw-wide; the render transaction decodes a
                               box-free ROM source into only margin columns.
                               The authentic center retains its BG2 box. */
        wide = 1;
        break;

      case kActRaiserNonActionMap_WorldMap:
                            /* Mode 7 world map: fully wide, no UI layers. */
        wide = 1; 
        break;
      
      case kActRaiserNonActionMap_Title:
        // Title is always not wide screen since the backdrop is black
        wide = 0;
        break;
    
      case kActRaiserNonActionMap_Fillmore:
      case kActRaiserNonActionMap_Bloodpool:
      case kActRaiserNonActionMap_Kasandora:
      case kActRaiserNonActionMap_Aitos:
      case kActRaiserNonActionMap_Marahna:
      case kActRaiserNonActionMap_Northwall: {
        /* $01:B4C6 clamps camera X ($22) to $0000-$0100, proving 256px of
         * world on either side of the authentic viewport. AR_WS_SIM=0 is the
         * same-binary authentic baseline for town regression captures. BG2 is
         * the bounded dialog/overlay plane and remains center-clamped. The
         * separate ADAD/AE6F and B473 ports use this same $01-$06 range. */
        wide = g_settings.ws_sim;
        bounded_world_margins = wide;
        canvas_layer = kActRaiserPpuLayer_Bg1;
        clamp = kActRaiserBgLayerMask_Bg2;
        break;
      }

      case kActRaiserNonActionMap_Temple:
      // Temple cut scenes don't need wide screen support
      // background is black
        wide = 0;
        break;

      default:              /* title(00), temple cutscene(08),
                               transitions, unknown: pillarbox (the temple is a
                               black backdrop with no wide-worthy layer). */
        wide = 0;
        break;
    }
  } else if (!survey && ActRaiser_IsActionMapGroup(map_group)) {
    /* Validated action-wide path, shared by all seven action-region handler
     * tables. Original tile streamers remain active for the authentic ring and
     * oracle; the bounded HLE provider supplies eligible world coordinates,
     * while the audited $8C98/$8D68 seams widen drawing and activation.
     * AR_WS_ACTION=0 restores the pillarboxed action baseline. */
    wide = g_settings.ws_action;
    ActionBgPresentationPolicy bg_policy;
    if (ActRaiser_ResolveActionBgPlan(
            map_group, map_number, true, &bg_plan, &bg_policy)) {
      bg_plan_valid = 1;
      bg_plan_source_bg1 = bg_plan.layer[0].source;
      bg_plan_source_bg2 = bg_plan.layer[1].source;
      bg_hle_allowed = wide;
      clamp = bg_policy.clamp_layers;
      mirror = bg_policy.mirror_layers;
      repeat = bg_policy.repeat_layers;
      bg_presentation = bg_policy;
    }
  }
  /* AR_WS_ONLYBG=N (1..4): isolate one BG layer so a capture identifies which
   * layer carries scene elements such as sky, dialog, or pillars. */
  {
    const ActRaiserDeveloperEnvironment *developer_environment =
        ActRaiser_GetDeveloperEnvironment();
    if (developer_environment->widescreen_only_bg_present) {
      int L = developer_environment->widescreen_only_bg_layer;
      if (L >= 0 && L < 4)
        cpu_write8(&g_cpu, 0x00, 0x212c, (uint8)(1u << L));
      wide = 1; clamp = 0; mirror = 0; repeat = 0;  /* raw tilemap data */
      bg_presentation = (ActionBgPresentationPolicy){ 0 };
      bg_hle_allowed = 0;
      project_final_bg_policy = true;
    }
  }
  /* AR_WS_CLAMP=<hex mask>: override the per-layer clamp for tuning. */
  {
    const ActRaiserDeveloperEnvironment *developer_environment =
        ActRaiser_GetDeveloperEnvironment();
    if (developer_environment->widescreen_clamp_present) {
      wide = 1; clamp = developer_environment->widescreen_clamp_mask;
      mirror = 0; repeat = 0;
      bg_presentation = (ActionBgPresentationPolicy){ 0 };
      bg_hle_allowed = 0;
      project_final_bg_policy = true;
    }
  }
  /* Capture presets are final policy overrides, intentionally after the
   * scene-specific rules and diagnostic clamp knob. This makes promotional
   * comparisons deterministic:
   *
   *   4:3  authentic centre 256, no HLE presentation
   *   RAW  full wide canvas, no clamp/pad/repeat/gap/world-edge correction
   *   FULL scene policy plus every HLE gate enabled by Settings_SetDisplayMode
   *
   * RAW must not inherit a sim BG2 clamp or an action finite-world margin just
   * because those policies are normally useful. The individual HLE builders
   * and sprite/activation seams are disabled by the RAW preset's ws_* flags. */
  if (g_settings.display_mode == kDisplayMode_43) {
    wide = 0; clamp = 0; mirror = 0; repeat = 0;
    bounded_world_margins = 0;
    bg_presentation = (ActionBgPresentationPolicy){ 0 };
    bg_hle_allowed = 0;
    project_final_bg_policy = true;
  } else if (g_settings.display_mode == kDisplayMode_WideRaw) {
    wide = 1; clamp = 0; mirror = 0; repeat = 0;
    bounded_world_margins = 0;
    bg_presentation = (ActionBgPresentationPolicy){ 0 };
    bg_hle_allowed = 0;
    project_final_bg_policy = true;
  }

  frame_policy.horizontal_mode = wide
      ? SR_PPU_HORIZONTAL_MARGIN_AVAILABLE
      : SR_PPU_HORIZONTAL_MARGIN_CENTERED;
  if (wide) {
    frame_policy.margin_left_pixels = (uint32_t)g_ws_extra;
    frame_policy.margin_right_pixels = (uint32_t)g_ws_extra;
    frame_policy.layer_clamp_mask = clamp;
    frame_policy.layer_mirror_mask = mirror;
    frame_policy.layer_repeat_mask = repeat;
    /* Captured-layer padding. Only meaningful in diorama mode, which is
     * the only thing that captures these layers; gating on the setting keeps
     * this a live A/B and keeps flat output untouched either way. */
    if (g_settings.diorama_mode && g_settings.diorama_margin_fix)
      frame_policy.flags |=
          SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET;
  }
  /* Seed vertical geometry from the resolved game-owned plan before BEGIN so
   * a stable extended frame does not momentarily collapse to 224 lines and
   * invalidate surface views again during FINALIZE. Provider fallback may
   * refine edge policy below, but it does not change the semantic owner used
   * to derive vertical camera bounds. */
  if (bg_plan_valid) s_pending_action_bg_plan = bg_plan;
  ActRaiser_ResolveVerticalMarginPolicy(
      map_group, map_number, &frame_policy);

  /* Begin clears the previous frame's providers, extents, row bands and HUD
   * state as one validated operation. Provider-dependent corrections are
   * resolved below and published through the matching finalize transaction. */
  if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
  if (hud_split_height)
    ActRaiser_ClaimOverlayCapture(
        SR_PPU_OVERLAY_BG3, 0, 0, kActRaiserAuthenticWidth,
        hud_split_height, SR_PPU_OVERLAY_REMOVE_FROM_GAME);
  if (wide) {
    if (bg_hle_allowed && bg_plan_valid) {
      bg_hle_bindings = ActRaiserActionBg_BindPlanWithVirtualLayers(
          g_ram, kActRaiserWramSize, &bg_plan,
          ActRaiser_CurrentVirtualLayerRoom());
      /* If any planned world layer cannot bind, clamp that layer to its
       * authentic viewport instead of exposing stale/wrapped ring cells in
       * synthetic margins. Wide Raw never reaches this block. */
      uint8 visible_bg_layers = 0;
      const uint32_t ppu_display = RtlGamePpuDisplayState();
      if ((RTL_GAME_PPU_DISPLAY_CONTROL(ppu_display) & 0x80u) == 0u &&
          (RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display) & 7u) == 1u) {
        visible_bg_layers = (uint8)(
            (RTL_GAME_PPU_MAIN_SCREEN(ppu_display) |
             RTL_GAME_PPU_SUB_SCREEN(ppu_display)) &
            ((1u << kActionBgPlanLayerCount) - 1u));
      }
      uint8 fallback_world_layers = ActionBgPlan_ClampUnboundWorldLayers(
          &bg_plan, bg_hle_bindings, visible_bg_layers);
      if (fallback_world_layers) {
        clamp |= fallback_world_layers;
        /* The failed world layer was atomically converted to a viewport Clamp
         * plan above. Recompile before the one band-application site so none
         * of its tuner-authored row overrides can bypass that safe fallback. */
        if (!ActionBgPlan_CompilePresentation(
                &bg_plan, &bg_presentation))
          bg_presentation = (ActionBgPresentationPolicy){ 0 };
      }
      /* The role catalog, not a PPU layer-number convention, owns the finite
       * action canvas. A missing, ambiguous, or unbound owner fails closed to
       * the already-rendered symmetric presentation. */
      canvas_layer = ActionBgPlan_CanvasOwner(&bg_plan);
      if (canvas_layer >= 0 &&
          (bg_hle_bindings & (uint8)(1u << canvas_layer)))
        bounded_world_margins = 1;
    }
    if (bounded_world_margins) {
      /* Clamp each side to the catalogued playfield's real world space.
       * Simulation towns use BG1 and the fixed 512px world proven by
       * $01:B4C6's camera clamp. Outside [0,width) stays transparent/clear. */
      int margin_left = 0, margin_right = 0;
      if (ActRaiser_CalculateCanvasMargins(
              map_group, map_number, canvas_layer, g_ws_extra,
              &margin_left, &margin_right)) {
        frame_policy.margin_left_pixels = (uint32_t)margin_left;
        frame_policy.margin_right_pixels = (uint32_t)margin_right;
      }
    }
  }
  frame_policy.layer_clamp_mask = wide ? clamp : 0u;
  frame_policy.layer_mirror_mask = wide ? mirror : 0u;
  frame_policy.layer_repeat_mask = wide ? repeat : 0u;
  if (!ActRaiser_ProjectBgPresentationBands(
          &bg_presentation, &frame_policy, frame_bands,
          sizeof(frame_bands) / sizeof(frame_bands[0]))) {
    SessionFatal_Request(
        "ActRaiser's background presentation exceeded the runner's "
        "frame-policy capacity.");
    return;
  }
  /* BH6 immutable handoff. In the ordinary action path the pure plan survives
   * unchanged, including Bloodpool/Death Heim row bands. Other scene types and
   * explicit global overrides are projected from the final local variables
   * that were just passed to the PPU; there is no present-side mask reversal. */
  if (bg_plan_valid)
    s_pending_action_bg_plan = bg_plan;
  if (!bg_plan_valid || project_final_bg_policy || !wide) {
    ActRaiser_ProjectBgPresentationPolicy(
        &s_pending_action_bg_plan,
        wide ? clamp : 0, wide ? mirror : 0, wide ? repeat : 0,
        wide && bounded_world_margins);
  } else {
    s_pending_action_bg_plan.bound_canvas_to_world =
        bounded_world_margins;
  }
  frame_policy.flags |= SR_PPU_FRAME_POLICY_FINALIZE;
  if (!ActRaiser_CommitPpuFramePolicy(&frame_policy)) return;
  ActRaiserActionBg_ApplyPlanExtents(&s_pending_action_bg_plan);
  s_pending_bg_capture_pad_to_budget =
      (frame_policy.flags &
       SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET) != 0u;
  const uint32 bg_band_signature =
      ActRaiser_BgBandSignature(&bg_presentation);
  /* One line per policy flip — cheap, and makes "why isn't this screen
   * wide?" diagnosable from any console.log (mode bytes included). */
  static int last_wide = -1, last_clamp = -1, last_mirror = -1,
             last_repeat = -1, last_bg_band_count = -1,
             last_bg_normal_scroll = -1,
             last_hud_split_height = -1, last_hud_split_left_end = -1,
             last_hud_split_right_start = -1,
             last_hud_left_only_y = -1, last_bg_plan_valid = -1,
             last_bg_plan_source_bg1 = -2, last_bg_plan_source_bg2 = -2,
             last_bg_hle_bindings = -1;
  static uint32 last_bg_band_signature;
  if (wide != last_wide || clamp != last_clamp || mirror != last_mirror ||
      repeat != last_repeat ||
      bg_presentation.band_count != last_bg_band_count ||
      bg_presentation.normal_scroll_layers != last_bg_normal_scroll ||
      bg_band_signature != last_bg_band_signature ||
      hud_split_height != last_hud_split_height ||
      hud_split_left_end != last_hud_split_left_end ||
      hud_split_right_start != last_hud_split_right_start ||
      hud_left_only_y != last_hud_left_only_y ||
      bg_plan_valid != last_bg_plan_valid ||
      bg_plan_source_bg1 != last_bg_plan_source_bg1 ||
      bg_plan_source_bg2 != last_bg_plan_source_bg2 ||
      bg_hle_bindings != last_bg_hle_bindings) {
    last_wide = wide;
    last_clamp = clamp;
    last_mirror = mirror;
    last_repeat = repeat;
    last_bg_band_count = bg_presentation.band_count;
    last_bg_normal_scroll = bg_presentation.normal_scroll_layers;
    last_bg_band_signature = bg_band_signature;
    last_hud_split_height = hud_split_height;
    last_hud_split_left_end = hud_split_left_end;
    last_hud_split_right_start = hud_split_right_start;
    last_hud_left_only_y = hud_left_only_y;
    last_bg_plan_valid = bg_plan_valid;
    last_bg_plan_source_bg1 = bg_plan_source_bg1;
    last_bg_plan_source_bg2 = bg_plan_source_bg2;
    last_bg_hle_bindings = bg_hle_bindings;
    fprintf(stderr, "[widescreen] gf=%u $18=%02x $19=%02x -> %s "
            "clamp=%02x mirror=%02x repeat=%02x bands=%u/%08x normal=%02x "
            "hud=%u/%u/%u left-only-y=%u bg-plan=%d source=%s/%s "
            "hle=%02x\n",
            (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            map_group, map_number, wide ? "WIDE" : "pillarbox",
            clamp, mirror, repeat, (unsigned)bg_presentation.band_count,
            (unsigned)bg_band_signature,
            bg_presentation.normal_scroll_layers,
            (unsigned)hud_split_height, (unsigned)hud_split_left_end,
            (unsigned)hud_split_right_start, (unsigned)hud_left_only_y,
            bg_plan_valid,
            bg_plan_valid
                ? ActionBgSourceKind_Name(
                      (ActionBgSourceKind)bg_plan_source_bg1)
                : "none",
            bg_plan_valid
                ? ActionBgSourceKind_Name(
                      (ActionBgSourceKind)bg_plan_source_bg2)
                : "none",
            bg_hle_bindings);
  }
  ActRaiser_LogWidescreenLayers();
}

/* The OAM slots ActRaiser_WidescreenHudObjPromote validated THIS frame.
 *
 * Kept separately from overlayCaptures[Obj] on purpose: the diorama block
 * further down ActRaiserDrawPpuFrame legitimately re-captures OBJ as a
 * full-frame scene layer over OAM slots 0..127, and PpuSetOverlayCapture
 * resets oamFirst/oamCount, so the capture stops being able to answer "which
 * sprites are the flat HUD icon" the moment the diorama is on. Anything that
 * needs that answer must read this record via ActRaiser_HudObjIconRange
 * instead of re-deriving it from the capture. */
static uint8_t s_hud_obj_icon_first;
static uint8_t s_hud_obj_icon_count;

bool ActRaiser_HudObjIconRange(uint8_t *first, uint8_t *count) {
  /* Writes both outputs on every path, including "nothing promoted" (0/0):
   * FrameSlot slots are recycled, so leaving them untouched would republish
   * the previous occupant's icon range on a frame that has no icon. */
  if (first) *first = s_hud_obj_icon_first;
  if (count) *count = s_hud_obj_icon_count;
  return s_hud_obj_icon_count != 0;
}

/* Promote a validated fixed-screen HUD icon out of OAM.
 *
 * Action's $00:923A icon uses tiles $D4-$D7 in the first four slots.
 * Simulation's hourglass uses x=$94/$9B, y=$0B/$13; ROM frames
 * $01:DD4B/$DD60/$DD75/$DD8A cycle upper tiles $EC-$EF and paired lower tiles
 * $FC-$FF, with horizontal flip on each right half. Both non-action icons move
 * to later slots when menu/dialog sprites appear and are scanned by their pure
 * helpers in actraiser_game.h.
 *
 * All three land on the same 16x16 footprint, which is what lets the host draw
 * whatever this promotes as one 16x16 chunk beside the right HUD group. No
 * OAM/WRAM state is changed. */
static SrResult ActRaiser_WidescreenHudObjPromoteTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context) {
  enum { kActRaiserPpuOamSlots = SR_PPU_OAM_WORD_COUNT / 2 };
  (void)user_data;
  (void)runner;
  s_hud_obj_icon_first = 0;
  s_hud_obj_icon_count = 0;
  if (!context)
    return SR_RESULT_OK;
  /* A regular HUD split is valid without a widescreen margin budget (4:3).
   * The native fallback handles flat Diorama modes that intentionally have no
   * split, notably Wide Raw. */
  const bool native_flat_diorama =
      Diorama_IsActiveThisFrame() && g_settings.diorama_hud_flat &&
      context->frame.hud_split_height == 0;
  if (!context->frame.margin_budget &&
      !context->frame.hud_split_height && !native_flat_diorama)
    return SR_RESULT_OK;

  uint8 capture_height = 0;
  uint8 capture_first = kActRaiserHudObjOamFirst;
  uint8 capture_count = kActRaiserHudObjOamCount;
  uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool split_action_hud =
      context->frame.hud_split_height == kActRaiserActionHudHeight &&
      context->frame.hud_left_end == kActRaiserActionHudLeftEnd &&
      context->frame.hud_right_start == kActRaiserActionHudRightStart &&
      context->frame.hud_player_row_y == kActRaiserActionHudPlayerRowY &&
      context->frame.hud_left_only_y == kActRaiserActionHudEnemyRowY;
  if ((split_action_hud || native_flat_diorama) &&
      ActRaiser_IsActionMapGroup(map_group)) {
    for (int slot = 0; slot < kActRaiserHudObjOamCount; slot++) {
      int index = slot * 2;
      uint8 tile = (uint8)context->oam.data[index + 1];
      uint8 y = (uint8)(context->oam.data[index] >> 8);
      uint8 expected_y = slot < 2
          ? kActRaiserHudObjUpperY : kActRaiserHudObjLowerY;
      if (tile != (uint8)(kActRaiserMagicHudFirstTile + slot) ||
          y != expected_y)
        return SR_RESULT_OK;
    }
    capture_height = kActRaiserActionHudHeight;
  } else if (context->frame.hud_split_height ==
                 kActRaiserSimulationHudHeight &&
             context->frame.hud_left_end == kActRaiserSimulationHudSplit &&
             context->frame.hud_right_start ==
                 kActRaiserSimulationHudSplit &&
             context->frame.hud_left_only_y ==
                 kActRaiserSimulationHudHeight &&
             map_group == kActRaiserMapGroup_NonAction &&
             map_number >= kActRaiserSimulationTown_First &&
             map_number <= kActRaiserNonActionMap_SkyPalace) {
    if (map_number == kActRaiserNonActionMap_SkyPalace) {
      /* Sky Palace magic icon shifts OAM slots when dialog sprites appear, and
       * changes SHAPE with the selected spell (see the two forms documented on
       * ActRaiser_SkyPalaceMagicIconSlots) -- so scan the complete OAM table
       * for the signature rather than hardcoding either a slot or a count. */
      const int large_px = context->state.object_large_size_pixels;
      int found_slot = -1, found_count = 0;
      ActRaiser_FindSkyPalaceMagicIcon(
          context->oam.data, context->high_oam.data,
          kActRaiserPpuOamSlots, large_px,
          &found_slot, &found_count);
      /* AR_HUDICON=1: one line per change of scan outcome, the same
       * change-triggered shape as the [widescreen] policy line above. This is
       * the answer to "why is the magic icon still at centre screen?" — a
       * slot=-1 line names the spell whose OAM shape the scan does not know,
       * which is exactly how the four-small-slots/one-large-slot split was
       * found. The environment gate is snapshotted before emulation starts. */
      if (ActRaiser_DeveloperFlagEnabled(
              kActRaiserDeveloperFlag_HudIconLog)) {
        static int last_spell = -1, last_slot = -2, last_count = -1;
        int spell = g_ram[kActRaiserWram_SelectedMagic];
        if (spell != last_spell || found_slot != last_slot ||
            found_count != last_count) {
          last_spell = spell; last_slot = found_slot; last_count = found_count;
          fprintf(stderr,
                  "[hud-icon] gf=%u sky-palace spell=%d -> slot=%d count=%d\n",
                  (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                  spell, found_slot, found_count);
        }
      }
      if (found_slot < 0)
        return SR_RESULT_OK;
      capture_height = kActRaiserSimulationHudHeight;
      capture_first = (uint8_t)found_slot;
      capture_count = (uint8_t)found_count;
    } else {
      /* Town sim: menus can push the four-sprite hourglass out of slots 0-3.
       * Scan for the complete phase-relative signature instead of assuming an
       * allocation; runs/20260810-231616 places it in slots 11-14. */
      const int found_slot = ActRaiser_FindSimulationHourglass(
          context->oam.data, context->high_oam.data,
          kActRaiserPpuOamSlots);
      if (found_slot < 0)
        return SR_RESULT_OK;
      capture_height = kActRaiserSimulationHudHeight;
      capture_first = (uint8_t)found_slot;
      capture_count = kActRaiserHudObjOamCount;
    }
  } else {
    return SR_RESULT_OK;
  }

  const SrPpuOverlayCaptureState expected =
      ActRaiser_OverlayCaptureState(
          &context->frame.overlays[SR_PPU_OVERLAY_OBJ]);
  const SrPpuOverlayCaptureState replacement = {
    .x1 = kActRaiserAuthenticWidth,
    .y1 = capture_height,
    .flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME,
    .oam_first = capture_first,
    .oam_count = capture_count,
  };
  if (ActRaiser_ExchangeOverlayCapture(
          SR_PPU_OVERLAY_OBJ, context->lifetime_generation,
          &expected, &replacement)) {
    s_hud_obj_icon_first = capture_first;
    s_hud_obj_icon_count = capture_count;
  }
  return SR_RESULT_OK;
}

static void ActRaiser_WidescreenHudObjPromote(void) {
  if (!s_runner || !s_runner_api ||
      s_runner_api->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !s_runner_api->visit_ppu_frame_transaction)
    return;
  const SrPpuFrameTransactionRequest request = {
    .struct_size = sizeof(request),
    .callback = ActRaiser_WidescreenHudObjPromoteTransaction,
  };
  (void)s_runner_api->visit_ppu_frame_transaction(s_runner, &request);
}

/* Split the promoted HUD icon back out of the diorama's OBJ planes. The
 * action-mode counterpart of sim3d.c's PrepareHudHandoff/RestoreTownHudPolicy
 * pair, and the reason the selected-magic icon is pinned beside the right HUD
 * group in diorama mode instead of riding the tilted scene at its authentic
 * centre-screen X.
 *
 * Why this is needed at all: ActRaiser_WidescreenHudObjPromote captures the
 * icon's OAM range into g_hud_obj_pixels for present.c to anchor, but the
 * diorama block later in this same frame re-captures OBJ as a full-frame scene
 * layer over slots 0..127 -- a strictly wider claim on the ONE capture slot the
 * PPU gives each source, and PpuSetOverlayCapture resets the OAM range as it
 * lands. That claim is right for the player and enemies and wrong for the HUD
 * icon, and there is no second OBJ capture to put the icon in, so the split has
 * to happen on captured pixels rather than on capture policy.
 *
 * Two host phases around one PPU-owned capture, because the icon's
 * tile/palette state can change during the raster:
 *
 *   Prepare, BEFORE scanout -- resolve the icon's OAM range and footprint and
 *     register an independent semantic range capture. The real sprite
 *     evaluator then writes those selected slots to g_hud_obj_pixels at the
 *     exact instant it fetches each displayed pixel. This is intentionally not
 *     a PpuRasterizeObjRange call on either side of scanout: the game-over
 *     return to Sky Palace changes the live OBJ state within the frame, and a
 *     second decode can disagree with the pixels the evaluator just emitted.
 *   Finish, AFTER scanout -- use that completed HUD surface as the opacity
 *     mask while clearing the same pixels from the diorama plane that holds
 *     the icon's priority band, which only has content once scanout has run.
 *     The diorama's OBJ capture is RemoveFromGame, so the icon is already out
 *     of the backdrop frame; only the plane needs erasing. */
enum { kActRaiserHudIconRasterLimit = 64 };
static SrPpuObjResolveResult s_hud_icon_bounds;
static int s_hud_icon_priority;
static bool s_hud_icon_ready;
static bool s_hud_icon_ppu_relocated;

/* The icon-footprint restore layer (see the block comment in
 * ActRaiser_DioramaHudObjPrepare): per footprint pixel, the colour and the
 * priority band of the sprite the promoted icon was covering. Static rather
 * than stack — three 64x64 scratch buffers is 36 KB, well past a sane frame. */
enum { kActRaiserHudRestoreNone = 0xFF };
static uint32_t s_hud_restore_argb[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];
static uint8_t s_hud_restore_prio[kActRaiserHudIconRasterLimit *
                                  kActRaiserHudIconRasterLimit];
static uint32_t s_hud_restore_slot[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];

/* g_diorama_layer_pixels[] index for an OBJ priority band. Band N is the plane
 * the diorama's kPrioBands table bound for band N, and band == OAM priority
 * (ppu.c's split does band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM
 * priority in those two bits). Band 0 is the primary source slot. */
static int ActRaiser_DioramaObjPlaneForPriority(int priority) {
  return priority ? kDioramaPlane_Obj1 + (priority - 1) : SR_PPU_OVERLAY_OBJ;
}

static void ActRaiser_DioramaHudObjPrepare(void) {
  extern bool g_diorama_frame_active;
  extern int g_ws_extra;
  extern uint8_t g_hud_obj_pixels[];
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();

  s_hud_icon_ready = false;
  s_hud_icon_ppu_relocated = false;
  /* Only the flat-HUD variant anchors a host overlay. With diorama_hud_flat
   * off the whole status bar is deliberately a tilted plane (see the A5/A7
   * note in the capture block), and the icon belongs on it. */
  if (!frame || !g_diorama_frame_active || !g_settings.diorama_hud_flat ||
      !s_hud_obj_icon_count)
    return;

  const uint8_t first = s_hud_obj_icon_first;
  const uint8_t count = s_hud_obj_icon_count;
  const int priority = (frame->oam.data[first * 2 + 1] >> 12) & 3;
  SrPpuObjResolveResult bounds;
  if (!ActRaiser_ResolvePpuObjRange(
          first, count, (uint8_t)priority, &bounds))
    return;
  const int raster_width = bounds.x1 - bounds.x0;
  const int raster_height = bounds.y1 - bounds.y0;
  /* Every promoted signature is 16x16, whether the ROM spent four small slots
   * on it or one large one; this ceiling is slack, not a target, and a range
   * that overruns it is refused rather than clipped. */
  if (raster_width <= 0 || raster_height <= 0 ||
      raster_width > kActRaiserHudIconRasterLimit ||
      raster_height > kActRaiserHudIconRasterLimit)
    return;

  s_hud_icon_bounds = bounds;
  s_hud_icon_priority = priority;
  const int surface_width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
  const SrPpuOverlayCaptureState *obj_capture =
      ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
  const bool relocate = obj_capture &&
      (obj_capture->flags & SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN) != 0u;
  const SrPpuObjCaptureRequest capture_request = {
    .struct_size = sizeof(capture_request),
    .flags = SR_PPU_OBJ_CAPTURE_RANGE |
             (relocate ? SR_PPU_OBJ_CAPTURE_RELOCATED : 0u),
    .lifetime_generation = frame->lifetime_generation,
    .range_first = first,
    .range_count = count,
    .range_x = bounds.x0,
    .range_y = bounds.y0,
    .range_width = (uint32_t)raster_width,
    .range_height = (uint32_t)raster_height,
    .range_pixels = g_hud_obj_pixels,
    .range_pixel_byte_size =
        (uint64_t)surface_width * sizeof(uint32_t) *
        kHostDisplayFramebufferHeight,
    .range_pitch_bytes = (uint64_t)surface_width * sizeof(uint32_t),
    .relocated_first = relocate ? first : 0u,
    .relocated_count = relocate ? count : 0u,
  };
  if (!ActRaiser_ConfigurePpuObjCapture(&capture_request))
    return;
  s_hud_icon_ready = true;
  s_hud_icon_ppu_relocated = relocate;

  /* --- Restore layer: what the promoted sprites were HIDING ---------------
   *
   * Every captured OBJ pixel competes in ONE shared z-buffer
   * (ppu.c `overlayBuffers[SR_PPU_OVERLAY_OBJ]`: first opaque writer wins,
   * OAM walked in slot order); only at scanout is the surviving pixel routed
   * to the plane matching its own priority. The promoted icon is the LEADING
   * slot range, so wherever it overlaps a world sprite it wins the z-test and
   * that sprite's pixels are never captured into ANY plane.
   *
   * That is correct on hardware -- the icon is in front, so it hides what is
   * behind it. It stops being correct the moment we MOVE the icon to the HUD
   * anchor: zeroing it out of its own band then leaves a hole shaped like the
   * icon cut out of whatever stood behind it. Found 2026-08-05 as a bite taken
   * out of a level gargoyle, and it needs only an on-screen overlap, so it is
   * independent of the vertical band.
   *
   * Fix: replay the same first-writer-wins rule over the icon's footprint with
   * the promoted slots REMOVED, keeping each restored pixel's colour AND the
   * priority band it belongs to. Rasterised here, pre-scanout, so the
   * best-effort underlay cannot accidentally use the NEXT frame's streamed
   * art. Unlike the visible icon this is only a rare overlap repair; capturing
   * its complete per-line OAM competition would require a second OBJ resolve.
   *
   * Not a full re-render: the hardware sprite-per-line limits are not replayed,
   * so a footprint contested by more than 34 slivers on a line could restore a
   * pixel the PPU would have dropped. Bounded by a 16x16 HUD icon, and failing
   * that way (showing the sprite) beats failing the other (a hole). */
  memset(s_hud_restore_prio, kActRaiserHudRestoreNone,
         (size_t)raster_width * raster_height);
  uint8_t index = frame->state.object_priority_rotation
      ? (uint8_t)(frame->state.oam_address_low & 0xfe) : 0;
  for (int evaluated = 0; evaluated < 128;
       evaluated++, index = (uint8_t)(index + 2)) {
    const int slot = index >> 1;
    if (slot >= first && slot < first + count)
      continue;                       /* the promoted sprites themselves */
    const int slot_priority =
        (frame->oam.data[slot * 2 + 1] >> 12) & 3;
    SrPpuObjResolveResult sb;
    if (!ActRaiser_ResolvePpuObjRange(
            (uint8_t)slot, 1, (uint8_t)slot_priority, &sb))
      continue;
    /* Cheap reject before rasterising: most slots cannot touch the icon. */
    if (sb.x1 <= bounds.x0 || sb.x0 >= bounds.x1 ||
        sb.y1 <= bounds.y0 || sb.y0 >= bounds.y1)
      continue;
    const int sw = sb.x1 - sb.x0, sh = sb.y1 - sb.y0;
    if (sw <= 0 || sh <= 0 ||
        sw > kActRaiserHudIconRasterLimit || sh > kActRaiserHudIconRasterLimit)
      continue;
    SrPpuObjRasterResult raster;
    if (!ActRaiser_RasterizePpuObjRange(
            (uint8_t)slot, 1, (uint8_t)slot_priority,
            s_hud_restore_slot, (size_t)sw * sizeof(uint32_t),
            sizeof(s_hud_restore_slot), &raster))
      continue;
    for (int y = 0; y < sh; y++) {
      const int fy = sb.y0 + y - bounds.y0;
      if (fy < 0 || fy >= raster_height) continue;
      for (int x = 0; x < sw; x++) {
        const int fx = sb.x0 + x - bounds.x0;
        if (fx < 0 || fx >= raster_width) continue;
        const size_t fi = (size_t)fy * raster_width + fx;
        if (s_hud_restore_prio[fi] != kActRaiserHudRestoreNone)
          continue;                   /* an earlier slot already won here */
        const uint32_t px = s_hud_restore_slot[(size_t)y * sw + x];
        if (!px) continue;
        s_hud_restore_argb[fi] = px;
        s_hud_restore_prio[fi] = (uint8_t)slot_priority;
      }
    }
  }
}

static void ActRaiser_DioramaHudObjFinish(int width) {
  extern uint8_t *g_diorama_layer_pixels[];
  extern uint8_t g_hud_obj_pixels[];

  /* Bound against the PLANE width -- the wider of the two destinations, and the
   * one the apron grew. Both surfaces are allocated kPpuSurfaceWidth wide. */
  if (!s_hud_icon_ready || width <= 0 ||
      width + SR_PPU_OBJ_APRON * 2 > SR_PPU_SURFACE_MAX_WIDTH)
    return;


  const int raster_width = s_hud_icon_bounds.x1 - s_hud_icon_bounds.x0;
  const int raster_height = s_hud_icon_bounds.y1 - s_hud_icon_bounds.y0;
  /* Band index == OAM priority (ppu.c's priority-split resolve does
   * band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM priority in those two
   * bits), so this is the same plane the diorama's kPrioBands table bound. */
  uint32_t *plane = (uint32_t *)g_diorama_layer_pixels[
      ActRaiser_DioramaObjPlaneForPriority(s_hud_icon_priority)];
  const size_t pitch = (size_t)width * 4;
  const int extra = (width - kActRaiserAuthenticWidth) / 2;
  /* TWO destinations, TWO widths. g_hud_obj_pixels stays at the DISPLAY width
   * (it is a screen-anchored overlay); the diorama OBJ planes are bound
   * APRON-wide. One `width` indexing both put the punch-out at the wrong stride
   * and the wrong column, so the promoted icon was never erased from the tilted
   * plane -- it rode the OBJ plane into the scene as a full-size sprite (the
   * priority-3 fire icon floating mid-scene, measured at gf1636 of
   * saves/artifacts2.rec).
   *
   * The plane side goes through the apron geometry rather than re-deriving it,
   * so this function and the apron pass cannot disagree about where a screen
   * column lands -- disagreeing is exactly what the bug WAS. */
  const ActionApronGeometry plane_geom = { extra, SR_PPU_OBJ_APRON };
  const int plane_width = ActionApron_SurfaceWidth(&plane_geom);
  /* Two destinations, two row origins. g_hud_obj_pixels is consumed in
   * authentic screen space (the promoted HUD overlay), so it indexes by
   * screen_y. The diorama PLANE is consumed in capture space, whose row 0 is
   * screen y = -g_ws_extra_top, so the hole punched in it must carry that
   * offset -- otherwise the icon is erased from the wrong rows and a ghost of
   * it stays in the tilted OBJ plane. Zero without a vertical margin. */
  extern int g_ws_extra_top;
  const int plane_row_bias = g_ws_extra_top;

  for (int y = 0; y < raster_height; y++) {
    const int screen_y = s_hud_icon_bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kActRaiserAuthenticHeight) continue;
    const uint32_t *hud_src = (const uint32_t *)(
        g_hud_obj_pixels + (size_t)screen_y * pitch);
    for (int x = 0; x < raster_width; x++) {
      const int texture_x = s_hud_icon_bounds.x0 + x + extra;
      if (texture_x < 0 || texture_x >= width) continue;
      const uint32_t pixel = hud_src[texture_x];
      if (!pixel) continue;
      const size_t plane_index =
          (size_t)(screen_y + plane_row_bias) * plane_width +
          ActionApron_SurfaceColumn(&plane_geom, s_hud_icon_bounds.x0 + x);
      if (plane && !s_hud_icon_ppu_relocated) plane[plane_index] = 0;
      /* Hand the pixel back to whatever the icon was covering, in ITS band --
       * the capture never recorded it, because the icon won the shared OBJ
       * z-test here. Only inside the icon's OPAQUE footprint: where the icon
       * was transparent the real capture already placed the right pixel in the
       * right band. Ordered after the zero above so a restore into the icon's
       * own band survives it. */
      const size_t footprint_index = (size_t)y * raster_width + x;
      const uint8_t restore_priority = s_hud_restore_prio[footprint_index];
      if (!s_hud_icon_ppu_relocated &&
          restore_priority != kActRaiserHudRestoreNone) {
        uint32_t *restore_plane = (uint32_t *)g_diorama_layer_pixels[
            ActRaiser_DioramaObjPlaneForPriority(restore_priority)];
        if (restore_plane)
          restore_plane[plane_index] = s_hud_restore_argb[footprint_index];
      }
    }
  }
}

/* Promote Death Heim's BG2 statues and attach their seven red-eye ornaments.
 *
 * The ROM models the eyes as priority-2 sprites so it can blink them without
 * rewriting the background. That is indistinguishable from being painted in
 * the sockets in flat presentation, but Diorama correctly gives OBJ and BG2
 * different depth transforms: the eyes then float in front of the faces. The
 * room manifest promotes the face band to BG2Far at focal z. Because 0701 is
 * native-only, the upper BG2 rows are split after scanout; then only the pixels
 * won by the complete measured eye range move from OBJ2 to that same surface.
 * Gameplay OAM, native composition, the water, portals, player, and every
 * other room stay untouched.
 *
 * Comparing the rasterized range against the captured OBJ pixel is important:
 * if an earlier OAM slot ever covers an eye, that earlier winner remains on
 * its own OBJ plane instead of being mistaken for part of the statue. */
enum {
  kDeathHeimHubFaceRows = 9 * 16,
  kDeathHeimHubEyeRasterMaxHeight = 128,
};
static uint32_t s_death_heim_hub_eye_raster[
    kActRaiserAuthenticWidth * kDeathHeimHubEyeRasterMaxHeight];
static bool s_death_heim_hub_faces_promoted;

bool ActRaiser_DioramaDeathHeimHubFacesPromoted(void) {
  return s_death_heim_hub_faces_promoted;
}

static void ActRaiser_DioramaDeathHeimHubStatuesFinish(int width) {
  extern bool g_diorama_frame_active;
  extern int g_ws_extra_top;
  extern uint8_t *g_diorama_layer_pixels[];
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();

  s_death_heim_hub_faces_promoted = false;
  if (!frame || !g_diorama_frame_active ||
      g_ram[kActRaiserWram_MapGroup] != kActRaiserMapGroup_DeathHeim ||
      g_ram[kActRaiserWram_CurrentMap] != kActRaiserDeathHeimMap_Hub ||
      width < kActRaiserAuthenticWidth)
    return;

  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  if (!room || !DioramaLayerOrder_VirtualLayerHasClassification(
                   &room->virtual_layers[1]))
    return;

  uint32_t *bg2 =
      (uint32_t *)g_diorama_layer_pixels[SR_PPU_OVERLAY_BG2];
  uint32_t *bg2_hi =
      (uint32_t *)g_diorama_layer_pixels[kDioramaPlane_Bg2Hi];
  uint32_t *faces =
      (uint32_t *)g_diorama_layer_pixels[kDioramaPlane_Bg2Far];
  if (!bg2 || !bg2_hi || !faces) return;

  const int extra = (width - kActRaiserAuthenticWidth) / 2;
  const ActionApronGeometry geom = { extra, SR_PPU_OBJ_APRON };
  const int plane_width = ActionApron_SurfaceWidth(&geom);
  /* 0701 is deliberately native-only (docs/rendering-engine.md): its BG2
   * cannot acquire virtual-band metadata from the world provider. Perform the
   * same authored cell split on the already-isolated pixels instead. Rows
   * 0..8 are statues/faces; row 9 begins the divider/fog/water. Both authentic
   * priority bands feed one focal plane because the distinction here is
   * semantic depth, not SNES paint order. */
  uint32_t *sources[] = { bg2, bg2_hi };
  for (size_t source = 0; source < sizeof(sources) / sizeof(sources[0]);
       source++) {
    for (int screen_y = 0; screen_y < kDeathHeimHubFaceRows; screen_y++) {
      const size_t row =
          (size_t)(screen_y + g_ws_extra_top) * plane_width;
      for (int screen_x = 0; screen_x < kActRaiserAuthenticWidth;
           screen_x++) {
        const int column = ActionApron_SurfaceColumn(&geom, screen_x);
        if (column < 0 || column >= plane_width) continue;
        const size_t index = row + column;
        const uint32_t pixel = sources[source][index];
        if (!pixel) continue;
        faces[index] = pixel;
        sources[source][index] = 0;
        s_death_heim_hub_faces_promoted = true;
      }
    }
  }

  const int first = ActRaiser_FindDeathHeimHubEyes(
      frame->oam.data, frame->high_oam.data, SR_PPU_OAM_WORD_COUNT / 2);
  if (first < 0) return;

  enum { kEyePriority = 2 };
  SrPpuObjResolveResult bounds;
  if (!ActRaiser_ResolvePpuObjRange(
          (uint8_t)first, kActRaiserDeathHeimHubEyeOamCount,
          kEyePriority, &bounds))
    return;
  const int raster_width = bounds.x1 - bounds.x0;
  const int raster_height = bounds.y1 - bounds.y0;
  if (raster_width <= 0 || raster_width > kActRaiserAuthenticWidth ||
      raster_height <= 0 ||
      raster_height > kDeathHeimHubEyeRasterMaxHeight || bounds.x0 < 0 ||
      bounds.x1 > kActRaiserAuthenticWidth)
    return;
  SrPpuObjRasterResult raster;
  if (!ActRaiser_RasterizePpuObjRange(
          (uint8_t)first, kActRaiserDeathHeimHubEyeOamCount,
          kEyePriority, s_death_heim_hub_eye_raster,
          (size_t)raster_width * sizeof(uint32_t),
          sizeof(s_death_heim_hub_eye_raster), &raster))
    return;

  uint32_t *eyes = (uint32_t *)g_diorama_layer_pixels[kDioramaPlane_Obj2];
  if (!eyes) return;

  for (int y = 0; y < raster_height; y++) {
    const int screen_y = bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kActRaiserAuthenticHeight) continue;
    for (int x = 0; x < raster_width; x++) {
      const uint32_t expected =
          s_death_heim_hub_eye_raster[(size_t)y * raster_width + x];
      if (!expected) continue;
      const int column = ActionApron_SurfaceColumn(&geom, bounds.x0 + x);
      if (column < 0 || column >= plane_width) continue;
      const size_t index =
          (size_t)(screen_y + g_ws_extra_top) * plane_width + column;
      const uint32_t captured = eyes[index];
      if (!captured ||
          (captured & 0x00ffffffu) != (expected & 0x00ffffffu))
        continue;
      faces[index] = captured;
      eyes[index] = 0;
    }
  }
}

ActionApronGeometry ActRaiser_ObjApronGeometry(void) {
  extern bool g_ws_active;
  extern int g_ws_extra;
  ActionApronGeometry g = { g_ws_extra, 0 };
  /* The same condition host_display.c uses to pin the margin budget to
   * kWsExtraMax. Read from settings + geometry rather than from a per-frame
   * diorama flag on purpose: the emitter runs during game logic, BEFORE
   * ActRaiserDrawPpuFrame sets g_diorama_frame_active, so a per-frame flag
   * would be one frame stale exactly when it matters. */
  if (g_settings.diorama_mode && g_ws_active)
    g.apron = SR_PPU_OBJ_APRON;
  return g;
}

/* Draw the apron part channel into the captured OBJ planes.
 *
 * Runs AFTER scanout, so the planes already hold this frame's in-window
 * sprites, and writes ONLY the two apron column bands. Never the display
 * window: those columns are scanout's, and writing them would both double-draw
 * a straddling part (with no z-test against the sprites it lost to) and break
 * the byte-identity gate this phase is judged on. The clip is structural --
 * PpuRasterizeParts takes the band as its `bounds` and crops to it.
 *
 * Ordering and band routing follow the hardware: OAM order decides who owns an
 * overlapping pixel via ONE shared z-test, and only the survivor's priority
 * decides which plane it lands on (see PpuWriteOverlayRenderLine's
 * priority-split resolve). So parts are drawn one at a time in list order and a
 * pixel already opaque in ANY of the four OBJ planes is left alone -- which is
 * exactly first-writer-wins across bands, with the planes themselves as the
 * claimed-set. That works because PpuClearOverlayRenderLine clears the full
 * bound pitch, apron included, every frame. */
static void ActRaiser_DioramaApronFinish(const ActionApronGeometry *geom) {
  extern uint8_t *g_diorama_layer_pixels[];
  extern int g_ws_extra_top;
  extern int g_ws_extra_bottom;

  if (!geom || geom->apron <= 0 || !ActRaiser_PpuFrame() ||
      !ActionApron_Count())
    return;

  /* AR_APRONLOG=1: the channel's sizing verdict. peak/overflow answer "is
   * kActionApronMaxParts right?" without guessing, which is what the plan asks
   * for instead of assuming a capacity. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_ApronLog))
    fprintf(stderr, "[apron] gf=%u parts=%d peak=%d overflow=%d\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            ActionApron_Count(), ActionApron_PeakCount(),
            ActionApron_Overflow());

  const int surface_width = ActionApron_SurfaceWidth(geom);
  if (surface_width > (int)SR_PPU_SURFACE_MAX_WIDTH)
    return;
  const SrPpuObjPart *parts = ActionApron_Parts();
  const int count = ActionApron_Count();
  const int rows = kActRaiserAuthenticHeight +
      g_ws_extra_top + g_ws_extra_bottom;

  /* Big enough for the largest SNES sprite (64x64). */
  static uint32_t scratch[64 * 64];

  int spans[2][2];
  ActionApron_LeftSpan(geom, &spans[0][0], &spans[0][1]);
  ActionApron_RightSpan(geom, &spans[1][0], &spans[1][1]);

  /* Resolved once, not per pixel: the claimed-set test below reads all four,
   * and re-deriving them inside the innermost loop made the plane mapping the
   * hottest thing in the pass. */
  uint32_t *planes[4];
  for (int p = 0; p < 4; p++)
    planes[p] = (uint32_t *)g_diorama_layer_pixels[
        ActRaiser_DioramaObjPlaneForPriority(p)];

  for (int i = 0; i < count; i++) {
    const SrPpuObjPart *part = &parts[i];
    const int priority = (part->tile_attr >> 12) & 3;
    uint32_t *plane = planes[priority];
    if (!plane)
      continue;
    const SrPpuOverlayCaptureState *obj_capture =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
    const bool color_math =
        obj_capture &&
        (obj_capture->flags & SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH) != 0u &&
        ActionApron_PartUsesColorMath(part->tile_attr);

    for (int band = 0; band < 2; band++) {
      /* Intersect the part with this apron band; skip when it does not reach. */
      const int win_x0 =
          part->x > spans[band][0] ? part->x : spans[band][0];
      const int win_x1 = part->x + part->size < spans[band][1]
          ? part->x + part->size : spans[band][1];
      if (win_x1 <= win_x0)
        continue;
      const int win_y0 = part->y;
      const int win_y1 = part->y + part->size;
      const int w = win_x1 - win_x0, h = win_y1 - win_y0;
      if (w <= 0 || h <= 0 || w > 64 || h > 64)
        continue;
      if (!ActRaiser_RasterizePpuObjParts(
              part, 1u, win_x0, win_y0, win_x1, win_y1,
              scratch, (size_t)w * sizeof(uint32_t), sizeof(scratch)))
        continue;

      /* The destination columns are a contiguous run, so resolve the base once
       * per part instead of mapping and bounds-checking every pixel. The run is
       * inside the surface by construction: `win` was clipped to an apron band,
       * and a band's columns are always within [0, surface_width). */
      const int base_col = ActionApron_SurfaceColumn(geom, win_x0);
      if (base_col < 0 || base_col + w > surface_width)
        continue;

      for (int y = 0; y < h; y++) {
        /* Plane rows are CAPTURE space: row 0 is screen y = -g_ws_extra_top,
         * the same bias ActRaiser_DioramaHudObjFinish applies. */
        const int row = win_y0 + y + g_ws_extra_top;
        if (row < 0 || row >= rows)
          continue;
        const size_t row_base = (size_t)row * surface_width + base_col;
        for (int x = 0; x < w; x++) {
          uint32_t pixel = scratch[(size_t)y * w + x];
          if (!pixel)
            continue;
          const size_t index = row_base + x;
          bool claimed = false;
          for (int p = 0; p < 4 && !claimed; p++)
            if (planes[p] && planes[p][index])
              claimed = true;
          if (claimed)
            continue;
          if (color_math)
            pixel = (pixel & 0x00ffffffu) | 0x80000000u;
          plane[index] = pixel;
        }
      }
    }
  }
}

/* Margin geometry of the last rendered frame,
 * latched at the end of ActRaiserDrawPpuFrame. See ActRaiser_LiveMargins. */
static int s_live_margin_left;
static int s_live_margin_right;
static int s_live_margin_top;
static int s_live_margin_bottom;
static ActionBgPlan s_live_action_bg_plan;
static bool s_live_bg_capture_pad_to_budget;

/* Sim3D reports renderer-local contract state; this host seam owns the policy
 * decision to end the session. Keeping that conversion beside the capture
 * orchestration prevents the low-level renderer from depending on app state. */
static void ActRaiser_ReportSim3DCaptureContractFailure(void) {
  switch (Sim3D_GetCaptureContractFailure()) {
    case kSim3DCaptureContract_RendererUnavailable:
      SessionFatal_Request(
          "Simulation town 3D is active, but its core renderer is unavailable. "
          "Restart after checking graphics memory and driver stability, or "
          "disable Simulation town 3D in settings.ini.");
      break;
    case kSim3DCaptureContract_SurfaceAllocation:
      SessionFatal_Request(
          "Simulation town 3D could not allocate or bind its core capture "
          "surfaces. Close other graphics-heavy applications and restart, or "
          "disable Simulation town 3D in settings.ini.");
      break;
    case kSim3DCaptureContract_ObjectSourcesUnavailable:
      SessionFatal_Request(
          "Simulation town 3D lost both of its object-rendering sources. "
          "Restart the game. If this repeats, disable Simulation object "
          "billboards in settings.ini and report the affected town/frame.");
      break;
    case kSim3DCaptureContract_Ok:
    default:
      break;
  }
}

typedef struct ActRaiserPpuScanoutContext {
  unsigned shape_game_frame;
  bool shape_trace;
  ActRaiserPpuShapeRegisters shape_before;
} ActRaiserPpuScanoutContext;

static void ActRaiser_PpuScanoutLineCallback(
    void *user_data, const SrPpuScanoutLineContext *context) {
  ActRaiserPpuScanoutContext *scanout = user_data;
  if (!scanout || !context) return;
  if ((context->flags & SR_PPU_SCANOUT_LINE_BEFORE) != 0u) {
    if (context->line > 0u)
      ActRaiserActionBg_ObserveRoomSceneFrameLine(
          &context->state, context->line - 1u);
    if (scanout->shape_trace)
      ActRaiser_PpuShapeCaptureRegisters(
          &context->state, &scanout->shape_before);
  } else if ((context->flags & SR_PPU_SCANOUT_LINE_AFTER_HDMA) != 0u &&
             scanout->shape_trace) {
    ActRaiser_PpuShapeTraceLine(
        scanout->shape_game_frame, (int)context->line,
        &scanout->shape_before, context);
  }
}

static void ActRaiser_PpuScanoutIrqCallback(
    void *user_data, uint32_t line) {
  CpuRegSnapshot snap;
  const bool observe_interrupt =
      RtlGameEventEnabled(SR_EVENT_MASK_INTERRUPT);
  const uint32 interrupt_pc =
      observe_interrupt ? ActRaiser_LastBlockPc() : 0u;
  const uint16 interrupt_vector =
      g_cpu.emulation ? 0xfffeu : 0xffeeu;
  (void)user_data;
  ActRaiser_SaveRegs(&g_cpu, &snap);
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_IRQ, SR_EVENT_INTERRUPT_ENTER, interrupt_pc,
        interrupt_vector, (int32_t)line, "irq");
  }
  cpu_push_interrupt_frame(&g_cpu);
  g_sr_in_interrupt = 1;
  IrqHandler_M1X1(&g_cpu);
  g_sr_in_interrupt = 0;
  ActRaiser_RestoreRegs(&g_cpu, &snap);
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_IRQ, SR_EVENT_INTERRUPT_EXIT, interrupt_pc,
        interrupt_vector, (int32_t)line, "irq");
  }
}

static SrResult ActRaiser_DrawPpuFrameTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context);

void ActRaiserDrawPpuFrame(void) {
  const uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool action = ActRaiser_IsActionMapGroup(map_group);
  const bool sim_town = ActRaiser_IsSimulationTown(map_group, map_number);

  /* Overlay bindings are host-owned and persistent; capture policy is
   * game-owned and rebuilt every frame so no prior mode can leak a region. */
  if (Sim3D_BeginFrame())
    ActRaiser_RebindPpuOutputSurfaces();
  if (!ActRaiser_ResetPpuFrameCaptures()) {
    SessionFatal_Request(
        "The runner could not reset frame capture policy. Restart after "
        "rebuilding the game and runner together.");
  }
  /* Exact-position overrides have two owners. Action rebuilds them during
   * its object scan and deliberately keeps them over pause/freeze redraws.
   * A sim town rebuilds its own sideband during the composition pass and must
   * likewise keep it over a redraw where the emulated game did not advance.
   * Any other scene clears the prior owner's positions: a stale action value
   * can resurrect a parked sim slot, while a stale sim value can displace the
   * next non-town scene (ledger §34). */
  const ActRaiserExactPositionOwner expected_owner = action
      ? kActRaiserExactPositionOwner_Action
      : sim_town ? kActRaiserExactPositionOwner_Sim
                 : kActRaiserExactPositionOwner_None;
  if (expected_owner == kActRaiserExactPositionOwner_None ||
      ActRaiser_GetExactPositionOwner() != expected_owner) {
    if (!ActRaiser_ClearPpuObjMetadata()) {
      SessionFatal_Request(
          "The runner could not clear stale object metadata. Restart after "
          "rebuilding the game and runner together.");
    }
    ActRaiser_MarkExactPositionOwner(kActRaiserExactPositionOwner_None);
  }
  ActRaiser_ApplyWidescreenPolicy();
  /* Stage D reconnaissance: read-only classification of objects that intersect
   * a live side margin but remain outside the authentic activation window. */
  ActRaiser_WidescreenSpriteActivationProbe();
  /* SPEC-bg-hle BH2 differential observer. Default-off and read-only: even
   * when enabled it compares the pure WRAM world against the native ring but
   * never supplies a tile to scanout. */
  ActRaiserActionBg_ObserveFrame(g_ram, kActRaiserWramSize);
  /* Sky Palace: synthesize only BG2's offscreen margin columns from its ROM
   * source page. The paired restore after scanout preserves UI staging. */
  ActRaiser_WidescreenSkyPalacePrepare(s_runner);
  ActRaiser_WidescreenHudObjPromote();
  /* Manifest-driven HD substitutions (game-assets/manifest.ini) — e.g. the
   * settled title logo. Runs after the HUD/OAM capture policies so a busy
   * source is detected rather than clobbered; entries without host-loaded
   * art never request captures, keeping headless/oracle output authentic. */
  HdReplacements_EvaluateFrame();

  if (!s_runner || !s_runner_api ||
      s_runner_api->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !s_runner_api->visit_ppu_frame_transaction) {
    SessionFatal_Request(
        "The runner does not provide coherent PPU frame access. Restart "
        "after rebuilding the game and runner together.");
    return;
  }
  const SrPpuFrameTransactionRequest request = {
    .struct_size = sizeof(request),
    .callback = ActRaiser_DrawPpuFrameTransaction,
  };
  if (s_runner_api->visit_ppu_frame_transaction(
          s_runner, &request) != SR_RESULT_OK) {
    SessionFatal_Request(
        "The runner rejected ActRaiser's PPU frame transaction. Restart "
        "after rebuilding the game and runner together.");
  }
}

static SrResult ActRaiser_DrawPpuFrameTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context) {
  const uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool action = ActRaiser_IsActionMapGroup(map_group);
  const SrPpuStateSnapshot *ppu;
  extern bool Diorama_IsActiveThisFrame(void);
  const bool profile_diorama = action && Diorama_IsActiveThisFrame();
  DioramaPerformanceScope producer_setup_performance = {0};
  ActRaiserPpuFrameAccess frame_access = {
    .context = context,
  };
  (void)user_data;
  (void)runner;
  if (!context) return SR_RESULT_INVALID_ARGUMENT;
  ppu = &context->state;
  for (uint32_t source = 0; source < SR_PPU_OVERLAY_SOURCE_COUNT; source++)
    frame_access.captures[source] =
        ActRaiser_OverlayCaptureState(&context->frame.overlays[source]);
  s_ppu_frame_access = &frame_access;
  if (profile_diorama)
    producer_setup_performance =
        DioramaPerformance_Begin(kDioramaPerformance_ProducerSetup);
  ActRaiser_ClearWidescreenMarginGaps(
      s_pending_action_bg_plan.bound_canvas_to_world, context);

  /* Diorama per-layer capture: when active (D toggle) or armed for a one-shot
   * dump (Shift+D), override all existing capture policies with full-frame
   * RemoveFromGame captures for BG1/2/3/OBJ. Bind dedicated diorama buffers
   * so we don't collide with the HUD/HD overlay surfaces. The captures
   * overwrite whatever the widescreen HUD split and HD replacements set
   * above — mutual exclusion for this frame. */
  {
    extern bool Diorama_IsActiveThisFrame(void);
    extern bool g_diorama_dump_pending;
    extern bool g_diorama_frame_active;
    extern uint8_t *g_diorama_layer_pixels[];
    bool active = Diorama_IsActiveThisFrame();
    bool want_capture = active ||
        (g_diorama_dump_pending &&
         ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]));
    g_diorama_frame_active = active;
    if (want_capture) {
      extern bool g_ws_active;
      extern int g_ws_extra;
      extern int g_ws_extra_top;
      extern int g_ws_extra_bottom;
      int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      /* Apron-wide, matching the main framebuffer bind: the capture rect stays
       * scanline-bounded (the scanline path cannot fill apron columns), but the
       * wider pitch makes PpuWriteOverlayRenderLine's texture_extra centre the
       * captured span, leaving the apron columns free for capture-time part
       * rasterization to fill. */
      size_t pitch = ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON);
      /* Capture rectangles are expressed in AUTHENTIC screen space, so
       * the vertical band starts at a negative y exactly as the side
       * margins start at -g_ws_extra. The PPU maps that onto row 0 of the
       * destination surface (PpuOutputRow), so a plane's texture row 0 is
       * screen y = -g_ws_extra_top -- the transpose of column 0 meaning
       * screen x = -g_ws_extra. */
      int capture_height =
          kActRaiserAuthenticHeight + g_ws_extra_top + g_ws_extra_bottom;
      /* A7/A5 (followup doc): BG3 (the status bar) is excluded from this
       * diorama capture loop whenever diorama_hud_flat is on (default) —
       * leaving the line-906 widescreen HUD split capture (PpuSetOverlayCapture
       * ... SR_PPU_OVERLAY_BG3 ... RemoveFromGame, above) standing instead
       * of being overridden by this block. That capture feeds
       * g_hud_bg_pixels/g_hud_bg_texture exactly as in flat mode, which is
       * what lets PresentCompositeScene's diorama branch call
       * PresentHudOverlayComposited (present.c) and get the same widescreen
       * HUD anchoring (ACT/TIME/SCORE spread, boss-health full width) flat
       * mode already has. Before A7 BG3 was unconditionally rebound here, so
       * the last bind won — the anchored capture never survived and the HUD
       * only ever showed as an unanchored tilted plane.
       *
       * diorama_hud_flat=false (A5's A/B option) restores that pre-A7
       * behavior on purpose: BG3 captured here as an ordinary diorama layer,
       * rendered as the tilted plane (diorama.c's kDioramaLayers table), with
       * no anchored overlay. This is a game-thread read of the setting — the
       * present-side choice (whether to call PresentHudOverlayComposited)
       * uses the FrameSlot-snapshotted copy per D6. */
      static const uint32_t kCaptureLayersCommon[] = {
        SR_PPU_OVERLAY_BG1, SR_PPU_OVERLAY_BG2, SR_PPU_OVERLAY_OBJ,
      };
      /* Diorama owns visual sources, not just the SNES main screen. Marahna
       * keeps BG1 and OBJ exclusively on TS ($11) and full-adds them to
       * main-screen BG2 ($06); gating on TM alone produced an empty BG1 plane
       * and no sprites even though the native composite was healthy. The PPU
       * overlay exporter prefers TM and falls back to TS for a subscreen-only
       * BG, so the union is the frontend's correct eligibility test. */
      const uint8_t capture_screens =
          (uint8_t)(ppu->main_screen | ppu->sub_screen);
      const uint8_t full_add_sub_sources =
          DioramaCaptureBlend_FullAddSubscreenSources(
              ppu->color_math_control, ppu->color_math_designation,
              ppu->main_screen, ppu->sub_screen);
      uint8_t layer_group = 0, layer_map = 0;
      uint8_t layer_section = kDioramaLayerSection_Room;
      const bool layer_room_live = Diorama_LiveRoom(
          &layer_group, &layer_map, &layer_section);
      /* F4 (2026-07-26 handback: "missing transparency on background layers in
       * diorama mode"). SNES colour math is not reproduced by the capture, so a
       * half-added BG used to arrive fully opaque and HIDE the planes behind it
       * instead of tinting them. Annotate those planes so the compositor draws
       * them at 50% instead — see kPpuOverlayFlag_MarkBgHalfAdd.
       *
       * Half-add is represented by alpha. A disjoint full-add scene is instead
       * tagged as a subscreen addend: the PPU capture masks each source to the
       * resolved TS winner, and the compositor uses additive blending. Measured
       * respectively in Fillmore act 2 ($02/$43) and Marahna act 1 ($02/$03,
       * TM=$06, TS=$11). Subtract and overlapping full-add ownership still fail
       * closed rather than guessing.
       *
       * The policy itself lives in diorama_capture_blend.c so it can be tested
       * without a ROM or a renderer. */
      for (int i = 0; i < (int)(sizeof(kCaptureLayersCommon) /
                                sizeof(kCaptureLayersCommon[0])); i++) {
        uint32_t src = kCaptureLayersCommon[i];
        if (!g_diorama_layer_pixels[src])
          g_diorama_layer_pixels[src] = calloc(
              1, SR_PPU_SURFACE_MAX_WIDTH * 4 *
                     kHostDisplayFramebufferHeight);
        ActRaiser_BindPpuOutput(
            SR_PPU_OUTPUT_OVERLAY, src, 0u,
            g_diorama_layer_pixels[src], pitch,
            kHostDisplayFramebufferHeight);
        if (capture_screens & (1 << src)) {
          uint32_t flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME;
          if (full_add_sub_sources & (1 << src))
            flags |= SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN;
          /* OBJ keeps its own per-palette-group flag; this one is BG-only. */
          if (src != SR_PPU_OVERLAY_OBJ &&
              DioramaCaptureBlend_LayerIsHalfAdded(
                  ppu->color_math_control,
                  ppu->color_math_designation,
                  ppu->sub_screen, (uint8_t)(1 << src))) {
            flags |= SR_PPU_OVERLAY_MARK_BG_HALF_ADD;
            /* Once per source: this is a fidelity change to the captured image,
             * so it should be visible in a log rather than inferred from
             * pixels. Silent on every stage that does not use this math.
             * AR_DIORAMA_BLEND_LOG=1 makes it per-frame instead, which is how
             * to tell a stage that never qualifies from one that qualifies only
             * on some frames (CGWSEL/CGADSUB are HDMA-writable per scanline, so
             * the value at capture-setup time is not necessarily the value
             * during scanout). */
            static bool reported[SR_PPU_OVERLAY_SOURCE_COUNT];
            static int verbose = -1;
            if (verbose < 0) verbose = getenv("AR_DIORAMA_BLEND_LOG") ? 1 : 0;
            if (!reported[src] || verbose) {
              reported[src] = true;
              fprintf(stderr,
                      "[diorama-blend] gf=%u BG%d half-added with subscreen "
                      "(cgwsel=$%02x cgadsub=$%02x main=$%02x sub=$%02x) "
                      "-> captured at 50%% alpha\n",
                      ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                      src + 1, ppu->color_math_control,
                      ppu->color_math_designation,
                      ppu->main_screen, ppu->sub_screen);
            }
          }
          /* Bloodpool 0204 dims BG1 with full fixed-colour subtraction
           * (cgwsel=$00 cgadsub=$81). Alpha cannot express subtraction, so ask
           * the PPU capture to bake it into this isolated plane in native
           * 5-bit colour space. */
          if (src != SR_PPU_OVERLAY_OBJ &&
              DioramaCaptureBlend_LayerUsesFixedColorSubtract(
                  ppu->color_math_control,
                  ppu->color_math_designation,
                  ppu->fixed_color, (uint8_t)(1 << src))) {
            flags |= SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT;
          }
          ActRaiser_SetPpuOverlayCapture(
              src, -g_ws_extra, -g_ws_extra_top,
              width, capture_height, flags);
          if (flags & SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN) {
            static bool reported_full_add;
            static int verbose_full_add = -1;
            if (verbose_full_add < 0)
              verbose_full_add = getenv("AR_DIORAMA_BLEND_LOG") ? 1 : 0;
            if (!reported_full_add || verbose_full_add) {
              reported_full_add = true;
              fprintf(stderr,
                      "[diorama-blend] gf=%u full subscreen add "
                      "(cgwsel=$%02x cgadsub=$%02x main=$%02x sub=$%02x) "
                      "-> resolved TS sources=$%02x use additive planes\n",
                      ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                      ppu->color_math_control,
                      ppu->color_math_designation,
                      ppu->main_screen, ppu->sub_screen,
                      full_add_sub_sources);
            }
          }
        }
        /* Backing is presentation policy even when this particular live BG is
         * disabled: a named ROM skybox can source BG1/BG2 independently of the
         * current room's screen-enable bits. Geometry and fill setup are
         * deliberately order-independent within this per-frame policy. */
        if (layer_room_live && (src == SR_PPU_OVERLAY_BG1 ||
                                src == SR_PPU_OVERLAY_BG2)) {
          DioramaTransparentFill fill = kDioramaTransparentFill_None;
          uint8_t fill_cgram = 0;
          if (DioramaLayerOrder_ResolveTransparentFill(
                  Diorama_LayerOverrides(), layer_group, layer_map,
                  layer_section, src, &fill, &fill_cgram)) {
            SrPpuTransparentFillMode mode =
                SR_PPU_TRANSPARENT_FILL_NONE;
            if (fill == kDioramaTransparentFill_Black)
              mode = SR_PPU_TRANSPARENT_FILL_BLACK;
            else if (fill == kDioramaTransparentFill_Cgram)
              mode = SR_PPU_TRANSPARENT_FILL_CGRAM;
            /* Unknown/corrupt values fail transparent rather than silently
             * becoming black and hiding a platform-specific capture fault.
             * Setting None is still significant: it carries authored Off into
             * the immutable frame snapshot used by ROM backdrop composition. */
            ActRaiser_SetPpuOverlayFill(src, mode, fill_cgram);
          }
        }
      }
      /* BG3 needs its OWN branch, not just an on/off entry in the loop above:
       * PpuBindOverlaySurface is the ONLY thing that changes a source's
       * bound destination buffer, so a source simply left OUT of a frame's
       * capture list keeps whatever buffer the LAST frame bound it to —
       * there is no implicit "unbind." With diorama_hud_flat=true excluding
       * BG3 from a shared loop (as A7 originally did), toggling
       * flat->tilted->flat left BG3 permanently bound to the diorama layer
       * buffer from the tilted frame: g_hud_bg_pixels silently stopped
       * receiving fresh captures (frozen HUD) while the diorama buffer kept
       * getting live writes and diorama.c kept drawing it — the HUD showing
       * in two places at once (a live tilted ghost plus a frozen flat
       * overlay). Explicitly rebinding BOTH ways, every frame, is
       * self-healing regardless of toggle history. */
      if (g_settings.diorama_hud_flat) {
        extern uint8_t g_hud_bg_pixels[];
        /* The NARROW pitch, deliberately -- not the apron-wide `pitch` the
         * diorama planes bind at. This surface is not a diorama plane: it feeds
         * the anchored flat HUD overlay, which present.c uploads at
         * snes_width*4 (PresentUpload's hud rect). Binding it apron-wide made
         * the PPU write rows 2*kPpuObjApron columns apart while the upload read
         * them snes_width apart, shearing the HUD across the top of the screen.
         * The apron is resolve headroom for content that slides in past a
         * tilted plane's edge; a screen-anchored HUD has no such edge. */
        ActRaiser_BindPpuOutput(
            SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u,
            g_hud_bg_pixels, (size_t)width * 4,
            kHostDisplayFramebufferHeight);
        /* Do NOT issue the generic wide capture here — the line-906
         * HUD-split-specific capture region (0,0,kActRaiserAuthenticWidth,
         * hud_split_height) stays the authority for this source's X range;
         * that wide path is only for the diorama's OWN layers.
         *
         * BG3 carries more than the status bar, though: the act-title card
         * ("FILLMORE / ACT-1", BG3 tilemap rows 8/10 -> y=64..88) and the
         * pause text are the same layer, just below the split. In flat mode
         * they stay in the game framebuffer and are simply visible. In
         * diorama mode that framebuffer becomes the BACKDROP plane, drawn
         * first and then painted over by the BG2/BG1/OBJ planes — the text
         * silently disappeared behind the scene. Extend the SAME capture
         * rectangle down the full authentic height so those rows land in
         * g_hud_bg_pixels too; present.c draws everything below
         * hud_split_height as one flat, centered "body" chunk (the HUD
         * split's three anchored bands are driven by wsHudSplitHeight, not
         * by this rectangle, so their geometry is unchanged).
         *
         * When widescreen policy supplied a split, extend that capture. In
         * an intentionally unsplit mode such as Wide Raw, flat Diorama still
         * needs a full BG3 capture so the HUD can be composited in native
         * screen space rather than inheriting the scene perspective. The
         * present side recognizes split-height zero plus this full capture as
         * one unsplit/native chunk. */
        const SrPpuOverlayCaptureState *bg3_capture =
            ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG3);
        if (bg3_capture->y1 > bg3_capture->y0 &&
            bg3_capture->y1 < kActRaiserAuthenticHeight)
          ActRaiser_SetPpuOverlayCapture(
              SR_PPU_OVERLAY_BG3, bg3_capture->x0, bg3_capture->y0,
              bg3_capture->x1 - bg3_capture->x0,
              kActRaiserAuthenticHeight - bg3_capture->y0,
              bg3_capture->flags);
        else if (bg3_capture->y1 <= bg3_capture->y0 &&
                 (capture_screens & (1 << SR_PPU_OVERLAY_BG3)))
          ActRaiser_SetPpuOverlayCapture(
              SR_PPU_OVERLAY_BG3, 0, 0, kActRaiserAuthenticWidth,
              kActRaiserAuthenticHeight,
              SR_PPU_OVERLAY_REMOVE_FROM_GAME);
      } else {
        if (!g_diorama_layer_pixels[SR_PPU_OVERLAY_BG3])
          g_diorama_layer_pixels[SR_PPU_OVERLAY_BG3] =
              calloc(1, SR_PPU_SURFACE_MAX_WIDTH * 4 *
                            kHostDisplayFramebufferHeight);
        ActRaiser_BindPpuOutput(
            SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u,
            g_diorama_layer_pixels[SR_PPU_OVERLAY_BG3], pitch,
            kHostDisplayFramebufferHeight);
        if (capture_screens & (1 << SR_PPU_OVERLAY_BG3))
          ActRaiser_SetPpuOverlayCapture(
              SR_PPU_OVERLAY_BG3, -g_ws_extra, -g_ws_extra_top,
              width, capture_height,
              SR_PPU_OVERLAY_REMOVE_FROM_GAME |
                  ((full_add_sub_sources & (1 << SR_PPU_OVERLAY_BG3))
                       ? SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN : 0u));
      }
      if (capture_screens & (1 << SR_PPU_OVERLAY_OBJ))
        ActRaiser_SetPpuOverlayOamRange(0, 128);
      /* Priority-band splits: scanout routes each captured pixel to the
       * surface matching its hardware priority (Mode-1 tile priority bit for
       * BGs, the 2-bit OAM priority for sprites), so the diorama can draw
       * the true Mode-1 interleave — foreground tiles over sprites, low
       * priority sprites behind the playfield. Bound after their primaries
       * because a primary rebind drops the band family. */
      static const struct { uint32_t src; int band; int plane; }
      kPrioBands[] = {
        { SR_PPU_OVERLAY_BG1, 1, kDioramaPlane_Bg1Hi },
        { SR_PPU_OVERLAY_BG2, 1, kDioramaPlane_Bg2Hi },
        { SR_PPU_OVERLAY_BG1, 2, kDioramaPlane_Bg1Far },
        { SR_PPU_OVERLAY_BG2, 2, kDioramaPlane_Bg2Far },
        { SR_PPU_OVERLAY_OBJ, 1, kDioramaPlane_Obj1 },
        { SR_PPU_OVERLAY_OBJ, 2, kDioramaPlane_Obj2 },
        { SR_PPU_OVERLAY_OBJ, 3, kDioramaPlane_Obj3 },
      };
      for (int i = 0; i < (int)(sizeof(kPrioBands) / sizeof(kPrioBands[0])); i++) {
        if (kPrioBands[i].plane == kDioramaPlane_Bg1Far ||
            kPrioBands[i].plane == kDioramaPlane_Bg2Far) {
          const DioramaRoomOverride *virtual_room =
              ActRaiser_CurrentVirtualLayerRoom();
          const int virtual_bg =
              kPrioBands[i].plane == kDioramaPlane_Bg1Far ? 0 : 1;
          if (!virtual_room ||
              !DioramaLayerOrder_VirtualLayerIsAuthored(
                  &virtual_room->virtual_layers[virtual_bg]))
            continue;
        }
        if (!g_diorama_layer_pixels[kPrioBands[i].plane])
          g_diorama_layer_pixels[kPrioBands[i].plane] =
              calloc(1, SR_PPU_SURFACE_MAX_WIDTH * 4 *
                            kHostDisplayFramebufferHeight);
        ActRaiser_BindPpuOutput(
            SR_PPU_OUTPUT_OVERLAY_PRIORITY, kPrioBands[i].src,
            (uint32_t)kPrioBands[i].band,
            g_diorama_layer_pixels[kPrioBands[i].plane], pitch,
            kHostDisplayFramebufferHeight);
      }
    }
  }

  /* Flat presentation has one already-composited framebuffer, so BG-local
   * enhancements need the PPU's real priority winners as occlusion masks.
   * Diorama owns isolated planes and inserts effects directly after BG1/BG2;
   * a one-shot dump likewise owns these capture slots. HD replacements get
   * first refusal above—never overwrite another source policy. */
  extern bool g_diorama_frame_active;
  extern bool g_diorama_dump_pending;
  const bool action_effects_enabled =
      g_settings.action_effect_lighting || g_settings.action_effect_particles;
  if (action_effects_enabled &&
      !g_diorama_frame_active && !g_diorama_dump_pending &&
      ActionSceneEffects_RoomUsesBg1Decorations(
          g_ram, kActRaiserWramSize)) {
    const SrPpuOverlayCaptureState *bg1 =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG1);
    if (bg1->x1 <= bg1->x0 || bg1->y1 <= bg1->y0) {
      extern int g_ws_extra;
      extern uint8_t g_action_bg1_mask_pixels[];
      const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      if (ActRaiser_BindPpuOutput(
              SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG1, 0u,
              g_action_bg1_mask_pixels, (size_t)width * 4,
              kHostDisplayFramebufferHeight)) {
        ActRaiser_SetPpuOverlayCapture(
            SR_PPU_OVERLAY_BG1, -g_ws_extra, 0,
            width, kActRaiserAuthenticHeight,
            SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER);
      }
    }
  }
  if (action_effects_enabled &&
      !g_diorama_frame_active && !g_diorama_dump_pending &&
      map_group == kActRaiserMapGroup_Aitos &&
      map_number >= 2 && map_number <= 3) {
    const SrPpuOverlayCaptureState *bg2 =
        ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG2);
    if (bg2->x1 <= bg2->x0 || bg2->y1 <= bg2->y0) {
      extern int g_ws_extra;
      extern uint8_t g_action_bg2_mask_pixels[];
      const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      if (ActRaiser_BindPpuOutput(
              SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG2, 0u,
              g_action_bg2_mask_pixels, (size_t)width * 4,
              kHostDisplayFramebufferHeight)) {
        ActRaiser_SetPpuOverlayCapture(
            SR_PPU_OVERLAY_BG2, -g_ws_extra, 0,
            width, kActRaiserAuthenticHeight,
            SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER);
      }
    }
  }

  /* D2: claim observational full-frame Mode-1 captures only after every
   * pre-existing HUD/HD/diorama policy has had a chance to declare a
   * conflict. The original PPU framebuffer remains intact as same-frame A0. */
  {
    extern bool g_sim3d_textures_ready;
    extern bool g_sim3d_billboard_renderer_ready;
    extern bool g_diorama_frame_active;
    extern int g_ws_extra;
    uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
    uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
    bool town = ActRaiser_IsSimulationTown(map_group, map_number);
    /* Build D1b before choosing D2's physical planes. This is the same live
     * OAM/VRAM/CGRAM state the scanline renderer is about to consume, but now
     * an atlas or metadata failure can retain raw OBJ before scanout instead
     * of being discovered after those fallback pixels were omitted. */
    bool billboard_atlas_ready = town && SimRenderAtlas_Build(
        s_runner,
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX),
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY));
    Sim3DCaptureRequest request = {
      .town = town,
      .master_enabled = g_settings.sim3d_mode,
      /* Mirror16, not ReadWram16: the picker flag lives at $7F:9215, so its
       * constant is 0x19215 -- 17 bits. The low-WRAM helper takes a uint16 and
       * silently truncated it to 0x9215, reading an unrelated byte pair in bank
       * $7E, so the picker never activated. ActRaiser_ReadWramMirror16 exists
       * precisely to make that impossible (actraiser_game.h) and every other
       * reader of this address already used it -- this was the one call site that
       * did not. -Wall reported it as "changes value from 102933 to 37397". */
      .picker_active = town && ActRaiser_SimMapPickerActiveForState(
          map_group, map_number,
          ActRaiser_ReadWramMirror16(kActRaiserWram_SimMapPickerFlag)),
      .renderer_ready = g_sim3d_textures_ready,
      .billboard_atlas_ready = billboard_atlas_ready,
      .billboard_renderer_ready = g_sim3d_billboard_renderer_ready,
      .diorama_active = g_diorama_frame_active,
      /* The inspector panel is the only on-screen reader of the capture's
       * diagnostic hash; with it off, that pass is skipped. */
      .inspector_active = g_settings.scene_inspector,
      .requested_features = Settings_Sim3DRequestedFeatures(),
      .diagnostic_layer_mask = g_settings.sim3d_diagnostic_layers,
      .width = kActRaiserAuthenticWidth + 2 * g_ws_extra,
      .height = kActRaiserAuthenticHeight,
    };
    Sim3D_PrepareCapture(s_runner, &request);
    ActRaiser_ReportSim3DCaptureContractFailure();
  }

  /* AR_TILE_CENSUS=1: read-only HD tile-pack sizing survey (hd_tile_census.c). */
  HdTileCensus_Frame(s_runner);
  const SnesRunnerApi *scanout_api =
      sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *scanout_runner = s_runner;
  SrGenerationSnapshot scanout_generations = {
      .struct_size = sizeof(scanout_generations),
  };
  SrPpuScanoutResult scanout_result = {
      .struct_size = sizeof(scanout_result),
  };
  SrResult scanout_status = SR_RESULT_UNAVAILABLE;
  const bool scanout_ready =
      scanout_runner != NULL && scanout_api != NULL &&
      scanout_api->struct_size >= SNES_RUNNER_API_PPU_SCANOUT_SIZE &&
      (scanout_api->capabilities & SR_RUNNER_CAP_PPU_SCANOUT) != 0u &&
      scanout_api->query_generations(
          scanout_runner, &scanout_generations) == SR_RESULT_OK;
  const uint8_t hdma_active_mask = ActRaiser_QueryHdmaActiveMask();
  /* AR_TITLELOG=1: per-frame title-screen PPU probe (map bytes, BG mode,
   * HDMAEN, Mode-7 matrix, INIDISP) for deriving/validating the settled-logo
   * gate above. Diagnostic only. */
  if (ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_TitleLog)) {
    static int last_gf = -1;
    int gf = (int)ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last_gf) {
      SrPpuStateSnapshot scanout_initial = {
          .struct_size = sizeof(scanout_initial),
      };
      if (scanout_ready && scanout_api->query_ppu_state(
              scanout_runner, &scanout_initial) == SR_RESULT_OK) {
        last_gf = gf;
        fprintf(stderr, "[titlelog] gf=%d $18=%02x $19=%02x bgmode=%02x "
                "hdmaen=%02x m7=[%04x %04x %04x %04x] inidisp=%02x\n",
                gf, g_ram[kActRaiserWram_MapGroup],
                g_ram[kActRaiserWram_CurrentMap],
                scanout_initial.bg_mode_control, hdma_active_mask,
                (uint16)scanout_initial.mode7_matrix[0],
                (uint16)scanout_initial.mode7_matrix[1],
                (uint16)scanout_initial.mode7_matrix[2],
                (uint16)scanout_initial.mode7_matrix[3],
                scanout_initial.display_control);
      }
    }
  }
  /* The immutable action-room authority resolves the same persistent raster
   * state from ROM + camera + frame clock. Its default-off shadow is prepared
   * once here, then samples the live registers immediately before each visible
   * line so HDMA timing remains part of the comparison. */
  ActRaiserActionBg_BeginRoomSceneFrame(
      g_ram, kActRaiserWramSize);
  ActRaiserPpuScanoutContext scanout_context = {
      .shape_game_frame =
          (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
  };
  scanout_context.shape_trace =
      ActRaiser_PpuShapeTraceActive(scanout_context.shape_game_frame);
  const bool observe_lines =
      scanout_context.shape_trace ||
      ActRaiserActionBg_RoomSceneFrameObserverActive();
  const SrPpuScanoutRequest scanout_request = {
      .struct_size = sizeof(scanout_request),
      .lifetime_generation = scanout_generations.lifetime_generation,
      .hdma_suppress_mask = 0u,
      .line_callback = observe_lines
          ? ActRaiser_PpuScanoutLineCallback : NULL,
      .irq_callback = ActRaiser_PpuScanoutIrqCallback,
      .user_data = &scanout_context,
  };

  /* Resolve the stable OAM footprint before scanout; the live sprite evaluator
   * writes the selected range to the HUD surface while each line is fetched. */
  ActRaiser_DioramaHudObjPrepare();

  DioramaPerformance_End(producer_setup_performance);
  DioramaPerformanceScope scanout_performance = {0};
  if (profile_diorama)
    scanout_performance =
        DioramaPerformance_Begin(kDioramaPerformance_Scanout);

  if (scanout_ready) {
    scanout_status = scanout_api->run_ppu_scanout(
        scanout_runner, &scanout_request, &scanout_result);
  }
  if (scanout_status != SR_RESULT_OK) {
    SessionFatal_Request(
        "The runner could not execute the PPU scanout ABI service. "
        "Restart after rebuilding the game and runner together.");
  }
  const uint32_t authentic_camera_flags =
      SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG1 |
      SR_PPU_SCANOUT_AUTHENTIC_CAMERA_BG2;
  const bool authentic_frame_valid =
      scanout_status == SR_RESULT_OK &&
      (scanout_result.flags &
       SR_PPU_SCANOUT_AUTHENTIC_SURFACE_READY) != 0u &&
      (!action ||
       (scanout_result.final_state.flags &
        SR_PPU_STATE_FORCED_BLANK) != 0u ||
       scanout_result.final_state.bg_mode != 1u ||
       (scanout_result.flags & authentic_camera_flags) ==
           authentic_camera_flags);
  ActRaiser_AuthenticCaptureFrameCompleted(authentic_frame_valid);
  DioramaPerformance_End(scanout_performance);
  DioramaPerformanceScope producer_finish_performance = {0};
  if (profile_diorama)
    producer_finish_performance =
        DioramaPerformance_Begin(kDioramaPerformance_ProducerFinish);
  {
    extern uint8_t g_pixels[];
    extern int g_ws_extra;
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    /* g_pixels is bound apron-wide; the authentic frame starts kPpuObjApron
     * columns in. Offset the base and pass the real pitch. */
    Sim3D_FinishCapture(
        g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON),
        ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON),
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame));
    ActRaiser_ReportSim3DCaptureContractFailure();
    /* After scanout (the diorama planes only hold this frame's sprites now) and
     * before FrameSlot_Capture publishes them to the presentation path. */
    ActRaiser_DioramaHudObjFinish(width);
    ActRaiser_DioramaDeathHeimHubStatuesFinish(width);
    /* After the HUD-icon promote, not before: that pass PUNCHES the promoted
     * icon out of the OBJ planes, and the apron's claimed-set test reads those
     * planes. Running first would let a hole it is about to punch look like
     * free space. */
    const ActionApronGeometry apron_geom = ActRaiser_ObjApronGeometry();
    ActRaiser_DioramaApronFinish(&apron_geom);
  }
  /* Latch the margin state the frame was ACTUALLY
   * rendered with, here, rather than letting FrameSlot_Capture read live g_ppu.
   * Between this function and the frame slot capture, main.c may call
   * ActRaiser_RebindPpuOutputSurfaces(), whose public margin configuration
   * zeroes both live margins — reading g_ppu later would silently describe a
   * different frame than the pixels came from. The non-diorama rebind gate
   * normally prevents this mismatch; latching at the producer boundary makes
   * that safety independent of the gate. */
  s_live_margin_top = scanout_status == SR_RESULT_OK
      ? (int)scanout_result.final_state.margin_top : 0;
  s_live_margin_bottom = scanout_status == SR_RESULT_OK
      ? (int)scanout_result.final_state.margin_bottom : 0;

  if (ActRaiser_DeveloperFlagEnabled(
          kActRaiserDeveloperFlag_VerticalExtensionLog)) {
    /* Where each destination's content actually LANDED, which is the check that
     * catches the row-origin class of bug: the HUD surfaces are consumed in
     * AUTHENTIC screen space and must not move when the vertical margin
     * changes, while the diorama planes are consumed in CAPTURE space and must
     * move by exactly the margin. Both on one line so a regression in either
     * is one diff apart. */
    extern uint8_t g_hud_bg_pixels[];
    extern uint8_t *g_diorama_layer_pixels[];
    extern int g_ws_extra;
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    size_t pitch = (size_t)width * 4;
    int hud0 = -1, hud1 = -1, plane0 = -1, plane1 = -1;
    for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
      const uint32_t *r = (const uint32_t *)(g_hud_bg_pixels + (size_t)y * pitch);
      for (int x = 0; x < width; x++)
        if (r[x]) { if (hud0 < 0) hud0 = y; hud1 = y; break; }
    }
    const uint8_t *bg2 = g_diorama_layer_pixels[SR_PPU_OVERLAY_BG2];
    /* The plane's own pitch, not the HUD's: the diorama planes are bound
     * apron-wide. A diagnostic that exists to catch origin bugs must not carry
     * one, and the wrong stride would slide its reported rows a little further
     * every row it walked. */
    const size_t plane_pitch =
        ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON);
    if (bg2)
      for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
        const uint32_t *r = (const uint32_t *)(bg2 + (size_t)y * plane_pitch);
        for (int x = 0; x < width + (int)SR_PPU_OBJ_APRON * 2; x++)
          if (r[x]) { if (plane0 < 0) plane0 = y; plane1 = y; break; }
      }
    fprintf(stderr,
            "[vext-rows] gf=%u top=%d bottom=%d hudbg=[%d..%d] "
            "bg2plane=[%d..%d] "
            "objs_unlocked=%u\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            s_live_margin_top, s_live_margin_bottom,
            hud0, hud1, plane0, plane1,
            ActRaiser_TakeVextUnlockedObjects());
  }
  s_live_margin_left = scanout_status == SR_RESULT_OK
      ? (int)scanout_result.final_state.margin_left : 0;
  s_live_margin_right = scanout_status == SR_RESULT_OK
      ? (int)scanout_result.final_state.margin_right : 0;
  s_live_action_bg_plan = s_pending_action_bg_plan;
  s_live_bg_capture_pad_to_budget =
      s_pending_bg_capture_pad_to_budget;

  /* Sky Palace BG2 is prepared at the top of this function and ALWAYS restored
   * here at the end. ActRaiserDrawPpuFrame has no early returns, so a pending
   * restore can never be stranded — keep it that way if you add control flow
   * above. */
  ActRaiser_WidescreenSkyPalaceRestore(s_runner);
  DioramaPerformance_End(producer_finish_performance);
  s_ppu_frame_access = NULL;
  return SR_RESULT_OK;
}

/* Same latch, same reason (see ActRaiser_LiveMargins): the vertical bands the
 * frame was ACTUALLY rendered with, not whatever g_ppu holds by the time the
 * frame slot is captured. */
void ActRaiser_LiveVerticalMargins(int *top, int *bottom) {
  if (top) *top = s_live_margin_top;
  if (bottom) *bottom = s_live_margin_bottom;
}

/* See the latch above. Reports the margin geometry of the most recently rendered
 * frame, which is what a consumer of that frame's captured pixels must use. */
void ActRaiser_LiveMargins(int *left, int *right) {
  if (left) *left = s_live_margin_left;
  if (right) *right = s_live_margin_right;
}

bool ActRaiser_LiveActionBgPlan(ActionBgPlan *out,
                                bool *pad_captured_to_budget) {
  if (out) *out = s_live_action_bg_plan;
  if (pad_captured_to_budget)
    *pad_captured_to_budget = s_live_bg_capture_pad_to_budget;
  return s_live_action_bg_plan.valid;
}

/* Reload the selector-dependent part of the action OBJ atlas after a live
 * magic selection change. The native level-entry loader $02:BC9E copies 128
 * words from $06:A400 + (selector-1)*$80 to VRAM $2D40. Merely changing
 * $02AC during an action stage would therefore run the new spell with the old
 * spell's resident tiles and produce a misleading graphics failure. This
 * targeted host copy reproduces only that selector-dependent upload; the
 * common atlas and palettes remain untouched. */
static void ActRaiser_ReloadSelectedMagicTiles(uint8 selector) {
  if (selector < 1 || selector > 4) return;
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram = {.struct_size = sizeof(vram)};
  SrPpuVramWordPatch patches[0x80];
  if (!ActRaiser_QueryPpuState(&ppu) || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE ||
      !s_runner_api->borrow_u16_memory ||
      !s_runner_api->compare_exchange_ppu_vram_words ||
      s_runner_api->borrow_u16_memory(
          s_runner, SR_MEMORY_VRAM, &vram) != SR_RESULT_OK)
    return;
  uint16 source = (uint16)(0xA400 + (selector - 1) * 0x80);
  for (uint16 word = 0; word < 0x80; word++) {
    const uint16_t address = (uint16_t)(0x2D40 + word);
    patches[word] = (SrPpuVramWordPatch) {
      .word_address = address,
      .expected = vram.data[address],
      .replacement =
          cpu_read16(&g_cpu, 0x06, (uint16)(source + word * 2)),
    };
  }
  const SrPpuVramPatchRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_VRAM_PATCH_ADDRESSES_SORTED,
    .lifetime_generation = ppu.lifetime_generation,
    .patches = patches,
    .patch_count = 0x80u,
  };
  (void)s_runner_api->compare_exchange_ppu_vram_words(s_runner, &request);
}

static const char *const kActRaiserMagicNames[] = {
  "none", "Magical Fire", "Magical Stardust", "Magical Aura", "Magical Light"
};

uint8 ActRaiser_SelectedMagic(void) {
  uint8 selected = g_ram[kActRaiserWram_SelectedMagic];
  return selected <= 4 ? selected : 0;
}

/* Set by the host input thread's edge dispatch, consumed once per frame by
 * ActRaiser_ApplyMagicCycle. Both sides run on the main thread (the present
 * thread is the one that was split out), so a plain flag is sufficient and a
 * held key cannot queue a burst: it is cleared unconditionally on read. */
static bool s_magic_cycle_requested;

void ActRaiser_RequestMagicCycle(void) { s_magic_cycle_requested = true; }

/* Which spells the save actually owns. The four inventory bytes $0299-$029C
 * hold spell ids (1..4) or 0 for an empty slot, and they are NOT sorted or
 * positional — All magic writes 1/2/3/4 in order, but a real save fills them
 * in pickup order. Collect the distinct ids so the cycle visits each unlocked
 * spell exactly once, in canonical Fire/Stardust/Aura/Light order rather than
 * in whatever order the player happened to find them. */
static unsigned ActRaiser_UnlockedMagic(uint8 out[4]) {
  bool present[5] = { false, false, false, false, false };
  for (unsigned slot = 0; slot < 4; slot++) {
    uint8 id = g_ram[kActRaiserWram_MagicInventory + slot];
    if (id >= 1 && id <= 4) present[id] = true;
  }
  unsigned count = 0;
  for (uint8 id = 1; id <= 4; id++)
    if (present[id]) out[count++] = id;
  return count;
}

/* Debug aid: step the action-stage spell selection to the next spell the save
 * has unlocked. Armed by the "Cycle magic spell" cheat and triggered by the
 * kInputAction_MagicCycle binding (keyboard or pad) — it no longer reserves a
 * SNES button, so it cannot shadow L in normal play. */
static void ActRaiser_ApplyMagicCycle(void) {
  bool requested = s_magic_cycle_requested;
  s_magic_cycle_requested = false;

  if (!requested || !g_settings.cheat_magic_cycle) return;

  /* Action stages only: $02AC and the $2D40 tile window are act-mode state,
   * and the sim-mode equip menu owns the selection there. Gated here rather
   * than at the call site so a press made in town is dropped outright instead
   * of firing the moment the next act loads. */
  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
    fprintf(stderr, "[magic-cycle] not in an action stage; ignored\n");
    return;
  }

  /* $00F8 is the act-mode cast state. Rewriting $02AC mid-cast would leave
   * the in-flight spell's actors running against the new spell's tiles. */
  if (g_ram[kActRaiserWram_MagicCastState] != 0) {
    fprintf(stderr, "[magic-cycle] cast still active; selection unchanged\n");
    return;
  }

  uint8 unlocked[4];
  unsigned count = ActRaiser_UnlockedMagic(unlocked);
  if (!count) {
    fprintf(stderr, "[magic-cycle] no spells unlocked; selection unchanged "
            "(enable the All magic cheat to test every spell)\n");
    return;
  }

  /* Advance past the current selection, wrapping. An unknown or unowned
   * current value (including 0 / "none") starts the cycle at the first
   * unlocked spell rather than being treated as an error. */
  uint8 current = g_ram[kActRaiserWram_SelectedMagic];
  unsigned index = 0;
  for (unsigned i = 0; i < count; i++) {
    if (unlocked[i] == current) { index = (i + 1) % count; break; }
  }
  uint8 next = unlocked[index];

  g_ram[kActRaiserWram_SelectedMagic] = next;
  ActRaiser_ReloadSelectedMagicTiles(next);
  fprintf(stderr, "[magic-cycle] selected %s (%u of %u unlocked, $02AC=$%02X, "
          "VRAM $2D40 refreshed)\n", kActRaiserMagicNames[next],
          index + 1, count, next);
}

/* Host-side cheat hooks (debug-menu scaffold). All settings-gated and seeded
 * OFF from their legacy env names, so they never affect a normal run. Applied
 * once per frame at the START of RunOneFrameOfGame (before the game's frame
 * logic), so a value pinned here is what the frame sees -> effective for death
 * prevention (HP) and physics override (moonjump). RAM is g_ram (WRAM): low
 * direct-page addrs map 1:1 ($1D player HP, $E6/$E7 timer, player object $08A0).
 * This is the framework the planned debug menu plugs into — see
 * docs/SEAMS.md "Gameplay / Tunable seams" + memory debug-menu-warp-roadmap. */
enum {
  kPackedBcdDigitRadix = 10,
  kPackedBcdPairPlaceValue = kPackedBcdDigitRadix * kPackedBcdDigitRadix,
};

static int ActRaiser_BcdTimerToSeconds(uint8 low, uint8 high) {
  int low_pair =
      (low & 0x0F) + ((low >> 4) & 0x0F) * kPackedBcdDigitRadix;
  int high_pair =
      (high & 0x0F) + ((high >> 4) & 0x0F) * kPackedBcdDigitRadix;
  return low_pair + high_pair * kPackedBcdPairPlaceValue;
}

void ActRaiser_ApplyCheats(void) {

  /* AR_PIN=<parcode>[,<parcode>...] — generic PAR/ZSNES cheat-code pinner
   * (2026-07-06). Each code is the standard 8-hex-digit PAR form BBAAAAVV
   * (bank $7E/$7F, 16-bit addr, byte value), applied every frame in EVERY
   * mode (unlike the mode-gated hand cheats below). Turns the whole
   * ./codes.txt catalogue (flamingspinach's 88 engineered codes — see
   * docs/ram-map.md "Cheat-derived WRAM map") into ready-made debug cheats
   * AND address-mapping probes with zero per-cheat C. Example:
   *   AR_PIN=7E00210A,7E029901   (INF MP + HAVE FIRE — the §7.18 kit)
   * Bad tokens are reported once and skipped. Max 32 pins. */
  for (int i = 0; i < g_settings.pin_count; i++)
    g_ram[g_settings.pins[i].off] = g_settings.pins[i].val;

  /* Action-stage gameplay tweaks only. MapGroup $01-$06 selects an ordinary
   * action region, $07 is Death Heim, $00 is the non-action town/world/UI
   * engine, and $08 is the ending presenter. Gate on the whole action range so
   * cheats persist across every region, not just Fillmore ($18==$01) — that
   * bug disabled them after warping to region 2+. The player object/HP/timer
   * fields are shared by the action engine across every region, so the same
   * writes apply everywhere. */
  /* ── ALL-MODE cheats (above the action-stage gate: they feed the sim-mode
   * equip menu / angel, so they must pin in every mode) ─────────────────── */

  /* AR_ALL_MAGIC=1: unlock all four spells. HAVE flags $0299-$029C = 01/02/03/04
   * (cheat-map values, docs/ram-map.md). Pinned in ALL modes so the sim-mode
   * equip menu lists them; SELECTING one still goes through the menu (the equip
   * routine $01:915D derives $02AC from these). docs/SEAMS.md has the full
   * magic wiring map and cast-gate interaction. */
  {
    if (g_settings.cheat_all_magic) {
      g_ram[kActRaiserWram_MagicInventory + 0] = 0x01; /* Magical Fire */
      g_ram[kActRaiserWram_MagicInventory + 1] = 0x02; /* Magical Stardust */
      g_ram[kActRaiserWram_MagicInventory + 2] = 0x03; /* Magical Aura */
      g_ram[kActRaiserWram_MagicInventory + 3] = 0x04; /* Magical Light */
    }
  }

  /* AR_RANGED_SWORD=1: sword fires a projectile ($E4 = $80, PAR 7E00E480). */
  if (g_settings.cheat_ranged_sword)
    g_ram[kActRaiserWram_RangedSwordFlag] = 0x80;

  /* AR_INF_MP: infinite magic scrolls. =1 -> pin the WORKING count $21 to 10
   * (PAR 7E00210A); =<n> -> pin to n. Deliberately does NOT touch the
   * PERSISTENT count $0295; $21 is the act-mode working copy loaded from
   * $0295 at $02:84E0, so the cheat never bakes into save.srm. */
  if (g_settings.cheat_inf_mp)
    g_ram[kActRaiserWram_WorkingMagicPoints] =
        (uint8)g_settings.cheat_inf_mp;

  /* AR_INF_SP=1: infinite sim-mode SP (miracle points). Self-calibrating: pins
   * current SP $0282/16 to max SP $0284/16 once max is known (vs the PAR code's
   * blunt $FF, which over-fills early-game maxima). */
  {
    if (g_settings.cheat_inf_sp &&
        ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp)) {
      ActRaiser_WriteWram16(
          kActRaiserWram_AngelCurrentSp,
          ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp));
    }
  }

  /* AR_ANGEL_HP=1: infinite sim-mode angel health. Self-calibrating: pins
   * current HP $0286 to max HP $0287 (the PAR code 7E028608 hardcodes 8, which
   * would UNDER-fill after level-ups raise the max). */
  if (g_settings.cheat_angel_hp &&
      g_ram[kActRaiserWram_AngelMaximumHp])
    g_ram[kActRaiserWram_AngelCurrentHp] =
        g_ram[kActRaiserWram_AngelMaximumHp];

  /* Live action-stage magic selection/asset reload for effect testing. Above
   * the action-stage gate so a request made outside an act is consumed and
   * discarded rather than queued until the next act loads; the handler
   * applies the gate itself. */
  ActRaiser_ApplyMagicCycle();

  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) return;

  /* AR_INF_HP: infinite health. =1 -> auto: pin player HP ($1D) to the
   * high-water max seen this stage (self-calibrates to "full" once you've been
   * at full, so we needn't know max HP statically). =<n> -> pin to literal n. */
  {
    static int previous_cheat_mode;
    static unsigned highest_hp;
    int cheat_mode = g_settings.cheat_inf_hp;
    if (cheat_mode != previous_cheat_mode) {
      /* Entering/re-entering auto mode must calibrate from the current stage,
       * not reuse a high-water value captured before a live toggle. */
      if (cheat_mode == 1) highest_hp = 0;
      previous_cheat_mode = cheat_mode;
    }
    if (cheat_mode) {
      if (cheat_mode > 1) {
        g_ram[kActRaiserWram_PlayerHp] = (uint8)cheat_mode;
      } else {
        unsigned current_hp = g_ram[kActRaiserWram_PlayerHp];
        if (current_hp > highest_hp && current_hp <= 0xFF)
          highest_hp = current_hp;
        if (highest_hp)
          g_ram[kActRaiserWram_PlayerHp] = (uint8)highest_hp;
      }
    }
  }

  /* AR_FREEZE_TIMER=1: pin the action-stage timer ($E6/$E7, BCD) to its first
   * captured value -> infinite time.
   *
   * Backs off automatically once the boss-defeat point-tally sequence starts
   * draining the timer: normal countdown never drops the BCD value by more
   * than 1 (roughly once per real-time second), so any single-frame drop
   * bigger than that can only be the drain script deliberately driving the
   * timer down, not the stage clock. When that happens, stop re-pinning and
   * let the game own the timer for the rest of THIS stage -- otherwise the
   * frozen timer blocks the drain and the boss->sim transition never
   * completes. No separate "boss defeated" flag needed; the abnormal
   * decrement rate IS the signal (2026-07-01). Latch resets on region change
  * ($18) so a fresh stage re-arms the freeze instead of staying stuck off
  * from a previous boss fight. */
  {
    static uint8 captured_timer_low;
    static uint8 captured_timer_high;
    static int timer_captured;
    static int drain_detected;
    static uint8 last_map_group = kActRaiserUnknownMapGroup;
    static int cheat_was_enabled;
    if (!g_settings.cheat_freeze_timer) {
      /* A future live off/on toggle starts a fresh capture and drain latch. */
      cheat_was_enabled = 0;
      timer_captured = 0;
      drain_detected = 0;
      last_map_group = kActRaiserUnknownMapGroup;
    } else {
      if (!cheat_was_enabled) {
        cheat_was_enabled = 1;
        timer_captured = 0;
        drain_detected = 0;
        last_map_group = kActRaiserUnknownMapGroup;
      }
      if (g_ram[kActRaiserWram_MapGroup] != last_map_group) {
        last_map_group = g_ram[kActRaiserWram_MapGroup];
        timer_captured = 0;
        drain_detected = 0;
      }
      if (!timer_captured) {
        captured_timer_low = g_ram[kActRaiserWram_ActionTimerLow];
        captured_timer_high = g_ram[kActRaiserWram_ActionTimerHigh];
        timer_captured = 1;
      }
      if (!drain_detected) {
        int captured_seconds = ActRaiser_BcdTimerToSeconds(
            captured_timer_low, captured_timer_high);
        int current_seconds = ActRaiser_BcdTimerToSeconds(
            g_ram[kActRaiserWram_ActionTimerLow],
            g_ram[kActRaiserWram_ActionTimerHigh]);
        if (captured_seconds - current_seconds > 1) {
          drain_detected = 1;
        } else {
          g_ram[kActRaiserWram_ActionTimerLow] = captured_timer_low;
          g_ram[kActRaiserWram_ActionTimerHigh] = captured_timer_high;
        }
      }
    }
  }

  /* AR_MOONJUMP=1: hold the game's jump button (SNES B) to FLY UP. We move the player
   * Y-POSITION ($08A4, +$04) directly rather than the Y-velocity ($08A8, +$08) —
   * object fields are polymorphic by state, so $08A8 is "Y-velocity" only in the
   * AIR state; while grounded it means something else (writing it there didn't
   * launch and leaked into other movement). Position is always position, so
   * decrementing $08A4 (screen-Y grows downward, so −Y = up) is a reliable
   * state-independent fly. AR_MOONJUMP_SPEED sets up-speed in pixels/frame
   * (default 6). The trigger follows the game's fixed jump mapping instead of
   * exposing a second, potentially contradictory cheat binding. */
  {
    enum { kActRaiserJoypadJump = 0x8000 };  /* auto-joypad SNES B bit */
    if (g_settings.cheat_moonjump) {
      SrInputStateSnapshot input;
      uint16 buttons = ActRaiser_QueryInputState(&input)
          ? input.auto_joypad[0] : 0u;
      if (buttons & kActRaiserJoypadJump) {
        uint16 player_y =
            ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY);
        player_y = (uint16)(player_y - g_settings.cheat_moonjump_speed);
        ActRaiser_WriteWram16(kActRaiserWram_PlayerPositionY, player_y);
      }
    }
  }

  /* AR_NO_KNOCKBACK=1: permanent invuln -> no hit registers -> no damage, no
   * knockback, no hitstun (speedrun "ignore hits"), using the game's own
   * i-frames. The hit-check gates on the INVULN FLAG ($08D0 bit 0x2000, +$30),
   * which the game sets on a hit and clears when the i-frame TIMER ($08C6, +$26)
   * counts down to 0. This authentic invulnerability state also suppresses
   * water drag; disable the cheat for movement/terrain-physics validation.
   * So we (a) pin the timer to 0xFF so the game never clears
   * the flag, and (b) SET the flag ourselves each frame so invuln is active from
   * frame one (without needing a first hit to bootstrap it — that was the
   * "works only after getting hit once" gap). Offsets found via AR_WATCHOBJ=08A0
   * while taking a hit. AR_NO_KNOCKBACK=<hexoff> (other than 1) instead raw-pins
   * $08A0+off to 0xFF for experimentation. */
  {
    static int prior_mode;
    int mode = g_settings.cheat_no_knockback;
    if (prior_mode == 1 && mode != 1) {
      /* Release only the two fields owned by full-invulnerability mode. Raw
       * experimental offset pins are intentionally not guessed/restored. */
      g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0;
      g_ram[kActRaiserWram_PlayerFlags + 1] &=
          (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
    }
    prior_mode = mode;
    if (mode) {
      if (mode == 1) {
        g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0xFF;
        /* MAGIC EXCEPTION: the cast gate ($00:9843 ->
         * $00:9DE1) does BIT #$2008 on player state $08D0 and refuses to cast
         * while the invuln flag ($2000) is set -- pinning it unconditionally made
         * magic permanently dead. Lift the pin ONLY when a cast will actually
         * fire this frame, i.e. when every condition of the game's own gate
         * holds: cast button held ($00A0 & $C0 -- the NMI joypad shadow of
         * $4218&$F4, bit7=A bit6=X, level-sensitive so the 1-frame NMI lag is
         * fine), no cast in progress ($F8==0), magic equipped ($02AC!=0), and
         * MP available ($21>0). The instant the cast starts the game sets $F8
         * nonzero -> the pin snaps back ON for the whole cast animation.
         * Residual vulnerability: only the 1-2 frames between press and cast
         * start; holding the button with no magic/MP no longer drops invuln
         * at all (the old version was vulnerable the entire time the button
         * was held). */
        int cast_imminent =
            (g_ram[kActRaiserWram_InputHeldHigh] & 0xC0) != 0 &&
            g_ram[kActRaiserWram_MagicCastState] == 0 &&
            g_ram[kActRaiserWram_SelectedMagic] != 0 &&
            g_ram[kActRaiserWram_WorkingMagicPoints] != 0;
        if (cast_imminent)
          g_ram[kActRaiserWram_PlayerFlags + 1] &=
              (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
        else
          g_ram[kActRaiserWram_PlayerFlags + 1] |=
              kActRaiserPlayerFlag_InvulnerableHighByte;
      } else {
        g_ram[kActRaiserWram_PlayerObject + (unsigned)mode] = 0xFF;
      }
    }
  }
}

/* Level-warp: stage the game's OWN sim->act transition to a chosen region/map,
 * bypassing the (broken) sim-mode UI. The intro/overworld stages an act entry by
 * writing the transition-DEST vars + a request flag, which the transition
 * processor then consumes (full fade + level-load + mode switch). Observed entry
 * into Fillmore act 1: $1B=01 (-> $18 region), $1A=01 (-> $19 act), $FB|=0x80
 * (request). We replicate that. Best triggered from a transition-capable state
 * (the intro, $18==00, which WORKS — unlike the post-act sim cascade). Hooked to
 * F6 in main.c with the raw target from AR_WARP=<region_hex><map_hex>. The map
 * byte is written directly to $19 and is not a uniform act number. */
void ActRaiser_Warp(unsigned region, unsigned map) {
  uint8 source_map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 source_map = g_ram[kActRaiserWram_CurrentMap];
  g_ram[kActRaiserWram_DestinationMapGroup] = (uint8)region;
  g_ram[kActRaiserWram_DestinationMap] = (uint8)map;
  g_ram[kActRaiserWram_TransitionRequest] |= kActRaiserTransitionRequestBit;
  fprintf(stderr, "[warp] from $18=$%02X $19=$%02X staged region=$%02X "
          "map=$%02X ($1B/$1A/$FB set); transition processor should pick it up.\n",
          source_map_group, source_map, region & 0xFF, map & 0xFF);
  if (ActRaiser_IsActionMapGroup(source_map_group)) {
    fprintf(stderr, "[warp] WARNING: action->action is not a naturally observed "
            "transition; inherited timing/object state may affect fidelity.\n");
  }
}

/* Create the game coroutine on its own 2MB stack. Returns false if the
 * supported coroutine contract cannot be established; callers end the
 * session instead of attempting to continue with a partial runtime. */
static bool CreateGameCoroutine(void) {
#ifdef _WIN32
  /* FIBER_FLAG_FLOAT_SWITCH is REQUIRED, not optional: MS documents that with
   * flags zero "the floating-point state on x86 systems is not switched and
   * data can be corrupted if a fiber uses floating-point arithmetic" — and the
   * game coroutine does use FP (the watchdog's `double elapsed`, the DSP
   * resample phase). Committing 64KB of the 2MB reserve up front instead of the
   * whole thing keeps the fiber cheap to (re)create while still reserving the
   * full stack; the recompiled dispatch stack can go 64 frames deep. */
  if (!g_host_fiber) {
    g_host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_host_fiber) {
      fprintf(stderr, "Failed to convert driver thread to fiber\n");
      return false;
    }
  }
  if (g_game_fiber) {
    DeleteFiber(g_game_fiber);
    g_game_fiber = NULL;
  }
  g_game_fiber = CreateFiberEx(kGameCoroutineStackCommitBytes,
                               kGameCoroutineStackReserveBytes,
                               FIBER_FLAG_FLOAT_SWITCH,
                               game_coroutine_fiber, NULL);
  if (!g_game_fiber) {
    fprintf(stderr, "Failed to create game coroutine fiber\n");
    return false;
  }
#else
  if (!g_game_stack) {
    /* mmap with a PROT_NONE GUARD PAGE below the stack rather than malloc.
     * makecontext requires the caller to supply the stack, and with an
     * app-supplied stack "it is the application's responsibility to handle
     * stack overflow" — a malloc'd stack has no guard, so a deep recompiled
     * dispatch chain that overruns it would silently scribble over whatever
     * the allocator placed underneath (heap corruption, arbitrary later
     * crash). With a guard page the overflow faults immediately, at the site
     * that caused it. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
      fprintf(stderr, "Failed to query the host page size\n");
      return false;
    }
    size_t guard = (size_t)page;
    size_t total = kGameCoroutineStackReserveBytes + guard;
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
      fprintf(stderr, "Failed to allocate game coroutine stack\n");
      return false;
    }
    /* Stacks grow DOWN, so the guard belongs at the lowest address. */
    if (mprotect(map, guard, PROT_NONE) != 0) {
      fprintf(stderr, "Failed to protect the game coroutine guard page: %s\n",
              strerror(errno));
      munmap(map, total);
      return false;
    }
    g_game_stack_map = map;
    g_game_stack_map_len = total;
    g_game_stack = (char *)map + guard;
  }
  /* The abandoned context is just register state pointing into this stack;
   * re-running makecontext over the SAME buffer resets the entry point, so no
   * unmap/remap is needed (and none would be safe while the old context's
   * frames still nominally live there). */
  if (getcontext(&g_game_ctx) != 0) {
    fprintf(stderr, "Failed to capture game coroutine context\n");
    return false;
  }
  g_game_ctx.uc_stack.ss_sp = g_game_stack;
  g_game_ctx.uc_stack.ss_size = kGameCoroutineStackReserveBytes;
  g_game_ctx.uc_link = &g_host_ctx;
  makecontext(&g_game_ctx, game_coroutine, 0);
#endif
  return true;
}

/* Release the coroutine's stack/fiber. Called from the game's shutdown path so
 * the guard-page mapping and the fiber are not leaked, and so a leak checker
 * run against a clean exit stays quiet. Safe to call without a coroutine. */
void ActRaiser_DestroyGameCoroutine(void) {
  ActRaiserHleFatal_RegisterHostEscape(NULL);
  g_game_coroutine_executing = false;
#ifdef _WIN32
  if (g_game_fiber) {
    DeleteFiber(g_game_fiber);
    g_game_fiber = NULL;
  }
#else
  if (g_game_stack_map) {
    munmap(g_game_stack_map, g_game_stack_map_len);
    g_game_stack_map = NULL;
    g_game_stack_map_len = 0;
    g_game_stack = NULL;
  }
#endif
  g_game_started = false;
}

static bool ActRaiser_ControlGameTiming(
    bool begin, uint32_t flags,
    uint32_t *out_transition_flags) {
  const int result = begin ? RtlGameFrameBegin() : RtlGameFrameComplete(flags);
  if (result < 0) {
    SessionFatal_Request(
        "The runner could not update the game timing state. Restart the "
        "game; if this repeats, report the active runner build.");
    return false;
  }
  if (out_transition_flags)
    *out_transition_flags = (uint32_t)result;
  return true;
}

void RunOneFrameOfGame(void) {
  if (!g_game_started) {
    /* config.ini and process environment layers are final by this point. */
    (void)ActRaiser_GetDeveloperEnvironment();
    g_game_started = true;
#if SNESRECOMP_WATCHDOG
    /* Give the runtime watchdog the coroutine yield to escape a stuck frame
     * with (the old longjmp out of this coroutine was UB / fiber-forbidden). */
    { extern void (*g_watchdog_yield_hook)(void);
      g_watchdog_yield_hook = ActRaiser_YieldToHost; }
#endif
    if (!CreateGameCoroutine()) {
      SessionFatal_Request(
          "The game could not create its emulation coroutine. Restart the "
          "game; if this repeats, check security software and virtual-memory "
          "limits for this process.");
      return;
    }
    ActRaiserHleFatal_RegisterHostEscape(ActRaiser_HleFatalEscapeToHost);
  }

  ActRaiser_ApplyCheats();   /* host-side cheats (live settings, default off) */

  if (g_action_load_hold_frames) {
    bool one_shot_completed = false;
    uint64_t one_shot_token = 0;
    if (g_action_load_one_shot_token) {
      one_shot_token =
          MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
    }
    if (ActionLoadPacing_ShouldReleaseForOneShot(
            g_action_load_hold_frames, g_action_load_one_shot_token,
            one_shot_token, one_shot_completed)) {
      if (ActRaiser_DeveloperFlagEnabled(
              kActRaiserDeveloperFlag_LoadPacingLog)) {
        fprintf(stderr,
                "[load-pace] f=%d HD one-shot complete; released forced "
                "blank %u frame(s) early\n",
                snes_frame_counter, g_action_load_hold_frames);
      }
      g_action_load_hold_frames = 0;
      g_action_load_one_shot_token = 0;
      /* A short enhanced cue is allowed to end the accuracy hold. Its muted
       * authentic sequencer may not have reached the loader acknowledgement,
       * so restore touch catch-up for the remaining collapsed work. */
      RtlSetApuCatchupSuppressed(false);
    } else {
      g_action_load_hold_frames--;
      if (!g_action_load_hold_frames)
        g_action_load_one_shot_token = 0;
      return;
    }
  }
  if (!ActRaiser_ControlGameTiming(
          true, 0u, NULL))
    return;
  g_game_coroutine_executing = true;
#ifdef _WIN32
  SwitchToFiber(g_game_fiber);
#else
  if (swapcontext(&g_host_ctx, &g_game_ctx) != 0) {
    g_game_coroutine_executing = false;
    (void)ActRaiser_ControlGameTiming(
        false, 0u, NULL);
    SessionFatal_Request(
        "The operating system could not resume the emulation coroutine "
        "(%s). Restart the game; if this repeats, check virtual-memory and "
        "process limits.",
        strerror(errno));
    return;
  }
#endif
  g_game_coroutine_executing = false;
  if (SessionFatal_Requested()) {
    (void)ActRaiser_ControlGameTiming(
        false, 0u, NULL);
    return;
  }
  if (g_watchdog_tripped) {
    (void)ActRaiser_ControlGameTiming(
        false, 0u, NULL);
    SessionFatal_Request(
        "The emulated game stopped responding and the watchdog ended the "
        "session. Your latest battery save will be flushed before exit. "
        "Restart the game; if the same room hangs again, report the room and "
        "active gameplay settings.");
    return;
  }
  uint32_t timing_transition;
  if (!ActRaiser_ControlGameTiming(
          false, RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED,
          &timing_transition))
    return;

  /* $4200 bit 7 remains the hardware NMI gate. A coroutine yield may model
   * CPU time rather than a vblank wait, so the runner reports whether it
   * entered NMI while preserving RDNMI's independent fresh-frame token. */
  if ((timing_transition & RTL_GAME_FRAME_NMI_ENTERED) == 0u)
    return;

  /* NmiHandler ends in RTI, which pops a hardware interrupt frame
   * (P/PC/PB). Push the matching frame first — otherwise the RTI
   * over-pops the stack and loads garbage into cpu->P, corrupting the
   * M/X width flags of the interrupted game code. Symmetric to the
   * IRQ path in ActRaiserDrawPpuFrame. */
  {
    CpuRegSnapshot snap;
    const bool observe_interrupt =
        RtlGameEventEnabled(SR_EVENT_MASK_INTERRUPT);
    const uint32 interrupt_pc =
        observe_interrupt ? ActRaiser_LastBlockPc() : 0u;
    const uint16 interrupt_vector =
        g_cpu.emulation ? 0xfffau : 0xffeau;
    ActRaiser_SaveRegs(&g_cpu, &snap);
    if (observe_interrupt) {
      ActRaiser_EmitInterrupt(
          SR_INTERRUPT_NMI, SR_EVENT_INTERRUPT_ENTER, interrupt_pc,
          interrupt_vector, SR_INTERRUPT_SCANLINE_UNKNOWN, "nmi");
    }
    cpu_push_interrupt_frame(&g_cpu);
    g_sr_in_interrupt = 1;
    NmiHandler_M1X1(&g_cpu);
    g_sr_in_interrupt = 0;
    ActRaiser_RestoreRegs(&g_cpu, &snap);
    if (observe_interrupt) {
      ActRaiser_EmitInterrupt(
          SR_INTERRUPT_NMI, SR_EVENT_INTERRUPT_EXIT, interrupt_pc,
          interrupt_vector, SR_INTERRUPT_SCANLINE_UNKNOWN, "nmi");
    }
  }
}
