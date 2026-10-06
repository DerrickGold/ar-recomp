/* _XOPEN_SOURCE exposes ucontext (getcontext/makecontext/swapcontext), but on
 * macOS it also HIDES the BSD extensions — including MAP_ANON, which the
 * coroutine stack's guard page needs. _DARWIN_C_SOURCE puts them back without
 * giving up the XSI namespace. */
#define _XOPEN_SOURCE 600
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "actraiser/actraiser_rtl_internal.h"
#include "actraiser/actraiser_localization_schedule.h"
#include "actraiser/enhancements/actraiser_world_resume.h"
#include "app/input_replay.h"
#include "actraiser/actraiser_angel.h"
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
/* The game runs as a ucontext coroutine so a VBlank wait can yield mid-frame
 * (see docs/rendering-engine.md). macOS deprecated get/make/swapcontext in
 * 10.6 and offers no replacement with the same semantics; the functions still
 * work and the model depends on them, so the deprecation is acknowledged here
 * rather than repeated at four call sites. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

enum {
  kGameCoroutineStackReserveBytes = 2 * 1024 * 1024,
  kGameCoroutineStackCommitBytes = 64 * 1024,
  kRdnmiRepeatedReadWarningThreshold = 4096,
};

static ActRaiserRomSetupResult s_rom_setup_result;
static SrRunnerHandle *s_runner;
static const SnesRunnerApi *s_runner_api;

bool ActRaiserSimMenu_ArtworkAvailable(void) {
  static SimMenuFrame preflight;
  static bool failure_reported;
  const bool available = SimMenuArt_Capture(&preflight, s_runner_api, s_runner);
  if (!available && !failure_reported)
    fprintf(stderr, "[sim-menu] modern artwork preflight failed; retaining native menu control\n");
  failure_reported = !available;
  return available;
}

void ActRaiser_BindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
  s_runner_api = runner
      ? sr_runner_get_api(SR_RUNNER_ABI_VERSION)
      : NULL;
}

/* Read-only views of the binding for the files split from this one; only
 * ActRaiser_BindRunner changes it. */
SrRunnerHandle *ActRaiser_Runner(void) { return s_runner; }
const SnesRunnerApi *ActRaiser_RunnerApi(void) { return s_runner_api; }

bool ActRaiser_QueryPpuState(SrPpuStateSnapshot *state) {
  if (!state || !s_runner || !s_runner_api ||
      s_runner_api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !s_runner_api->query_ppu_state)
    return false;
  *state = (SrPpuStateSnapshot){
    .struct_size = sizeof(*state),
  };
  return s_runner_api->query_ppu_state(s_runner, state) == SR_RESULT_OK;
}

uint8_t ActRaiser_QueryHdmaActiveMask(void) {
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

bool ActRaiser_ResetPpuFrameCaptures(void) {
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

bool ActRaiser_ClearPpuObjMetadata(void) {
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
  ActRaiserHud_Reset();
  ActRaiserCredits_Reset();
  ActRaiserLocalizationText_ResetObservation();
  ActRaiserSimMenu_Reset();
  ActRaiserAngel_Reset();
  ActRaiserBg3Upload_Reset();
  ActRaiserSpriteOwnership_Reset();
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

/* RunOneFrameOfGame takes the snapshot once, but the readers below run from
 * vblank, object, and presentation paths, so the publish has to be safe for a
 * second thread that arrives first: resolve into a local, copy it in, and only
 * then release the ready flag (DioramaPerformance_Enabled uses the same
 * idiom). Nothing may consult shared storage to decide whether a pointer is
 * safe to dereference -- the local that was tested is the only thing allowed to
 * gate the parse, so a torn or clobbered flag byte cannot hand strtol a NULL
 * string. */
static ActRaiserDeveloperEnvironment s_developer_environment;
static atomic_bool s_developer_environment_ready;

const ActRaiserDeveloperEnvironment *
ActRaiser_GetDeveloperEnvironment(void) {
  if (atomic_load_explicit(&s_developer_environment_ready,
                           memory_order_acquire))
    return &s_developer_environment;

  ActRaiserDeveloperEnvironment env = {0};
  for (unsigned flag = 0; flag < kActRaiserDeveloperFlag_Count; flag++)
    env.flags[flag] = getenv(kActRaiserDeveloperFlagNames[flag]) != NULL;

  env.widescreen_only_bg_layer = -1;
  const char *value = getenv("AR_WS_ONLYBG");
  if (value && value[0]) {
    env.widescreen_only_bg_present = true;
    env.widescreen_only_bg_layer = atoi(value) - 1;
  }

  value = getenv("AR_WS_CLAMP");
  if (value && value[0]) {
    env.widescreen_clamp_present = true;
    env.widescreen_clamp_mask = (uint8_t)strtoul(value, NULL, 16);
  }

  s_developer_environment = env;
  atomic_store_explicit(&s_developer_environment_ready, true,
                        memory_order_release);
  return &s_developer_environment;
}

bool ActRaiser_DeveloperFlagEnabled(ActRaiserDeveloperFlag flag) {
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

bool ActRaiser_PpuShapeTraceActive(unsigned gf) {
  ActRaiserPpuShapeTrace *trace = &s_ppu_shape_trace;
  if (!trace->initialized) {
    const char *path = getenv("AR_PPU_SHAPE_TRACE");
    const unsigned any_gf = kActRaiserPpuShapeMaximumRecords;
    unsigned exact_gf = ActRaiser_PpuShapeUnsignedEnvironment(
        "AR_PPU_SHAPE_GF", any_gf, 0xffffu);
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
      trace->file = sr_fopen(path, "wb");
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
    /* Last, so the flag never advertises a window/file that is not filled in
     * yet -- the fields below are read on the strength of it. */
    trace->initialized = true;
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

void ActRaiser_PpuShapeCaptureRegisters(
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

void ActRaiser_PpuShapeTraceLine(
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
static void *s_host_fiber;   /* ConvertThreadToFiber result (driver thread) */
static void *s_game_fiber;   /* CreateFiber result (game coroutine) */
#else
static ucontext_t s_host_ctx;
static ucontext_t s_game_ctx;
static char *s_game_stack;        /* usable stack (guard page excluded) */
static void  *s_game_stack_map;   /* mmap base, including the guard page */
static size_t s_game_stack_map_len;
#endif
static bool s_game_started;
static bool s_game_coroutine_executing;
/* A host restart can discard the coroutine while an inline VBlank wait is
 * suspended. Its reentrancy guard belongs to that coroutine, not the process. */
static bool s_rdnmi_yielding;

static void SuspendGameCoroutine(void *unused) {
  (void)unused;
#ifdef _WIN32
  SwitchToFiber(s_host_fiber);
#else
  /* swapcontext can fail with ENOMEM ("Insufficient stack space left"). An
   * unchecked failure would silently return and keep running on a stack the
   * host believes it owns; there is no recovery, so abort loudly instead. */
  if (swapcontext(&s_game_ctx, &s_host_ctx) != 0) {
    fprintf(stderr, "FATAL: swapcontext (game -> host) failed\n");
    abort();
  }
#endif
}

void ActRaiser_YieldToHost(void) {
  /* The next host tick resumes this stack, not a new generated activation.
   * Preserve the runner's scopes even for native VBlank yields outside an
   * optional block checkpoint/poll callback. */
  cpu_yield_execution(SuspendGameCoroutine, NULL);
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
  if (!s_game_coroutine_executing) {
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
static unsigned s_action_load_armed_frames;
static unsigned s_action_load_hold_frames;
static uint64_t s_action_load_one_shot_token;
void ActRaiser_OnInidispWrite(uint8_t value) {
  uint32_t block = 0;
  (void)sr_block_history(&block, 1);
  unsigned frames = ActionLoadPacing_ForceBlankHoldFrames(
      g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_DestinationMapGroup], block, value);
  if (!frames || ActRaiser_DeveloperFlagEnabled(
                     kActRaiserDeveloperFlag_DisableActionLoadPacing))
    return;

  s_action_load_armed_frames = frames;
  /* The loader's many APU-port polls are statically collapsed into this one
   * host call. Converting their synthetic touch credit into SPC cycles fills
   * the DSP ring and drops roughly five seconds of authentic Advent audio
   * before the host can present it. Begin the calibrated hold at force blank,
   * before those polls: each yielded frame's runner-owned APU target (or an
   * audio consumer that reaches it first) advances the sequencer and makes its
   * acknowledgement ready while black is presented. The loader then resumes
   * without double-advancing the SPC. */
  RtlSetApuCatchupSuppressed(true);
  bool one_shot_completed = false;
  const uint64_t one_shot_token =
      MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
  if (ActionLoadPacing_ShouldReleaseForOneShot(
          frames, one_shot_token, one_shot_token, one_shot_completed)) {
    s_action_load_armed_frames = 0;
    s_action_load_one_shot_token = 0;
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
  s_action_load_one_shot_token = one_shot_token;
  s_action_load_hold_frames = frames - 1;
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
          s_action_load_armed_frames,
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
    s_action_load_armed_frames = 0;
    s_action_load_one_shot_token = 0;
    RtlSetApuCatchupSuppressed(false);
    return;
  }

  const unsigned frames = s_action_load_armed_frames;
  s_action_load_armed_frames = 0;
  RtlSetApuCatchupSuppressed(false);
  s_action_load_one_shot_token = 0;
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
                s_rdnmi_yielding ? 1 : 0, in_nmi ? 1 : 0,
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
  if (!(force_nmi && !s_rdnmi_yielding)) {
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
  if (force_nmi && !s_rdnmi_yielding && !no_4210_yield) {
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
      s_rdnmi_yielding = true;
      ActRaiser_YieldToHost();
      s_rdnmi_yielding = false;
      return 0x82;
    }
    /* Clear/post/ack reads do not yield and report no vblank. */
    return 0x02;
  }

  return -1;
}

void ActRaiser_SaveRegs(CpuState *c, CpuRegSnapshot *s) {
  s->A = c->A;
  s->X = c->X;
  s->Y = c->Y;
  s->S = c->S;
  s->D = c->D;
  s->DB = c->DB;
  s->PB = c->PB;
  s->P = c->P;
  s->m_flag = c->m_flag;
  s->x_flag = c->x_flag;
  s->emulation = c->emulation;
  s->host_return_valid = c->host_return_valid;
  s->fN = c->_flag_N;
  s->fV = c->_flag_V;
  s->fZ = c->_flag_Z;
  s->fC = c->_flag_C;
  s->fI = c->_flag_I;
  s->fD = c->_flag_D;
}

void ActRaiser_RestoreRegs(CpuState *c, const CpuRegSnapshot *s) {
  c->A = s->A;
  c->X = s->X;
  c->Y = s->Y;
  c->S = s->S;
  c->D = s->D;
  c->DB = s->DB;
  c->PB = s->PB;
  c->P = s->P;
  c->m_flag = s->m_flag;
  c->x_flag = s->x_flag;
  c->emulation = s->emulation;
  c->host_return_valid = s->host_return_valid;
  c->_flag_N = s->fN;
  c->_flag_V = s->fV;
  c->_flag_Z = s->fZ;
  c->_flag_C = s->fC;
  c->_flag_I = s->fI;
  c->_flag_D = s->fD;
}

/* Set while an NMI/IRQ handler is executing on the host stack (the calls
 * below are bracketed by SaveRegs/RestoreRegs, so cpu->S is restored after).
 * The stack-drift tripwire reads this to ignore handler-internal imbalance. */
volatile int g_sr_in_interrupt = 0;

void ActRaiser_EmitInterrupt(SrInterruptKind kind, uint32 flags,
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
  const bool extended = NativeAudioExtension_QueueGameRequest(
      g_ram, kSnesWramSize, false, id, site, game_frame, (uint16_t)cpu->X, (uint16_t)cpu->Y,
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
uint32 ActRaiser_LastBlockPc(void) {
  uint32 pc = 0;
  return sr_block_history(&pc, 1) == 1 ? pc : 0;
}

enum {
  kDialogueBlipRequest = 0x07,
  kDialogueBlipSite = 0x01902D,
};

/* Both native COPs and authored glyphs post through the same audio/event
 * routing. An authored request carries its origin explicitly; it does not
 * execute a CPU block or software interrupt. */
static void PostCopRequest(CpuState *cpu, uint8 id, uint32 site,
                           const char *source, bool suppress_native_blip) {
  /* $01:901C is the message composer's per-glyph pacing helper. Its
   * non-space path at $01:902D posts COP #$07 after drawing each character.
   * Suppress only this exact site: id 07 also drives unrelated game events. */
  const bool suppress_dialog_blip =
      id == kDialogueBlipRequest && site == kDialogueBlipSite &&
      (suppress_native_blip ||
       !AudioPresentationPolicy_ShouldEmitDialogBlip(
           g_settings.audio_dialog_blip));
  const uint64_t trace_serial = NativeAudioTrace_OnCpuRequest(
      kNativeAudioRequest_Event, id, !suppress_dialog_blip,
      source, site,
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
      (uint16_t)cpu->X, (uint16_t)cpu->Y);
  if (suppress_dialog_blip)
    NativeAudioExtension_ObserveGameState(g_ram, kSnesWramSize);
  const bool extended = !suppress_dialog_blip &&
      NativeAudioExtension_QueueGameRequest(
          g_ram, kSnesWramSize, true, id, site,
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
            game_frame, source ? source : "?",
            site, id, suppress_dialog_blip ? " suppressed-dialog-blip" : "",
            g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap]);
  }
}

void ActRaiser_RequestDialogueBlip(CpuState *cpu) {
  PostCopRequest(cpu, kDialogueBlipRequest, kDialogueBlipSite,
                 "enhanced-dialogue", false);
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
  if (observe_interrupt) {
    ActRaiser_EmitInterrupt(
        SR_INTERRUPT_COP, SR_EVENT_INTERRUPT_ENTER, site, vector,
        SR_INTERRUPT_SCANLINE_UNKNOWN, "cop");
  }
  const bool suppress_native_blip =
      id == kDialogueBlipRequest && site == kDialogueBlipSite &&
      ActRaiser_LocalizationConsumeNativeBlipSuppression();
  PostCopRequest(cpu, id, site, g_last_recomp_func, suppress_native_blip);
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
  FILE *file = sr_fopen(path, "w");
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

bool ActRaiser_QueryInputState(SrInputStateSnapshot *state) {
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
  f = sr_fopen(path, "wb");
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
    f = sr_fopen(path, "wb");
    if (f) { fwrite(vram.data, sizeof(*vram.data),
                    vram.element_count, f); fclose(f); }
    snprintf(path, sizeof path, "%s.cgram.bin", prefix);
    f = sr_fopen(path, "wb");
    if (f) { fwrite(cgram.data, sizeof(*cgram.data),
                    cgram.element_count, f); fclose(f); }
    snprintf(path, sizeof path, "%s.oam.bin", prefix);
    f = sr_fopen(path, "wb");
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
    f = sr_fopen(path, "wb");
    if (f) { fwrite(high_oam.data, 1, high_oam.byte_size, f); fclose(f); }
    ActRaiser_WritePpuSnapshotMetadata(prefix, &ppu_state);
  }
}

static void game_coroutine(void) {
  s_action_load_armed_frames = 0;
  s_action_load_hold_frames = 0;
  s_action_load_one_shot_token = 0;
  RtlSetApuCatchupSuppressed(false);
  cpu_state_init(&g_cpu, g_ram);
  g_cpu_brk_hook = ActRaiser_BrkHook;
  g_cpu_cop_hook = ActRaiser_CopHook;
  CpuReturnScope reset_owner;
  cpu_reset_scope_begin(&reset_owner, &g_cpu);
  RecompReturn result = ResetHandler_M1X1(&g_cpu);
  while (result == RECOMP_RETURN_TAILCALL ||
         (result == RECOMP_RETURN_OWNED_UNWIND && cpu_finish_reset_tail(&reset_owner, &g_cpu)))
    result = cpu_dispatch_pc_from(&g_cpu, g_tailcall_pc24, g_tailcall_miss_s, g_tailcall_src24);
  cpu_return_scope_end(&reset_owner);
  for (;;)
    ActRaiser_YieldToHost();
}

#ifdef _WIN32
static VOID CALLBACK game_coroutine_fiber(LPVOID param) {
  (void)param;
  game_coroutine();   /* never returns: for(;;) ActRaiser_YieldToHost() */
}
#endif

RecompReturn ActRaiser_CreditsWaitForReset(CpuState *cpu) {
  /* The ROM parks forever at $02:AADF (The End) or $02:AAE7 (Best Player).
   * Keep that final page and native CPU/stack intact, but let the host service
   * input and its reset/quit overlay between frames. A reset destroys this
   * coroutine; there is no native return address to pop or resume. */
  (void)cpu;
  for (;;)
    ActRaiser_YieldToHost();
}

RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {
  ActRaiserCredits_ObserveWait(cpu);
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
    fprintf(
        stderr,
        "[frame] f=%d gf=%u push+%lu callsite=%02x:%04x A=%04x m=%d $18=%02x $19=%02x $1A=%02x $1B=%02x $F4=%02x $F5=%02x $FB=%02x time$E6=%02x%02x HP$1D=%02x joy=%04x(raw=%04x) pos=%04x,%04x d=%+d,%+d vel=%+d,%+d h=%04x state=%04x boost=%02x crest=%02x\n",
        snes_frame_counter, game_frame, g_recomp_push_count - last_push, cpu->PB, ret, cpu->A,
        cpu->m_flag, map_group, current_map, g_ram[kActRaiserWram_DestinationMap],
        g_ram[kActRaiserWram_DestinationMapGroup], g_ram[kActRaiserWram_InputEnableMask],
        g_ram[kActRaiserWram_InputEnableMask + 1], g_ram[kActRaiserWram_TransitionRequest],
        g_ram[kActRaiserWram_ActionTimerHigh], g_ram[kActRaiserWram_ActionTimerLow],
        g_ram[kActRaiserWram_PlayerHp], joy, joy_raw, player_x, player_y, delta_x, delta_y,
        player_velocity_x, player_velocity_y, player_handler, player_flags,
        g_ram[kActRaiserWram_PlayerBoost], g_ram[kActRaiserWram_PlayerCrest]);
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
      fprintf(
          stderr,
          "[ppu] f=%d inidisp=%02x bright=%d fblank=%d bgmode=%02x main=%02x sub=%02x hdmaen=%02x\n",
          snes_frame_counter, display_control, display_control & 0xf,
          (display_control & 0x80) ? 1 : 0, RTL_GAME_PPU_BG_MODE_CONTROL(ppu_display),
          RTL_GAME_PPU_MAIN_SCREEN(ppu_display), RTL_GAME_PPU_SUB_SCREEN(ppu_display),
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
  if (!s_host_fiber) {
    s_host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (!s_host_fiber) {
      fprintf(stderr, "Failed to convert driver thread to fiber\n");
      return false;
    }
  }
  if (s_game_fiber) {
    DeleteFiber(s_game_fiber);
    s_game_fiber = NULL;
  }
  s_game_fiber = CreateFiberEx(kGameCoroutineStackCommitBytes,
                               kGameCoroutineStackReserveBytes,
                               FIBER_FLAG_FLOAT_SWITCH,
                               game_coroutine_fiber, NULL);
  if (!s_game_fiber) {
    fprintf(stderr, "Failed to create game coroutine fiber\n");
    return false;
  }
#else
  if (!s_game_stack) {
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
    s_game_stack_map = map;
    s_game_stack_map_len = total;
    s_game_stack = (char *)map + guard;
  }
  /* The abandoned context is just register state pointing into this stack;
   * re-running makecontext over the SAME buffer resets the entry point, so no
   * unmap/remap is needed (and none would be safe while the old context's
   * frames still nominally live there). */
  if (getcontext(&s_game_ctx) != 0) {
    fprintf(stderr, "Failed to capture game coroutine context\n");
    return false;
  }
  s_game_ctx.uc_stack.ss_sp = s_game_stack;
  s_game_ctx.uc_stack.ss_size = kGameCoroutineStackReserveBytes;
  s_game_ctx.uc_link = &s_host_ctx;
  makecontext(&s_game_ctx, game_coroutine, 0);
#endif
  return true;
}

/* Release the coroutine's stack/fiber. Called from the game's shutdown path so
 * the guard-page mapping and the fiber are not leaked, and so a leak checker
 * run against a clean exit stays quiet. Safe to call without a coroutine. */
void ActRaiser_DestroyGameCoroutine(void) {
  ActRaiserBg3Upload_Reset();
  ActRaiserSpriteOwnership_Reset();
  ActRaiserHleFatal_RegisterHostEscape(NULL);
  s_game_coroutine_executing = false;
  s_rdnmi_yielding = false;
#ifdef _WIN32
  if (s_game_fiber) {
    DeleteFiber(s_game_fiber);
    s_game_fiber = NULL;
  }
  if (s_host_fiber) {
    if (!ConvertFiberToThread())
      SessionFatal_Request("Could not release the game coroutine's owning fiber.");
    s_host_fiber = NULL;
  }
#else
  if (s_game_stack_map) {
    munmap(s_game_stack_map, s_game_stack_map_len);
    s_game_stack_map = NULL;
    s_game_stack_map_len = 0;
    s_game_stack = NULL;
  }
#endif
  s_game_started = false;
  s_action_load_armed_frames = s_action_load_hold_frames = 0;
  s_action_load_one_shot_token = 0;
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
  ActRaiserWorldResume_Observe(g_ram, kSnesWramSize,
      g_settings.remember_last_town && InputReplay_PolicyChangesAllowed());
  ActRaiserHud_ObserveScene(g_ram[kActRaiserWram_MapGroup],
                            g_ram[kActRaiserWram_CurrentMap]);
  ActRaiserCredits_ObserveScene(g_ram[kActRaiserWram_MapGroup],
                               g_ram[kActRaiserWram_CurrentMap]);
  NativeAudioExtension_ObserveGameState(g_ram, kSnesWramSize);
  if (!s_game_started) {
    /* config.ini and process environment layers are final by this point. */
    (void)ActRaiser_GetDeveloperEnvironment();
    s_game_started = true;
#if SNESRECOMP_WATCHDOG
    /* Give the runtime watchdog the coroutine yield to escape a stuck frame
     * with (the old longjmp out of this coroutine was UB / fiber-forbidden). */
    g_watchdog_yield_hook = ActRaiser_YieldToHost;
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

  /* This changes only the exact retail-ROM state proven by broken_text.rec;
   * all event selection and dispatch remains in the native town engine. */
  if (g_settings.fix_aitos_event_queue)
    (void)ActRaiser_RepairAitosEventQueueCollision(g_ram, kSnesWramSize);

  ActRaiser_ApplyCheats();   /* host-side cheats (live settings, default off) */

  if (s_action_load_hold_frames) {
    bool one_shot_completed = false;
    uint64_t one_shot_token = 0;
    if (s_action_load_one_shot_token) {
      one_shot_token =
          MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
    }
    if (ActionLoadPacing_ShouldReleaseForOneShot(
            s_action_load_hold_frames, s_action_load_one_shot_token,
            one_shot_token, one_shot_completed)) {
      if (ActRaiser_DeveloperFlagEnabled(
              kActRaiserDeveloperFlag_LoadPacingLog)) {
        fprintf(stderr,
                "[load-pace] f=%d HD one-shot complete; released forced "
                "blank %u frame(s) early\n",
                snes_frame_counter, s_action_load_hold_frames);
      }
      s_action_load_hold_frames = 0;
      s_action_load_one_shot_token = 0;
      /* A short enhanced cue is allowed to end the accuracy hold. Its muted
       * authentic sequencer may not have reached the loader acknowledgement,
       * so restore touch catch-up for the remaining collapsed work. */
      RtlSetApuCatchupSuppressed(false);
    } else {
      s_action_load_hold_frames--;
      if (!s_action_load_hold_frames)
        s_action_load_one_shot_token = 0;
      return;
    }
  }
  if (!ActRaiser_ControlGameTiming(
          true, 0u, NULL))
    return;
  s_game_coroutine_executing = true;
#ifdef _WIN32
  SwitchToFiber(s_game_fiber);
#else
  if (swapcontext(&s_host_ctx, &s_game_ctx) != 0) {
    s_game_coroutine_executing = false;
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
  s_game_coroutine_executing = false;
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
    ActRaiserRegional_ObserveInputRelease(&g_cpu);
    if (observe_interrupt) {
      ActRaiser_EmitInterrupt(
          SR_INTERRUPT_NMI, SR_EVENT_INTERRUPT_EXIT, interrupt_pc,
          interrupt_vector, SR_INTERRUPT_SCANLINE_UNKNOWN, "nmi");
    }
  }
}
