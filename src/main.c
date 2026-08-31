#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "action/action_obj_apron.h"
#include "snesrecomp/game/types.h"
#include "actraiser_rtl.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "config.h"
#include "crt_post.h"
#include "settings.h"
#include "settings_overlay.h"
#include "sim/sim3d_depth_pass.h"
#include "input_map.h"
#include "dev/scene_inspector.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "presentation_frame_generation.h"
#include "forced_input.h"
#include "save_system.h"
#include "hd_replacement_host.h"
#include "music_replacements.h"
#include "audio_presentation_policy.h"
#include "native_audio_extension.h"
#include "native_audio_mixer.h"
#include "render_comparison.h"
#include "dev/sfx_census.h"
#include "dev/native_audio_trace.h"
#include "display_geometry.h"
#include "run_dir.h"
#include "snesrecomp/host/launcher.h"
#include "snesrecomp/support/file.h"
#include "actraiser/actraiser_action_bg.h"
#include "actraiser_game.h"
#include "snesrecomp/game/trace.h"
#include "present.h"
#include "frame_slot.h"
#include "host/host_audio.h"
#include "dev/host_dev_tools.h"
#include "host/host_display.h"
#include "host/host_display_pacing.h"
#include "host/host_input.h"
#include "manual/manual_reader.h"
#include "ini_upgrade_apply.h"
#include "input_replay.h"
#include "dev/oracle_trace.h"
#include "portable_paths.h"
#include "runtime_settings.h"
#include "session_fatal.h"
#include "runtime_diagnostics.h"
#include "snesrecomp/runner.h"
#include "scheduled_settings.h"
#include "user_data_dir.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_render_metadata.h"
#include "sim/sim_world_map_build.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_background_voxels.h"
#include "sim/sim_town_canvas.h"
#include "sim/sim_world_map.h"
#include "sim/sim3d.h"
#include "constants.h"
#include "platform/sdl/render_sdl.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
enum {
  kDefaultPowerOnWramFill = 0x55,
  kDefaultPowerOnSramFill = 0x60,
  kUninitializedEnvironmentOption = -2,
  kPerformanceReportIntervalMs = kMillisecondsPerSecond,
  kPowerOnGameFrameSentinel =
      kDefaultPowerOnWramFill | (kDefaultPowerOnWramFill << 8),
};
/* Reverse-domain app identifier: compositors key window grouping and icon
 * lookup off this, and a shipped .desktop file must share its basename. */
#define AR_APP_IDENTIFIER "dev.quintet-enix.actraiser-recomp"
#define AR_APP_VERSION "0.1.0-dev"
/* Not static: present.c and host_display.c read these presentation resources
 * directly. They are boot-created once and, after that, either read-only
 * pointers or used synchronously on the main thread — not part of the
 * g_ppu/g_settings state boundary D6 fences off. */
SDL_Window *g_window;
ArRenderDevice g_render_device;
/* The SDL GPU renderer is the presentation backend. Individual optional
 * shader effects still check their own AR_GPU_FX_* toggles; this flag reports
 * that the mandatory GPU device and renderer were created successfully. */
bool g_gpu_shaders_requested;
bool g_gpu_shaders_active;
ArRenderTexture g_texture;
ArRenderTexture g_authentic_texture;
ArRenderTexture g_hud_bg_texture;
ArRenderTexture g_hud_obj_texture;
/* InspectorPresentationKind comes from the portable HUD-layout contract;
 * InspectorPresentationSelection lives in present.h. Both are shared by the
 * live hit-test and the FrameSlot-fed renderer. */
/* external: read by FrameSlot_Capture (frame_slot.c) */
InspectorPresentationSelection g_scene_inspector_presentation;
static bool g_window_hidden;  /* true while MINIMIZED or HIDDEN: skip present */
/* external: read by FrameSlot_Capture (frame_slot.c) */
int g_snes_width = kActRaiserAuthenticWidth,
    g_snes_height = kActRaiserAuthenticHeight;
/* Framebuffer sized for the PPU's full widescreen budget (512 wide) so the
 * active width can change live without reallocating storage; each frame uses
 * only the leading g_snes_width*4 bytes per row. Rows follow the same rule on
 * the other axis: capacity for the full vertical margin band, of which a frame
 * uses only 224 + g_ws_extra_top + g_ws_extra_bottom. */
_Static_assert(kHostDisplayFramebufferHeight >= SR_PPU_SURFACE_MAX_HEIGHT,
               "frame surfaces must hold every row the PPU can render");
uint8_t g_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
/* Complete native PPU result captured beside g_pixels before host presentation
 * extractions remove layers. It stays at the active scanline width (no OBJ
 * apron) because comparison presents only a native 256x224 crop. */
uint8_t g_authentic_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_bg_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
uint8_t g_hud_obj_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
/* Flat-mode mask of pixels for which BG1 wins the priority resolve of its
 * owning PPU screen. This remains correct in Marahna/Viper rooms where BG1
 * and OBJ are TS-only inputs to the final colour-add composite. */
uint8_t g_action_bg1_mask_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
/* Flat-mode mask of pixels for which BG2 wins the complete PPU main-screen
 * priority resolve. A BG2-stage presentation effect is multiplied by this
 * before compositing, so later BG1/OBJ art retains authentic occlusion. */
uint8_t g_action_bg2_mask_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];

/* Diorama per-plane capture buffers, indexed by kDioramaPlane_* (engine
 * sources = the priority-0 remainder of each layer, appended entries = the
 * priority-band splits; see diorama_planes.h). Dedicated set separate from
 * the HUD/HD overlay buffers (BG3/OBJ reuse those for the widescreen HUD
 * split, and HD replacements claim per-source capture slots — see §4.3).
 * Allocated lazily on first diorama capture (actraiser_rtl.c) and released at
 * shutdown. BG4 is never drawn in Mode 1, so excluded; the backdrop slot
 * stays NULL (RenderDiorama points it at g_pixels). */
uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
bool g_diorama_dump_pending;
bool g_diorama_frame_active;
ArRenderTexture g_diorama_textures[kDioramaPlane_Count];
ArRenderTexture g_sim_obj_atlas_texture;
ArRenderTexture g_sim3d_layer_textures[kSim3DPlane_Count];
ArRenderTexture g_sim3d_flat_texture;
bool g_sim3d_textures_ready;
bool g_sim3d_billboard_renderer_ready;

static void DestroyDioramaTextures(void) {
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(&g_render_device, g_diorama_textures[i]);
    g_diorama_textures[i] = ArRenderTexture_Invalid();
  }
}

static void CreateDioramaTextures(void) {
  /* Allocated at the PPU's full render-target size on BOTH axes, for the same
   * reason: the ABI surface limits already cover every horizontal and vertical
   * margin without a realloc. Only the leading snes_width x
   * (snes_height + ws_extra_top + ws_extra_bottom) region is uploaded
   * each frame; Diorama_Composite's UV window is expressed against these
   * allocated dimensions. */
  uint8_t *zero_fill =
      calloc(1, (size_t)SR_PPU_SURFACE_MAX_WIDTH *
                    SR_PPU_SURFACE_MAX_HEIGHT * 4);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    if (i == SR_PPU_OVERLAY_BG4)
      continue;
    const ArRenderTextureDesc desc = {
      .width = SR_PPU_SURFACE_MAX_WIDTH,
      .height = SR_PPU_SURFACE_MAX_HEIGHT,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = i == kDioramaPlane_Backdrop
          ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            &g_render_device, &desc, &g_diorama_textures[i]))
      continue;
    if (zero_fill)
      ArRenderDevice_UpdateTexture(
          &g_render_device, g_diorama_textures[i], NULL, zero_fill,
          SR_PPU_SURFACE_MAX_WIDTH * 4);
  }
  free(zero_fill);
}

extern const RtlGameModule kActRaiserGameModule;


static bool SettingsOverlayLiveCgram(
    uint16_t out_cgram[kSettingsOverlayLayerPaletteEntries]) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *runner = RtlGameRunner();
  SrBorrowedU16Span cgram = {
    .struct_size = sizeof(cgram),
  };
  if (!api || !runner || !out_cgram ||
      api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !(api->capabilities & SR_RUNNER_CAP_BORROWED_U16_SPANS) ||
      api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, &cgram) !=
          SR_RESULT_OK ||
      cgram.element_count < kSettingsOverlayLayerPaletteEntries)
    return false;
  memcpy(out_cgram, cgram.data,
         sizeof(uint16_t) * kSettingsOverlayLayerPaletteEntries);
  return true;
}

static bool CaptureTownCanvasPpuView(SrPpuStateSnapshot *ppu,
                                     SrBorrowedU16Span *vram,
                                     SrBorrowedU16Span *cgram) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *runner = RtlGameRunner();
  const uint64_t required_caps =
      SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_BORROWED_U16_SPANS;
  if (!api || !runner || !ppu || !vram || !cgram ||
      api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      (api->capabilities & required_caps) != required_caps)
    return false;
  *ppu = (SrPpuStateSnapshot){.struct_size = sizeof(*ppu)};
  *vram = (SrBorrowedU16Span){.struct_size = sizeof(*vram)};
  *cgram = (SrBorrowedU16Span){.struct_size = sizeof(*cgram)};
  return api->query_ppu_state(runner, ppu) == SR_RESULT_OK &&
      api->borrow_u16_memory(runner, SR_MEMORY_VRAM, vram) ==
             SR_RESULT_OK &&
      api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, cgram) ==
             SR_RESULT_OK &&
      vram->element_count >= SR_PPU_VRAM_WORD_COUNT &&
      cgram->element_count >= SR_PPU_CGRAM_WORD_COUNT &&
      vram->lifetime_generation == ppu->lifetime_generation &&
      cgram->lifetime_generation == ppu->lifetime_generation;
}

void NORETURN Die(const char *error) {
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

static void RtlDrawPpuFrame(void) {
  (void)RtlGameDrawPpuFrame();
}

/* Execute one canonical application-owned runner transaction. Replay input,
 * the emulated frame, post-frame diagnostics, and replay completion must stay
 * indivisible so turbo and ordinary pacing cannot acquire different ordering
 * as new per-frame services are added. */
static bool RunOneRecompiledFrame(uint32 live_inputs, bool *stop_running) {
  InputReplayFrameResult replay = InputReplay_Resolve(live_inputs);
  if (InputReplay_Failed()) {
    SessionFatal_Request("Input replay failed: %s",
                         InputReplay_LastError());
    *stop_running = true;
    return false;
  }
  if (replay.stop_requested) *stop_running = true;

  (void)RtlRunFrame(replay.inputs);
  OracleTrace_CompleteTick();
  if (SessionFatal_Requested()) {
    *stop_running = true;
    return false;
  }
  if (!InputReplay_CompleteTick(RtlGameRunner())) {
    SessionFatal_Request("Input replay failed: %s",
                         InputReplay_LastError());
    *stop_running = true;
    return false;
  }
  return true;
}

/* One emulated tick: sample input, run the recompiled game logic, apply
 * turbo's extra same-input sub-frames (§3.2 — unchanged mechanism, just
 * relocated so it fires once per emulated tick instead of once per outer
 * host iteration), and the AR_PERF/SNESRECOMP_APU_PROFILE instrumentation that measures
 * it (§3.5 — "wrap the per-tick RtlRunFrame"). Called once per outer
 * iteration by the headless loop (§3.6) and 0-N times per outer iteration by
 * the non-headless fixed-timestep accumulator loop (§3.1). */
static void RunOneEmulatedTick(bool *stop_running) {
  extern uint8 g_ram[];
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;
  uint64_t perf_t0 = perf_on ? SDL_GetTicks() : 0;
  /* SNESRECOMP_APU_PROFILE=<ms>: per-frame APU-stall attribution. Any game frame whose
   * wall time reaches the threshold (default 8 ms; the flag value overrides
   * when >= 2) prints one [apuprof] line splitting the frame into lock-wait
   * vs SPC catch-up vs handshake-spin vs upload vs music-hook time. */
  static int apuprof_ms = kUninitializedEnvironmentOption;
  if (apuprof_ms == kUninitializedEnvironmentOption) {
    apuprof_ms = RtlApuProfileIsEnabled()
        ? atoi(getenv("SNESRECOMP_APU_PROFILE")) : -1;
    if (apuprof_ms >= 0 && apuprof_ms < 2) apuprof_ms = 8;
  }
  uint64_t apuprof_t0 = 0;
  unsigned long apuprof_push0 = 0;
  uint64_t apuprof_loop0 = 0;
  if (apuprof_ms > 0) {
    extern unsigned long g_recomp_push_count;
    extern uint64_t g_watchdog_loop_headers;
    RtlApuProfileReset();
    apuprof_push0 = g_recomp_push_count;
    apuprof_loop0 = g_watchdog_loop_headers;
    apuprof_t0 = SDL_GetTicksNS();
  }

  const uint32 live_inputs = HostInput_SampleLiveInputs();

  /* Do not hold the APU lock for a whole frame. Every APU-touching path takes
   * RtlApuLock itself (RtlApuWrite,
   * snes_readBBus, ReadRegWord, the SPC upload HLE), and the engine's
   * audio thread renders in short locked batches precisely so the two
   * threads interleave. Holding the lock across the whole frame starved
   * the audio callback during transition frames — a map-load frame runs
   * 20-55 ms of collapsed multi-hardware-frame work ([apuprof] loops
   * 25k-75k vs ~3k normal), the callback missed 2-3 fill deadlines, and
   * every level/song transition audibly dropped out even with 250 ms of
   * DSP ring buffered. It also pinned scheduled port-write latency at the
   * produced+3-quanta ceiling (~50 ms) because `produced` could not
   * advance while the game thread held the lock. */
  if (!RunOneRecompiledFrame(live_inputs, stop_running)) return;
  /* TURBO ('t' toggle): real fast-forward = run extra game frames per
   * emulated TICK (not per rendered/present frame — that decoupling is
   * M5's job). Same input word each sub-frame (level-held buttons repeat;
   * fine for skipping sim waits). Cheats/pins apply inside RtlRunFrame, so
   * they hold during the skipped frames too. */
  if (HostInput_IsTurbo() && !*stop_running) {
    int mult = g_settings.turbo_multiplier;
    for (int tf = 1; tf < mult && !SessionFatal_Requested() &&
         !*stop_running; tf++) {
      if (!RunOneRecompiledFrame(live_inputs, stop_running)) break;
    }
    if (SessionFatal_Requested()) {
      *stop_running = true;
      return;
    }
  }
  if (apuprof_t0) {
    RtlApuProfile profile = {.struct_size = RTL_APU_PROFILE_V2_SIZE};
    extern unsigned long g_recomp_push_count;
    extern uint64_t g_watchdog_loop_headers;
    uint64_t dt_ns = SDL_GetTicksNS() - apuprof_t0;
    RtlApuProfileRead(&profile);
    if (dt_ns >=
        (uint64_t)apuprof_ms * kNanosecondsPerMillisecond) {
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      double audiowait_ms = RtlApuProfileTakeAudioWaitMax() /
          (double)kNanosecondsPerMillisecond;
      fprintf(stderr,
              "[apuprof] gf=%u dt=%.1fms lockwait=%.2fms "
              "portsync=%.2fms/%llucyc/%uc apu=%llu "
              "audio=%llu uploadctl=%llu timeline=%llu other=%llu "
              "reads=%u writes=%u "
              "hook=%.2fms upload=%.2fms schedlat=%llusmp pushes=%lu "
              "loops=%llu audiowait-max=%.2fms last=%s\n",
              gf, dt_ns / (double)kNanosecondsPerMillisecond,
              profile.lock_wait_ns /
                  (double)kNanosecondsPerMillisecond,
              profile.port_sync_ns /
                  (double)kNanosecondsPerMillisecond,
              (unsigned long long)profile.apu_cycles_port_sync,
              profile.port_sync_calls,
              (unsigned long long)profile.apu_cycles_total,
              (unsigned long long)profile.apu_cycles_audio_demand,
              (unsigned long long)profile.apu_cycles_upload_control,
              (unsigned long long)profile.apu_cycles_timeline,
              (unsigned long long)profile.apu_cycles_unattributed,
              profile.port_reads,
              profile.port_writes,
              profile.hook_ns / (double)kNanosecondsPerMillisecond,
              profile.upload_ns /
                  (double)kNanosecondsPerMillisecond,
              (unsigned long long)profile.scheduled_latency_max,
              g_recomp_push_count - apuprof_push0,
              (unsigned long long)(g_watchdog_loop_headers - apuprof_loop0),
              audiowait_ms,
              profile.last_port_function ? profile.last_port_function : "-");
    }
  }
  if (perf_on) {
    extern void snes_catchup_stats(uint64_t *calls, uint64_t *cycles);
    static uint64_t win_start, run_ms_sum, run_ms_max;
    static int win_frames;
    static uint64_t last_cu_calls, last_cu_cycles; static unsigned last_gf;
    uint64_t t1 = SDL_GetTicks();
    uint64_t dt = t1 - perf_t0;
    run_ms_sum += dt; if (dt > run_ms_max) run_ms_max = dt;
    win_frames++;
    if (!win_start) win_start = t1;
    if (t1 - win_start >= kPerformanceReportIntervalMs) {
      uint64_t cc, cy; snes_catchup_stats(&cc, &cy);
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      fprintf(stderr, "[perf] fps=%d run-ms avg=%.1f max=%llu gf+=%u "
              "apu-catchup calls=%llu cyc=%llu $18=%02x\n",
              win_frames, (double)run_ms_sum / win_frames,
              (unsigned long long)run_ms_max,
              (unsigned)(uint16)(gf - last_gf),
              (unsigned long long)(cc - last_cu_calls),
              (unsigned long long)(cy - last_cu_cycles),
              g_ram[kActRaiserWram_MapGroup]);
      last_cu_calls = cc; last_cu_cycles = cy; last_gf = gf;
      win_start = t1; run_ms_sum = 0; run_ms_max = 0; win_frames = 0;
    }
  }
}

/* Per-outer-iteration draw + present (§3.5 — "PPM screenshot capture" and
 * the draw step both stay per-outer-iteration, not per-tick: even if the
 * accumulator ran several catch-up ticks this iteration, we draw/present
 * only the LAST one's resulting PPU state once). Caller gates this on
 * "did at least one tick actually run" (headless: always; non-headless:
 * produced_frame).
 *
 * alpha (R17/C4): the sub-tick phase, forwarded to the present. Both headless
 * modes pass kPresentationFrameGenerationPhaseNone and retain
 * one-tick-per-iteration cadence (§3.6):
 * pure headless skips submission, while headless-video submits that tick to
 * its unpaced hidden compositor. */
static void DrawAndPresentFrame(HostDisplayPresentMode present_mode,
                                float alpha) {
  extern uint8 g_ram[];
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;

  uint64_t perf_draw_t0 = perf_on ? SDL_GetTicks() : 0;
  RtlDrawPpuFrame();
  DioramaPerformanceScope host_post_performance = {0};
  if (Diorama_IsActiveThisFrame())
    host_post_performance =
        DioramaPerformance_Begin(kDioramaPerformance_HostPost);
  /* Own the developed world tilemap instead of observing $7E:C000, which acts
   * and towns both reuse as unrelated scratch. This runs only on the game
   * thread, after an emulated tick reached a stable frame boundary. */
  SimWorldMap_BuildIfNeeded();
  /* #16: function-scope so the annotated sim outlives the block below and can
   * be published to FrameSlot_Capture around the HostDisplay_SubmitFrame tail. */
  SimFrameData sim;
  {
    extern int snes_frame_counter;
    SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram,
                         RtlGameRunner());
    SimRenderMetadata_CaptureFrame(
        &sim, g_ram, g_settings.sim3d_mode,
        g_settings.sim3d_world_navigation,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&sim, &tuning);
    SimWorldNavigationCapture_Capture(&sim, RtlGameRunner());
    /* This site runs for every drawn frame, including headless runs that never
     * call HostDisplay_SubmitFrame or FrameSlot_Capture. */
    SrPpuStateSnapshot town_ppu;
    SrBorrowedU16Span town_vram;
    SrBorrowedU16Span town_cgram;
    const bool have_town_ppu_view =
        Sim3D_TownCanvasNeedsPpuView(&sim) &&
        CaptureTownCanvasPpuView(&town_ppu, &town_vram, &town_cgram);
    Sim3D_RenderTownCanvas(
        &sim, g_ram,
        have_town_ppu_view ? &town_ppu : NULL,
        have_town_ppu_view ? &town_vram : NULL,
        have_town_ppu_view ? &town_cgram : NULL);
    sim.town_canvas_serial = SimTownCanvas_Serial();
    sim.background_voxel_serial = SimBackgroundVoxels_Serial();
    Sim3D_LogViewTransition(&sim);
    SceneInspector_SetSimFrameData(&sim);
    /* g_pixels is bound apron-wide; offset past the apron so the trace sees
     * the authentic frame at column 0, as it always has. */
    const size_t trace_pitch =
        ActionApron_SurfacePitch(g_snes_width, SR_PPU_OBJ_APRON);
    if (trace_pitch <= INT_MAX) {
      SimRenderMetadata_TraceFrame(
          (uint32)snes_frame_counter, &sim,
          g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON),
          g_snes_width, g_snes_height, (int)trace_pitch);
    }
  }
  /* AR_DIORAMA_DUMP_GF=<gf>[,<gf>...]: arm the Shift+D layer dump from a replay
   * instead of the keyboard, so a diorama frame can be inspected headlessly.
   * The PNGs keep the captured ALPHA, which is what makes F4's half-add
   * annotation verifiable without looking at the screen. Same shape as
   * AR_VRAMDUMP_GF. */
  {
    static const char *dump_list = NULL;
    static bool dump_list_read;
    static unsigned last_dumped_gf = (unsigned)-1;
    if (!dump_list_read) {
      dump_list_read = true;
      dump_list = getenv("AR_DIORAMA_DUMP_GF");
    }
    if (dump_list && dump_list[0]) {
      const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      if (gf != last_dumped_gf) {
        for (const char *at = dump_list; at && *at;) {
          if ((unsigned)strtoul(at, NULL, 0) == gf) {
            last_dumped_gf = gf;
            g_diorama_dump_pending = true;
            break;
          }
          const char *comma = strchr(at, ',');
          at = comma ? comma + 1 : NULL;
        }
      }
    }
  }
  if (g_diorama_dump_pending) {
    HostDevTools_DumpDioramaLayers();
    g_diorama_dump_pending = false;
    if (!g_settings.diorama_mode)
      ActRaiser_RebindPpuOutputSurfaces();
  }
  DioramaPerformance_End(host_post_performance);
  HostInput_MarkFrameDrawn();
  if (perf_on) {
    static uint64_t draw_win_start, draw_ms_sum, draw_ms_max;
    static int draw_win_frames;
    uint64_t now = SDL_GetTicks();
    uint64_t dt = now - perf_draw_t0;
    draw_ms_sum += dt;
    if (dt > draw_ms_max) draw_ms_max = dt;
    draw_win_frames++;
    if (!draw_win_start) draw_win_start = now;
    if (now - draw_win_start >= kPerformanceReportIntervalMs) {
      fprintf(stderr,
              "[draw-perf] frames=%d draw-ms avg=%.1f max=%llu "
              "$18=%02x $19=%02x authentic-capture=%s\n",
              draw_win_frames, (double)draw_ms_sum / draw_win_frames,
              (unsigned long long)draw_ms_max,
              g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap],
              ActRaiser_AuthenticCaptureEnabled() ? "on" : "off");
      draw_win_start = now;
      draw_ms_sum = 0;
      draw_ms_max = 0;
      draw_win_frames = 0;
    }
  }

  /* Framebuffer capture to PPM (works headless — g_pixels is always populated).
   * AR_SHOT_AT_GF=N      : one shot to saves/shot.ppm at game-frame >= N.
   * AR_SHOT_EVERY=N      : a SERIES — saves/shot_<gf>.ppm every N game-frames,
   *   optionally bounded by AR_SHOT_FROM / AR_SHOT_TO. Lets us compare steady
   *   state vs bug state frame by frame. */
  {
    static bool schedule_initialized;
    static bool shot_done;
    static bool shot_at_enabled;
    static bool shot_series_enabled;
    static unsigned shot_at;
    static unsigned shot_every;
    static unsigned shot_from;
    static unsigned shot_to;
    if (!schedule_initialized) {
      const char *value = getenv("AR_SHOT_AT_GF");
      shot_at_enabled = value && value[0];
      shot_at = shot_at_enabled ? (unsigned)strtoul(value, NULL, 0) : 0u;
      value = getenv("AR_SHOT_EVERY");
      shot_series_enabled = value && value[0];
      shot_every = shot_series_enabled
          ? (unsigned)strtoul(value, NULL, 0) : 0u;
      if (shot_series_enabled && !shot_every) shot_every = 1u;
      value = getenv("AR_SHOT_FROM");
      shot_from = value ? (unsigned)strtoul(value, NULL, 0) : 0u;
      value = getenv("AR_SHOT_TO");
      shot_to = value
          ? (unsigned)strtoul(value, NULL, 0) : UINT_MAX;
      schedule_initialized = true;
    }
    const unsigned gf =
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    int want = 0; char fname[320]; fname[0] = 0;
    if (shot_at_enabled && !shot_done && gf >= shot_at) {
      shot_done = true; want = 1;
      RunDirFile(fname, sizeof(fname), "shot.ppm");
    } else if (shot_series_enabled && gf >= shot_from && gf <= shot_to &&
               (gf % shot_every) == 0) {
      want = 1;
      RunDirFile(fname, sizeof(fname), "shot_%u.ppm", gf);
    }
    if (want) {
      FILE *pf = fopen(fname, "wb");
      if (pf) {
        const ArRenderExtentI shot_size =
            HostDevTools_WriteFramebufferPpm(pf);
        fclose(pf);
        int margin_left = 0;
        int margin_right = 0;
        ActRaiser_LiveMargins(&margin_left, &margin_right);
        fprintf(stderr, "[shot] wrote %s at gf=%u (%dx%d) margins=%d/%d mode=%s\n",
                fname, gf, shot_size.width, shot_size.height,
                margin_left, margin_right,
                Settings_DisplayModeName(g_settings.display_mode));
      }
    }
  }

  if (present_mode != kHostDisplayPresent_None) {
    /* FrameSlot_Capture inside this call copies the sim annotated above
     * instead of recomputing it (identical inputs, same thread, nothing
     * mutates them in between). Cleared immediately after: the screenshot
     * and paused/menu-redraw captures run outside this window and must
     * self-annotate. */
    FrameSlot_SetPendingAnnotatedSim(&sim);
    (void)HostDisplay_SubmitFrame(present_mode, alpha);
    FrameSlot_SetPendingAnnotatedSim(NULL);
  }
}

/* Host-side work that follows one or more completed emulation ticks. Catch-up
 * still coalesces it to one pass, but retained-frame redraws do not run it:
 * host presentation can outpace emulation (dramatically in Unlimited), and
 * multiplying SRAM scans or host/APU policy checks by presentation throughput
 * both wastes work and contaminates the rendering measurement. */
static void RunPostTickHousekeeping(void) {
  extern uint8 g_ram[];
  /* Surface audio-chunk drops the callback counted (R12). Reported here, off
   * the audio thread, and coalesced so a sustained problem cannot spam. */
  {
    int dropped = HostAudio_TakeRejectedChunkCount();
    if (dropped) {
      static int total;
      total += dropped;
      fprintf(stderr, "[audio] %d chunk(s) rejected by SDL_PutAudioStreamData "
                      "(%d total this session) — audio glitched\n",
              dropped, total);
    }
  }

  /* Complete the SPC engine's resident uploader once it enters the $CC-wait,
   * for the case where the CPU's HLEd $9A56 ran before the engine got there
   * (takes its own APU lock — must be outside the lock above). */
  ActRaiser_SpcUploaderCompleteTick();

  /* Music replacement live policy (setting toggled off mid-song). Takes
   * its own APU lock — also outside the lock above. */
  MusicReplacements_FrameTick();

  /* AR_WARP_AT=<gameframe>: fire the AR_WARP target automatically once the
   * 16-bit game-frame counter reaches the value. Headless runs can't press
   * F6; used e.g. to sweep the warp table capturing each level's music src
   * (AR_MUSICLOG). Same transition-capable-state caveats as F6. */
  {
    static long warp_at = kUninitializedEnvironmentOption;
    static bool warp_fired;
    if (warp_at == kUninitializedEnvironmentOption) {
      const char *at = getenv("AR_WARP_AT");
      warp_at = (at && at[0]) ? strtol(at, NULL, 0) : -1;
    }
    if (warp_at >= 0 && !warp_fired) {
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      /* The power-on fill value is numerically above ordinary scheduled
       * frames. Ignore it just like AR_DIORAMA_AT below, or windowed startup
       * can stage a warp before the game has initialized its transition
       * state. */
      if (gf != kPowerOnGameFrameSentinel && gf >= (unsigned)warp_at) {
        warp_fired = true;
        (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
      }
    }
  }

  /* AR_DIORAMA_AT=<gameframe>: flip Diorama 3D on once the game-frame counter
   * reaches the value, through the same descriptor path the D hotkey uses.
   * Booting straight into diorama changes the widescreen margin budget and
   * changes the rendered baseline, so a visual-regression run should replay
   * flat into the stage and only then switch. Canonical input is host-tick
   * ordered; the game-frame value here is only the deterministic trigger. */
  {
    static long diorama_at = kUninitializedEnvironmentOption;
    static bool diorama_fired;
    if (diorama_at == kUninitializedEnvironmentOption) {
      const char *at = getenv("AR_DIORAMA_AT");
      diorama_at = (at && at[0]) ? strtol(at, NULL, 0) : -1;
    }
    if (diorama_at >= 0 && !diorama_fired) {
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      /* $0088 is $5555-filled before the game initialises it; ignore that
       * boot sentinel or every target fires on frame 0. */
      if (gf != kPowerOnGameFrameSentinel &&
          gf >= (unsigned)diorama_at) {
        diorama_fired = true;
        const SettingDesc *mode = Settings_Find("diorama_mode");
        if (mode && Settings_IsAvailable(mode) && !g_settings.diorama_mode) {
          Settings_SetLong(mode, 1);
          fprintf(stderr, "[diorama] ON via AR_DIORAMA_AT at gf=%u\n", gf);
        }
      }
    }
  }

  Diorama_FlushSettingsIfDirty();
  Sim3DCamera_FlushSettingsIfDirty();

  /* Auto-persist battery SRAM the moment the game writes a save, so progress
   * survives a freeze/force-quit (the clean-exit save-system write never runs
   * if the game hangs). Cheap: only writes when the 8KB SRAM actually changes.
   * SKIPPED during input replay: letting a diagnostic run overwrite save.srm
   * would change the initial state of the NEXT replay and invalidate canonical
   * initial-state/checkpoint digests as well as legacy frame alignment. */
  if (!InputReplay_ShouldProtectSaveData()) {
    static bool write_error_reported;
    static uint64_t first_write_failure_ms;
    SaveError error = {{0}};
    if (!SaveSystem_AutoPersistIfChanged(&error)) {
      if (!write_error_reported)
        fprintf(stderr, "[saves] auto-persist failed: %s\n", error.message);
      write_error_reported = true;
      const uint64_t now_ms = SDL_GetTicks();
      if (!first_write_failure_ms) first_write_failure_ms = now_ms;
      if (now_ms - first_write_failure_ms >= 5000) {
        SessionFatal_Request(
            "The game could not write your battery save for five seconds "
            "(%s). It is closing instead of letting you continue with "
            "unsaved progress. Check free disk space and permissions for %s.",
            error.message, SaveSystem_ActivePath());
      }
    } else {
      write_error_reported = false;
      first_write_failure_ms = 0;
    }
  }
}

/* One application-level host-pause edge owns both transport layers. The order
 * matters: stop the device before latching the OGG decoder, then release the
 * decoder before resuming the device, so no callback can advance only one
 * source across the edge. */
static void ApplyHostAudioPause(bool paused) {
  static bool initialized;
  static bool applied_pause;
  if (initialized && applied_pause == paused) return;
  initialized = true;
  applied_pause = paused;
  bool success = true;
  if (paused) success = HostAudio_SetHostPaused(true);
  MusicReplacements_SetHostPaused(paused);
  if (!paused) success = HostAudio_SetHostPaused(false);
  if (!success) {
    SessionFatal_Request(
        "The audio device stopped accepting the game's audio stream (%s). "
        "Restart the game after checking the selected output device. If the "
        "problem repeats, choose another device or buffer size.",
        SDL_GetError());
  }
}


/* ---------------------------------------------------------------------------
 * Boot decomposition.
 *
 * main() was a single 1,311-line function: argument parsing, config, SDL and
 * window/renderer/texture creation, subsystem injection, the frame loop, and
 * teardown, all inline. It is now a sequence of named phases over one context.
 *
 * ORDER IS THE CONTRACT HERE. Nearly every phase below documents something that
 * must happen before or after something else -- the portable chdir before any
 * relative path resolves, the shipped-defaults upgrade before any config read,
 * RunDirInit before anything prints, the widescreen budget before presentation
 * resources are allocated, the visual patches between cart_load and
 * Randomizer_Init. These are called in exactly the order the inline code ran.
 * Do not reorder them to make the call site read more nicely.
 * ------------------------------------------------------------------------- */
typedef struct AppBoot {
  const char *rom_path;
  const char *config_path;
  uint8 *rom_data;
  size_t rom_size;
  bool headless;        /* no window/renderer; PPU emulation still runs */
  bool headless_video;  /* headless, but with a hidden-window renderer */
  bool video;           /* !headless || headless_video */
  bool ws_headless;     /* opt a headless run into the configured wide geometry */
  Snes *snes;
} AppBoot;

/* Argument parsing, the portable-bundle chdir, the per-run artifact dir, the
 * shipped-defaults ini upgrade, the config layer, and the ROM read.
 * Returns a process exit code on failure, or -1 to continue booting. */
static int AppBoot_ParseArgs(AppBoot *app, int argc, char **argv) {
  app->rom_path = NULL;
  app->config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      app->config_path = argv[++i];
    } else if (argv[i][0] != '-') {
      app->rom_path = argv[i];
    }
  }

  /* Portability: in a shipped bundle, chdir next to the executable so
   * config.ini, settings.ini, saves/, runs/, and game-assets/ resolve — and
   * are regenerated by the existing mkdir paths — beside the binary no matter
   * where it was launched from (double-click, moved folder, script). ROM and
   * config arguments are absolutized first so a relative path on the command
   * line still resolves after the chdir. An in-tree dev build has no marker
   * beside build/ActRaiserRecomp, so the CWD stays authoritative and the dev
   * workflow is unchanged. Must precede RunDirInit (console tee into runs/)
   * and any relative file access. */
  static char rom_abs[kHostPathCapacity], config_abs[kHostPathCapacity];
  if (PortablePaths_IsBundle()) {
    if (app->rom_path && snesrecomp_abspath(app->rom_path, rom_abs, sizeof rom_abs))
      app->rom_path = rom_abs;
    if (app->config_path &&
        snesrecomp_abspath(app->config_path, config_abs, sizeof config_abs))
      app->config_path = config_abs;
    snesrecomp_anchor_to_exe_dir();
  }

  /* Per-run artifact ringfence (runs/<ts>/): must run before anything prints
   * (console tee) or reads an AR_* output path. See run_dir.h. */
  RunDirInit(argc, argv);

  cpu_trace_init();

  /* AR_DRIFT_FRAME=N: arm the stack-drift tripwire to fire on the first
   * NORMAL function exit at/after frame N whose exit S != entry S (the
   * unbalanced push/pop leaker). Diagnostic only. */
#if SNESRECOMP_TRACE
  { const char *v = getenv("AR_DRIFT_FRAME");
    if (v && v[0]) {
      extern void cpu_trace_arm_stack_drift_tripwire(int32_t);
      cpu_trace_arm_stack_drift_tripwire((int32_t)strtol(v, NULL, 0));
      fprintf(stderr, "[AR_DRIFT_FRAME] stack-drift tripwire armed at frame %s\n", v);
    } }
#endif

  /* Upgrade step, BEFORE anything reads a config file: merge the bundle's
   * shipped defaults into the user's live copies, keeping every value they
   * changed and adding only what is new in this version. A no-op in a developer
   * checkout (no defaults/ directory) and silent when nothing changed.
   *
   * Here rather than only in the builder GUI because run-game starts the game
   * directly, so a GUI-only upgrade would never run for those users. */
  IniUpgrade_ApplyShippedDefaults();

  Settings_ClearConfigLayer();
  if (app->config_path)
    ParseConfigFile(app->config_path);
  else
    ParseConfigFile("config.ini");

  /* Now that config-file AR_* values are env-bridged, point bare output
   * filenames into the per-run dir (see run_dir.h). */
  RunDirRebaseEnvOutputs();

  /* One authoritative line identifies both build capability and resolved
   * runtime mode. This prevents a trace-capable diagnostic build with tracing
   * off from being confused with a play/release build where the recorder was
   * compiled out entirely. Persist the same line beside replay artifacts. */
  const char *trace_status = sr_trace_status();
  fprintf(stderr, "[runner-trace] %s\n", trace_status);
  RunDirRecordTraceStatus(trace_status);

  if (!app->rom_path) {
    fprintf(stderr, "Usage: %s <rom.sfc> [--config config.ini]\n", argv[0]);
    return 1;
  }

  app->rom_size = 0;
  app->rom_data = snesrecomp_read_whole_file(app->rom_path, &app->rom_size);
  if (!app->rom_data) {
    fprintf(stderr, "Error: cannot open ROM file '%s'\n", app->rom_path);
    return 1;
  }
  fprintf(stderr, "Loaded ROM: %s (%zu bytes)\n", app->rom_path, app->rom_size);
  return -1;
}

/* Resolve headless/video mode and the widescreen budget, then load settings and
 * finalize the display mode. Must precede any presentation-resource allocation:
 * the display presets are only authoritative once g_ws_active/g_ws_extra are. */
static void AppBoot_ResolveDisplayAndSettings(AppBoot *app) {
  /* Headless mode for the differential-oracle harness: no window/renderer,
   * run uncapped. PPU emulation still runs (HDMA/IRQ timing affects game
   * state); only the on-screen present is skipped. Parallels snesref's
   * SNESREF_HEADLESS. */
  app->headless = getenv("AR_HEADLESS") && getenv("AR_HEADLESS")[0]
                  && getenv("AR_HEADLESS")[0] != '0';
  app->headless_video = app->headless && getenv("AR_HEADLESS_VIDEO") &&
                        getenv("AR_HEADLESS_VIDEO")[0] &&
                        getenv("AR_HEADLESS_VIDEO")[0] != '0';
  app->video = !app->headless || app->headless_video;

  /* Widescreen budget from config. internal_width = 224 * (ax/ay) display
   * units, divided by the 7:6 pixel stretch when the 4:3-corrected look is
   * on (AspectPAR=4:3, default): 16:9 -> 342 px (extra=43/side), 16:10 -> 308
   * (26); square pixels: 399 (72) / 359 (52). Headless (oracle/differential)
   * runs force authentic geometry so comparisons never see wide framebuffers,
   * unless AR_WS_HEADLESS=1 explicitly opts a visual-regression run into the
   * configured wide geometry. The oracle harness leaves it unset. */
  app->ws_headless = getenv("AR_WS_HEADLESS") && getenv("AR_WS_HEADLESS")[0]
                     && getenv("AR_WS_HEADLESS")[0] != '0';
  HostDisplay_SetWidescreenRuntimeAllowed(!app->headless || app->ws_headless);
  /* Resolve application and game settings before allocating presentation
   * resources. Known config.ini values were staged by ParseConfigFile;
   * settings.ini overrides them, and real environment variables win last.
   * The default load path is the SAME portable-relative location every
   * Settings_Save site writes. AR_SETTINGS_PATH still wins so replay fixtures
   * (tools/sim3d_demo.py) can keep their pinned settings. */
  char settings_file[kHostPathCapacity];
  const char *settings_path = getenv("AR_SETTINGS_PATH");
  if (!settings_path || !settings_path[0])
    settings_path = UserDataFile(settings_file, sizeof settings_file,
                                 "settings.ini");
  Settings_InitWithFile(settings_path);
  HostDisplay_ResolveVideoGeometry(false);

  /* Display presets depend on whether the resolved aspect selected a wide
   * budget. Finalize only after g_ws_active/g_ws_extra are authoritative. */
  Settings_FinalizeDisplayMode();
}

/* The SNESRECOMP_ENTRY_MX_CHECK / SNESRECOMP_MX_HISTORY / SNESRECOMP_EXIT_MX_CHECK / SNESRECOMP_CALL_MX_CHECK / SNESRECOMP_TRAP_FUNCTION family: runtime
 * m/x invariant checks and call-stack traps. All diagnostic, all opt-in, and all
 * resolved once here so no hot path pays a getenv. */
static void AppBoot_ArmDiagnostics(void) {
  /* SNESRECOMP_ENTRY_MX_CHECK=1: enable the per-function-entry m/x invariant check
   * (validates the emitter's static m/x analysis on every direct call). */
  { extern int g_sr_entry_mx_check_enabled; const char *e = getenv("SNESRECOMP_ENTRY_MX_CHECK");
    g_sr_entry_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* SNESRECOMP_MX_HISTORY=1: per-PC runtime m/x histogram + live misdecode anomaly trap. */
  { extern int g_sr_mx_history_enabled; extern void sr_mx_history_dump(void);
    const char *e = getenv("SNESRECOMP_MX_HISTORY");
    g_sr_mx_history_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (g_sr_mx_history_enabled) atexit(sr_mx_history_dump); }
  /* SNESRECOMP_EXIT_MX_CHECK=1: per-function EXIT m/x check — fires when a function's runtime
   * exit (m,x) differs from what the emitter told its callers (exit-mx
   * misdecode, e.g. $03:9156). SNESRECOMP_EXIT_STACK_CHECK=1: per-function EXIT stack-balance
   * check — fires when a paired frame's RTS/RTL drifts S (e.g. $01:B8CF).
   * Symmetric twins of SNESRECOMP_ENTRY_MX_CHECK; name the culprit at its own return. */
  { extern int g_sr_exit_mx_check_enabled; const char *e = getenv("SNESRECOMP_EXIT_MX_CHECK");
    g_sr_exit_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  { extern int g_sr_exit_stack_check_enabled; const char *e = getenv("SNESRECOMP_EXIT_STACK_CHECK");
    g_sr_exit_stack_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* SNESRECOMP_CALL_MX_CHECK=1: per-CALL-SITE m/x invariant check — fires at every JSR/JSL
   * when runtime (m,x) disagrees with what the decoder statically knew at
   * that exact instruction. Catches (m,x) corruption from ANYWHERE upstream
   * of a call (not just decode-time mistakes SNESRECOMP_ENTRY_MX_CHECK/SNESRECOMP_EXIT_MX_CHECK cover),
   * narrowed to the first call site downstream of the corruption. */
  { extern int g_sr_call_mx_check_enabled; const char *e = getenv("SNESRECOMP_CALL_MX_CHECK");
    g_sr_call_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }

  /* SNESRECOMP_TRAP_FUNCTION=<substring>: dump the recomp call stack the first time a matching
   * function is entered (finds the dispatch chain into a misdecode variant). */
  { extern const char *g_sr_trap_function;
    const char *e = getenv("SNESRECOMP_TRAP_FUNCTION");
    g_sr_trap_function = (e && e[0]) ? e : 0; }

}

/* Every presentation texture, created once the renderer exists: the base
 * framebuffer, the HUD BG/OBJ planes, the Mode-7 overlay, the D1b semantic OBJ
 * atlas, the D2 SIM capture family, and the diorama planes. Split out of
 * AppBoot_CreateVideo, which otherwise carried SDL init, window creation and
 * renderer configuration in the same 300 lines. */
static void AppBoot_CreatePresentationTextures(void) {
  const ArRenderTextureDesc base_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = g_snes_height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &base_texture, &g_texture))
    Die(ArRenderDevice_LastError(&g_render_device));
  /* The base framebuffer is opaque: the PPU writes RGB with the alpha byte
   * left 0 (see ppu_old.c). SDL2 defaulted new textures to BLENDMODE_NONE so
   * that alpha was ignored, but SDL3 defaults them to BLENDMODE_BLEND — which
   * would blend those alpha-0 pixels to fully transparent and present a BLACK
   * screen. The descriptor's opaque blend mode preserves that behavior. (The
   * HUD/overlay textures below deliberately use alpha; they carry real alpha.) */
  /* SDL3 textures default to linear filtering; the SDL2 build set the global
   * SDL_HINT_RENDER_SCALE_QUALITY=0 (nearest). The descriptor pins nearest
   * filtering so the pixel-art framebuffer upscales crisply. */

  const ArRenderTextureDesc authentic_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = SR_PPU_SURFACE_MAX_HEIGHT,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &authentic_texture, &g_authentic_texture))
    Die(ArRenderDevice_LastError(&g_render_device));

  const ArRenderTextureDesc hud_texture = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = g_snes_height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &hud_texture, &g_hud_bg_texture) ||
      !ArRenderDevice_CreateTexture(
          &g_render_device, &hud_texture, &g_hud_obj_texture))
    Die(ArRenderDevice_LastError(&g_render_device));

  /* D1b semantic OBJ atlas. It is uploaded every supported SIM frame but is
   * not selected by the compositor until the later separated-composite
   * capability lands, keeping this checkpoint visually authentic. */
  const ArRenderTextureDesc sim_atlas_texture = {
    .width = kSimObjAtlasWidth,
    .height = kSimObjAtlasHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (ArRenderDevice_CreateTexture(
          &g_render_device, &sim_atlas_texture,
          &g_sim_obj_atlas_texture)) {
    /* Static storage is zero-initialized before the game thread starts. */
    ArRenderDevice_UpdateTexture(
        &g_render_device, g_sim_obj_atlas_texture, NULL,
        g_sim_obj_atlas_pixels, kSimObjAtlasPitch);
  } else {
    fprintf(stderr,
            "[sim3d-d1] semantic atlas texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
  }
  g_sim3d_billboard_renderer_ready =
      ArRenderTexture_IsValid(g_sim_obj_atlas_texture);

  /* D2's observational Mode-1 capture family. Layer textures are retained
   * for inspector/future geometry use; the pitch-zero reference and its
   * absolute-difference image have dedicated opaque streaming textures. */
  g_sim3d_textures_ready = true;
  const ArRenderTextureDesc sim_layer_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!ArRenderDevice_CreateTexture(
            &g_render_device, &sim_layer_texture,
            &g_sim3d_layer_textures[plane])) {
      g_sim3d_textures_ready = false;
      break;
    }
  }
  const ArRenderTextureDesc sim_flat_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &sim_flat_texture, &g_sim3d_flat_texture))
    g_sim3d_textures_ready = false;
  if (!g_sim3d_textures_ready) {
    fprintf(stderr,
            "[sim3d-d2] capture textures unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      ArRenderDevice_DestroyTexture(
          &g_render_device, g_sim3d_layer_textures[plane]);
      g_sim3d_layer_textures[plane] = ArRenderTexture_Invalid();
    }
    ArRenderDevice_DestroyTexture(&g_render_device, g_sim3d_flat_texture);
    g_sim3d_flat_texture = ArRenderTexture_Invalid();
  }
  if (g_settings.sim3d_mode && !g_sim3d_textures_ready) {
    Die("Simulation town 3D is enabled, but its core capture textures could "
        "not be created. Restart after checking graphics memory and driver "
        "stability, or disable Simulation town 3D in settings.ini.");
  }
  if (g_settings.sim3d_mode &&
      (Settings_Sim3DRequestedFeatures() & kSimFeature_ObjectBillboards) &&
      !g_sim3d_billboard_renderer_ready) {
    Die("Simulation object billboards are enabled, but their renderer atlas "
        "could not be created. Restart after checking graphics memory and "
        "driver stability, or disable object billboards in settings.ini.");
  }

  HdReplacementHost_LoadTextures();

  /* One streaming texture per diorama plane (priority bands included).
   * Only the backdrop is opaque — every other plane alpha-blends. */
  /* Live report (2026-07-21): a persistent pink/garbage-colored line at
   * the diorama's right edge, root-caused across two failed attempts (the
   * B1b-crisp supersample copy, then suspected in the DOF/edge-AA shader)
   * before landing on the actual source: every consumer that ever samples
   * near the true edge of what Diorama_Upload writes (u=uv_u1 =
   * snes_width/SR_PPU_SURFACE_MAX_WIDTH, always < 1.0 — the buffer is
   * allocated at the PPU's max width but a layer's real content is narrower,
   * capped by kActRaiserWidescreenExtraMax's tilemap-ring streaming limit)
   * can reach into
   * columns snes_width..SR_PPU_SURFACE_MAX_WIDTH-1, which Diorama_Upload's
   * SDL_UpdateTexture never touches. SDL_TEXTUREACCESS_STREAMING content
   * is undefined until written (no zero guarantee, confirmed non-zero in
   * practice on this backend), so that tail is genuine garbage, not just
   * theoretically risky — and every fix so far (B1b's UV-window clamp,
   * B1b-crisp's valid-subrect blit, the skybox blur's UV inset) was
   * patching ONE consumer at a time as each was discovered, while the DOF/
   * edge-AA shader's own unclamped blur sampling proved there would always
   * be another. Fix it once at the SOURCE instead: zero-fill each
   * texture's FULL extent immediately after creation, before any real
   * frame ever writes into it. Diorama_Upload only ever touches the valid
   * {0,0,snes_width,snes_height} sub-rect afterward, so the margin stays
   * deterministically transparent black (not garbage) for the texture's
   * entire lifetime — every current and future consumer is safe without
   * needing its own clamp/inset workaround. */
  CreateDioramaTextures();
}

/* SDL init, window, renderer, and every presentation texture. The window/renderer
 * body is skipped for a pure-headless run; a headless_video run takes it with a
 * hidden window so the present path still executes for frame capture.
 * Returns a process exit code if SDL_Init fails, or -1 to continue booting. */
static int AppBoot_CreateVideo(AppBoot *app) {
  /* App metadata, BEFORE SDL_Init — SDL documents that it "should be called as
   * early as possible, before SDL_Init", and it cannot be retrofitted later.
   * The identifier "must be in reverse-domain format" and is what "desktop
   * compositors [use] to identify and group windows together": without it a
   * Linux/Deck window gets a generic icon and no taskbar grouping, and the
   * matching .desktop file (same basename as this identifier) has nothing to
   * associate with. */
  SDL_SetAppMetadata(kWindowTitle, AR_APP_VERSION, AR_APP_IDENTIFIER);
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");

  SDL_InitFlags sdl_flags = SDL_INIT_AUDIO;
  if (app->video) sdl_flags |= SDL_INIT_VIDEO;
  if (!app->headless) sdl_flags |= SDL_INIT_GAMEPAD;
  /* SDL3 returns true on success (the SDL2 0-on-success convention flipped). */
  if (!SDL_Init(sdl_flags)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (app->video) {
    /* Which backend SDL actually chose. A "dummy"/"offscreen" driver makes
     * every video call succeed while nothing reaches the screen (audio is
     * unaffected), so a silent window is otherwise indistinguishable from a
     * working one. Listing the compiled-in drivers also tells you instantly
     * whether a hand-supplied libSDL3 was built without a real backend. */
    const char *driver = SDL_GetCurrentVideoDriver();
    fprintf(stderr, "[video] driver: %s (available:", driver ? driver : "(none)");
    for (int i = 0, n = SDL_GetNumVideoDrivers(); i < n; i++)
      fprintf(stderr, " %s", SDL_GetVideoDriver(i));
    fprintf(stderr, ")\n");
    if (driver && (SDL_strcmp(driver, "dummy") == 0 ||
                   SDL_strcmp(driver, "offscreen") == 0))
      Die("SIM3D requires a real GPU video driver; dummy/offscreen is unsupported");

    int scale = g_settings.window_scale ? g_settings.window_scale : 3;
    /* Window sized to the DISPLAY aspect: with the 4:3-corrected PAR the
     * rendered width (e.g. 342) is narrower than the displayed width (16:9 of
     * the height), so derive the window from the target ratio, not the
     * framebuffer. Faithful mode keeps the historical width*scale.
     *
     * Must use the DISPLAY crop (Settings_VisibleWidth), not g_snes_width:
     * diorama mode inflates the render width to the full
     * kActRaiserWidescreenExtraMax margin
     * (HostDisplay_ResolveVideoGeometry) while the displayed width stays
     * aspect-derived. HostDisplay_CalculateWindowSize shares the same
     * calculation with later explicit scale/aspect changes. */
    /* Clamp the scale to what the desktop can actually hold — the setting
     * allows up to 8x (~2400px wide), which overflows small laptop panels
     * (1366x768) with no recourse: the oversized window's title bar can land
     * off-screen. Usable bounds (excludes docks/taskbars) of the primary
     * display, checked against the WIDEST possible window for this scale
     * (the 16:9-of-height display width); shrink until it fits, floor 1x. */
    /* Points, not pixels — the same conversion
     * HostDisplay_ApplyWindowScale uses, so boot and later re-apply agree on
     * what Nx means. The density is not known until the window exists, so use
     * the primary display's content scale as the boot-time stand-in; the first
     * HostDisplay_UpdateProperties call corrects it. */
    {
      float boot_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
      if (boot_scale > 1.0f) {
        int points = (int)((float)scale / boot_scale + 0.5f);
        scale = points > 0 ? points : 1;
      }
    }
    {
      SDL_Rect usable;
      if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        while (scale > 1 &&
               ((g_snes_height * scale * 16 + 4) / 9 > usable.w ||
                g_snes_height * scale > usable.h))
          scale--;
      }
    }
    int win_w;
    int win_h;
    HostDisplay_CalculateWindowSize(scale, &win_w, &win_h);
    /* SDL3 merged FULLSCREEN_DESKTOP into FULLSCREEN (borderless desktop is
     * the default fullscreen mode when no exclusive video mode is set).
     * Exclusive fullscreen's video mode is set after window creation by
     * HostDisplay_ApplyWindowMode; at boot the flag just requests fullscreen. */
    /* HIGH_PIXEL_DENSITY: request a native-resolution backing store on
     * scaled displays (Retina macOS, scaled Wayland). Without it SDL creates
     * a 1x store and the compositor upscales — the game, PAR resample, and
     * overlay all render soft at logical resolution. Downstream needs no
     * change: every consumer sizes itself from SDL_GetRenderOutputSize, and
     * HostDisplay_WindowPointToOutput already maps window points to output
     * pixels. */
    SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (app->headless_video ? SDL_WINDOW_HIDDEN : 0) |
        (g_settings.window_mode != kWindowMode_Windowed
             ? SDL_WINDOW_FULLSCREEN : 0);
    /* SDL3 SDL_CreateWindow no longer takes an x,y position; it is created at
     * a default (centered) position. */
    g_window = SDL_CreateWindow(
      kWindowTitle,
      win_w, win_h,
      window_flags
    );
    if (!g_window) Die("SDL_CreateWindow failed");

    /* SIM3D now relies on per-pixel depth testing, so SDL's cross-platform
     * GPU renderer is a baseline requirement rather than an optional shader
     * effects switch. Hidden capture windows use the same backend: a software
     * renderer would produce screenshots from a different visibility model.
     * SPIR-V feeds Vulkan, DXIL feeds D3D12, and MSL feeds Metal; unsupported
     * machines fail at launch instead of silently returning to painter
     * sorting. */
    g_gpu_shaders_requested = true;
    g_settings.gpu_shaders_enabled = true;  /* legacy config/UI mirror */
    if (!ArSdlRenderBackend_CreateForWindow(
            &g_render_device, g_window))
      Die("SDL GPU render backend creation failed");
    if (!Sim3DDepthPass_Require(&g_render_device))
      Die(Sim3DDepthPass_LastError());
    g_gpu_shaders_active = true;
    /* Apply the selected refresh policy after renderer creation. Hidden-video
     * automation requests vsync off and uses no host throttle; a platform
     * swapchain may still serialize SDL_RenderPresent at its own cadence.
     * Interactive Limit/Uncapped modes use host deadlines; VSync delegates to
     * SDL and Unlimited deliberately has no host throttle. */
    if (app->headless_video)
      HostDisplay_DisableVsync();
    else
      HostDisplay_ApplyRefreshVsync();

    /* Exclusive fullscreen needs its video mode set after creation; borderless
     * and windowed are already handled by the creation flag. */
    if (!app->headless_video && g_settings.window_mode == kWindowMode_Exclusive)
      HostDisplay_ApplyWindowMode();
    HostDisplay_UpdateProperties();

    /* Aspect-correct letterboxing via SDL's logical presentation — one
     * implementation shared with the resize/settings paths so boot and runtime
     * can never disagree (4:3-PAR encodes the 7:6 stretch in the logical size;
     * Screen ratio > Stretch opts out of aspect fitting). */
    HostDisplay_RecomputeLogicalPresentation();

    AppBoot_CreatePresentationTextures();

    /* Take keyboard focus on launch. A window created by SDL is ordered in
     * but the process is not necessarily activated — launched from a terminal
     * (or as an un-bundled binary on macOS) the shell keeps focus and the
     * game starts behind it, silently swallowing input until the user clicks
     * on it. SDL_RaiseWindow both raises and, with the default
     * SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, activates the application.
     * Deliberately last in the video setup so focus lands on a window that is
     * fully configured, and skipped for headless_video (that window is
     * SDL_WINDOW_HIDDEN and must never steal focus from a batch run). */
    if (!app->headless_video && !SDL_RaiseWindow(g_window))
      fprintf(stderr, "[window] could not raise to foreground: %s\n",
              SDL_GetError());
  }
  return -1;
}

/* Overlay, world map, diorama manifest, the injected overlay hooks (layer editor,
 * manual), input, and music. Injection rather than direct calls is what keeps
 * settings_overlay.c testable with no renderer at all -- see settings_overlay.h. */
static void AppBoot_InstallSubsystems(AppBoot *app) {
  if (!SettingsOverlay_Init(&g_render_device, g_window,
                            app->rom_data, app->rom_size))
    Die("font atlas creation for settings overlay failed");
  /* The world-map image and pure development-builder tables are immutable ROM
   * data. Failure is not fatal: consumers retain the authentic presentation. */
  if (SimWorldMap_Init(app->rom_data, app->rom_size))
    SimWorldMapBuild_Init(app->rom_data, app->rom_size);
  if (!Diorama_InitRomBackdrops(app->rom_data, app->rom_size))
    fprintf(stderr, "[diorama] named ROM backdrops unavailable\n");
  if (!ActRaiserActionBg_InitRoomScenes(app->rom_data, app->rom_size))
    fprintf(stderr, "[action-room-scene] immutable loader unavailable\n");
  /* Per-room diorama layer overrides. Absent file is the normal case and leaves
   * every room drawing as built. */
  Diorama_LoadLayerManifest();
  SettingsOverlay_SetInspectorInfoProvider(
      HostDevTools_FormatInspectorInfo);
  /* The layer editor (Settings > Layers, developer-only) edits the override
   * table loaded above and writes the manifest back. Injected rather than called
   * directly from the overlay so that file stays testable without diorama.c --
   * see settings_overlay.h. */
  SettingsOverlay_SetLayerEditorHooks(Diorama_LayerOverrides, Diorama_LiveRoom,
                                      Diorama_SaveLayerManifest);
  SettingsOverlay_SetLayerPaletteProvider(SettingsOverlayLiveCgram);

  /* The in-game manual, injected for the same reason: it owns textures and an
   * image decoder, and the overlay's own test links settings_overlay.c with no
   * renderer at all. Nothing is loaded here -- the availability hook reads and
   * indexes the file when the overlay first needs it; page textures remain lazy
   * until the player opens the reader. */
  static const SettingsOverlayManualHooks kManualHooks = {
    .available = ManualReader_Available,
    .is_open = ManualReader_IsOpen,
    .close = ManualReader_Close,
    .render = ManualReader_Render,
    .handle_key = ManualReader_HandleKey,
    .handle_pad = ManualReader_HandleGamepadEvent,
  };
  SettingsOverlay_SetManualHooks(&kManualHooks);

  RuntimeSettings_Install();
  /* After the action observer is installed: the pad's save/load-state
   * bindings route through it. */
  InputMap_Init();
  HostInput_InstallActionHandler();
  RenderComparison_Reset();
  Diorama_SeedCameraFromSettings();

  /* Music replacement is audio-side and works headless too (unlike the HD
   * manifest load above, which needs the renderer for textures). Same
   * manifest file; [music:] sections. AR_MUSIC_MANIFEST overrides. */
  {
    const char *music_manifest = getenv("AR_MUSIC_MANIFEST");
    if (!music_manifest || !music_manifest[0])
      music_manifest = "game-assets/manifest.ini";
    MusicReplacements_Load(music_manifest);
    MusicReplacements_InstallHooks();
    NativeAudioExtension_Install();
    NativeAudioMixer_Install();
    AudioPresentationPolicy_Reset();
  }

  /* After music: the census chains the APU port seam music installs. */
  SfxCensus_Init();
}

/* Register the game, bring up the SNES, apply the deterministic visual patches
 * (which must sit between cart_load and Randomizer_Init), fill power-on WRAM and
 * battery SRAM, load the persisted save, and honour AR_LOADSTATE. */
static void AppBoot_StartGame(AppBoot *app) {
  if (RtlRegisterGame(&kActRaiserGameModule) != SR_RESULT_OK)
    Die("The linked game module is incompatible with this runner.");
  app->snes = SnesInit(app->rom_data, (int)app->rom_size);
  if (!app->snes) Die("SnesInit failed");
  if (!RuntimeDiagnostics_Bind(RtlGameRunner()))
    Die("runner diagnostics observer bind failed");
  if (!NativeAudioTrace_Init(RtlGameRunner()))
    Die("native audio trace observer bind failed");

  /* Lifecycle initialization applies deterministic visual source-data
   * adjustments after cartridge loading and before Randomizer_Init snapshots
   * the live ROM. A signature mismatch is safe but important: it means effects
   * metadata and the running visual script no longer share the investigated
   * USA-ROM contract. */
  const ActRaiserRomSetupResult rom_setup =
      ActRaiser_LastRomSetupResult();
  if (!rom_setup.visual_patches_applied)
    fprintf(stderr,
            "[sim-visuals] house-fire cadence patch skipped: "
            "unexpected ROM signature\n");

  if (!rom_setup.randomizer_initialized && g_settings.rando_enable) {
    Die("The Randomizer is enabled, but its pristine ROM snapshot could not "
        "be created. Verify that the configured ROM is supported and that "
        "enough memory is available, or disable Randomizer in settings.ini.");
  }

  HdReplacementHost_BindSurfaces();
  ActRaiser_RebindPpuOutputSurfaces();
  /* Frame-0 margin state: pillarboxed-authentic (render the 256 columns
   * centered in the wide framebuffer). The ABI surface rebind above configures
   * it; ActRaiser_ApplyWidescreenPolicy reapplies per-frame policy after the
   * PPU reset clears the live fields. */

  /* Power-on WRAM fill. The SNES does not clear WRAM at power-on; snes9x (our
   * reference emulator) fills it with the 0x55 pattern, and ActRaiser's title
   * sequence depends on that — with zero-filled WRAM the title's per-frame loop
   * takes a path that underflows the SNES stack and crashes into the $2100
   * open-bus reads (bank_02_AF86). Match snes9x: fill g_ram with 0x55 before
   * boot so uninitialized-RAM reads agree with the reference. AR_WRAM_INIT
   * overrides with an exact dump (used by the differential harness). */
  {
    const char *fenv = getenv("AR_WRAM_FILL");
    int fill = fenv
        ? (int)strtoul(fenv, NULL, 0)
        : kDefaultPowerOnWramFill;
    memset(g_ram, fill, kActRaiserWramSize);
    const char *wp0 = getenv("AR_WRAM_INIT");
    if (wp0 && wp0[0]) {
      FILE *f = fopen(wp0, "rb");
      if (f) { size_t n = fread(g_ram, 1, kActRaiserWramSize, f); fclose(f);
        fprintf(stderr, "[wram-init] seeded %zu bytes from %s\n", n, wp0); }
      else fprintf(stderr, "AR_WRAM_INIT: cannot open %s\n", wp0);
    }
  }

  /* Power-on battery SRAM fill. A never-written cartridge battery is NOT zero;
   * snes9x (our reference) powers SRAM up to the 0x60 pattern, and ActRaiser
   * validates its save data — an all-zero SRAM is misread as a corrupt/level-0
   * save (the "must be level 1" symptom) instead of "blank -> new game". Match
   * the reference so the save-validity check behaves identically. Only applies
   * to a fresh cart (cart_load zero-fills it); a real .sav load overrides. */
  {
    extern uint8 *g_sram; extern int g_sram_size;
    const char *senv = getenv("AR_SRAM_FILL");
    int sfill = senv ? (int)strtoul(senv, NULL, 0)
                     : kDefaultPowerOnSramFill;
    if (g_sram && g_sram_size > 0) memset(g_sram, sfill, g_sram_size);
  }

  /* Load persisted battery save (overrides the fresh-cart fill if present).
   * Portable builds use saves/ beside the executable after the bundle anchor;
   * developer runs use saves/ under their launch directory. */
  char saves_dir[kHostPathCapacity], save_srm[kHostPathCapacity],
      save_ini[kHostPathCapacity], legacy_srm[kHostPathCapacity];
  UserDataFile(saves_dir, sizeof saves_dir, "saves");
  mkdir(saves_dir, 0755);
  {
    extern uint8 *g_sram; extern int g_sram_size;
    SaveError error = {{0}};
    const char *native_path = getenv("AR_SAVE_NATIVE_PATH");
    const char *ini_path = getenv("AR_SAVE_INI_PATH");
    if (!native_path || !native_path[0]) {
      UserDataFile(save_srm, sizeof save_srm, "saves/save.srm");
      native_path = save_srm;
    }
    if (!ini_path || !ini_path[0]) {
      UserDataFile(save_ini, sizeof save_ini, "saves/save.ini");
      ini_path = save_ini;
    }
    if (!SaveSystem_Attach(g_sram, (size_t)g_sram_size,
                           (SaveBackend)g_settings.save_backend,
                           native_path, ini_path, &error))
      Die(error.message);
    snprintf(legacy_srm, sizeof(legacy_srm), "%s/%s.srm",
             saves_dir, RtlGameIdentifier());
    if (!SaveSystem_MigrateLegacyNative(legacy_srm, &error))
      Die(error.message);
    if (!SaveSystem_LoadActive(&error)) {
      char message[512];
      snprintf(message, sizeof(message),
               "The active save could not be loaded: %s\n\nThe game will "
               "not start with fresh SRAM because doing so could overwrite "
               "recoverable progress. Repair, restore, or move %s and try "
               "again.",
               error.message, SaveSystem_ActivePath());
      Die(message);
    }

    SaveEditRequest edits;
    bool staged = RuntimeSettings_BuildSaveEditRequest(&edits);
    if (staged && g_settings.save_edit_armed) {
      if (!SaveSystem_ApplyEdits(
              &edits, true, false, g_settings.save_autobackup, &error))
        fprintf(stderr, "[save-editor] boot edits rejected: %s\n",
                error.message);
      else
        fprintf(stderr, "[save-editor] boot edits applied for this session\n");
    } else if (staged) {
      fprintf(stderr,
              "[save-editor] staged boot edits ignored; save editing is not armed\n");
    }
  }

  OracleTrace_Init(RtlGameRunner());
  ForcedInput_Init();
  InputReplay_Init();
  /* A replay must not mutate the player's configuration, for the same reason it
   * refuses to persist SRAM. Set from the same predicate so the two protections
   * cannot drift apart. */
  Settings_SetPersistenceEnabled(!InputReplay_ShouldProtectSaveData());
  ScheduledSettings_Init();

  if (!HostAudio_Init(Settings_AudioFrequencyHz(), g_settings.audio_samples,
                      g_settings.audio_master_volume,
                      g_settings.audio_enabled)) {
    Die("The selected audio output could not be opened. Check the system "
        "output device, then restart the game. You can also change the "
        "audio buffer or sample-rate setting before launching again.");
  }

  /* AR_LOADSTATE=<slot>: load a savestate at boot (before the main loop), so a
   * headless/instrumented run can start from a captured moment instead of
   * replaying from power-on. Runs a few frames first so the game reaches a
   * stable frame boundary, then loads — matches the F7 hotkey path. */
  { const char *ls = getenv("AR_LOADSTATE");
    if (ls && ls[0]) {
      int slot = atoi(ls);
      for (int i = 0; i < 4 && !SessionFatal_Requested(); i++) {
        RtlRunFrame(0);
        OracleTrace_CompleteTick();
      }
      if (!SessionFatal_Requested()) {
        RtlSaveLoad(kSaveLoad_Load, slot);
        FrameSlot_ResetActionEffects();
        ActRaiserActionBg_Reset();
        fprintf(stderr, "[loadstate] loaded slot %d at boot\n", slot);
      }
    } }
  /* Canonical replay identity is defined by the state that will execute its
   * first recorded runner tick, so validate/write the header only after the
   * optional boot savestate has replaced the power-on state. */
  if (!InputReplay_BeginSession(RtlGameRunner(), RtlGameIdentifier()))
    Die(InputReplay_LastError());
}

/* The SDL event pump. One long switch over event types -- flat and skimmable
 * the way a dispatch table is, since every arm is independent. Clears
 * *running on quit. */
static void AppLoop_PumpEvents(AppBoot *app, bool *running) {
  SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          *running = false;
          break;
        /* Dragging the window to another monitor, or that monitor changing
         * mode, can change the refresh rate the Vsync row reports — and the
         * SAME monitor can re-mode under us (user flips 60->144Hz in OS
         * settings, Windows dynamic refresh re-modes on power state), which
         * arrives as a display-level event, not a window one. The R2 soft-
         * cap and the menu's UI pacing both derive from this value. The same
         * events change the window's PIXEL DENSITY (dragging between a Retina
         * and a 1x monitor), which the pinned HUD/menu scale percentages are
         * corrected by — so refresh both together. */
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
          HostDisplay_WindowDisplayChanged();
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
          HostDisplay_WindowDisplayScaleChanged();
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        case SDL_EVENT_DISPLAY_ADDED:
          HostDisplay_DisplayModeChanged(event.display.displayID);
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_DISPLAY_REMOVED:
          HostDisplay_DisplayRemoved(event.display.displayID);
          HostInput_RequestPausedRedraw();
          break;
        /* The window is the USER's: a drag-resize re-derives the picture inside
         * it and nothing else. HostDisplay_RecomputeLogicalPresentation never
         * calls SDL_SetWindowSize — calling the window-sizing path here
         * snapped every manual resize straight back to window_scale and could
         * oscillate on a fractional-scale compositor, where SDL notes the
         * granted size "may not match the exact size requested". The render
         * resolution and aspect settings are still fully respected: they
         * letterbox inside whatever size the user chose.
         *
         * Rendering is synchronous, so re-deriving logical presentation here
         * cannot race a composite. */
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
          HostDisplay_RecomputeLogicalPresentation();
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
          g_window_hidden = true;
          break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
          g_window_hidden = false;
          HostDisplay_ResetVsyncPacing();
          HostInput_RequestPausedRedraw();
          break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
          HostDisplay_ResetVsyncPacing();
          HostInput_RequestPausedRedraw();
          break;
        /* GPU device/target reset: STATIC textures lose their contents and
         * must be recreated — both the HD replacements and the settings
         * overlay's atlases (fonts/icons/dialog frame, uploaded once at
         * Init). DEVICE_LOST is unrecoverable. */
        case SDL_EVENT_RENDER_TARGETS_RESET:
        case SDL_EVENT_RENDER_DEVICE_RESET:
          HostDisplay_ResetVsyncPacing();
          if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            Diorama_ResetRendererResources(&g_render_device);
            DestroyDioramaTextures();
            CreateDioramaTextures();
          }
          /* Manual pages are STATIC textures too. Drop their cache before any
           * stale pointer can be mistaken for a resident page; the next manual
           * frame re-decodes from the retained PDF bytes under its normal budget. */
          ManualReader_DestroyTextures();
          HdReplacementHost_ReloadTextures();
          if (!SettingsOverlay_ReloadTextures(app->rom_data, app->rom_size)) {
            SessionFatal_Request(
                "The graphics device reset, but the settings and controls "
                "overlay could not be restored (%s). Restart the game after "
                "checking graphics-driver stability.",
                SDL_GetError());
          }
          /* The sim-3D caches are serial-gated on GAME state, so they would
           * never notice the reset and would keep presenting discarded
           * contents for the rest of the session (a settled town never bumps
           * the underlay serial). Drop them so the next present re-bakes. */
          PresentRendererResources_Reset();
          HostInput_RequestPausedRedraw();
          /* R17/C2: the retained re-present slot copies opaque HD texture
           * handles. The host reload just destroyed and recreated every one,
           * so those copies are now stale even though their native pointer
           * representation is hidden. Drop the slot; the next tick retains a
           * fresh one. This also keeps retained-frame upload skipping from
           * relying on resources invalidated by the reset. */
          HostDisplay_InvalidatePresentHistory();
          break;
        case SDL_EVENT_RENDER_DEVICE_LOST:
          SessionFatal_Request(
              "The graphics device was lost and cannot continue this session "
              "(%s). Restart the game after checking graphics-driver and GPU "
              "stability.",
              SDL_GetError());
          break;
        case SDL_EVENT_KEY_DOWN:
          /* An armed binding row consumes the raw key: it needs the scancode,
           * and it must win over F5/F9/etc. so those stay bindable. */
          if (SettingsOverlay_HandleCaptureEvent(&event)) break;
          /* Steam Input can emit a keyboard mapping and a native gamepad
           * event for one physical control. Auto mode gives the live gamepad
           * event ownership, including host hotkeys, so the synthesized key
           * cannot perform a second action. Key-up is still processed below
           * to ensure a previously accepted key can never stick. */
          if (HostInput_KeyboardIsSuppressed()) break;
          if (SettingsOverlay_IsOpen()) {
            /* Only the menu's active device drives navigation, so one
             * physical press (+ its synthesized twin) moves the menu once. */
            if (HostInput_MenuKeyboardIsActive()) {
              bool was_open = true;
              bool consumed = SettingsOverlay_HandleKey(
                  event.key.key, true, event.key.repeat != 0);
              if (was_open && !SettingsOverlay_IsOpen())
                HostInput_ClearHeld();
              if (consumed) break;
            } else {
              /* Menu owns the screen but keyboard isn't its device: swallow
               * the key so it never reaches gameplay HandleInput. */
              break;
            }
          }
          /* The settings UI is host-owned and safe in every emulated state.
           * Escape/F1 are not SNES inputs, so consume them before HandleInput
           * and clear held joypad state before freezing game advancement. */
          if (!event.key.repeat &&
              (event.key.key == SDLK_ESCAPE ||
               event.key.key == SDLK_F1)) {
            HostInput_ClearHeld();
            SettingsOverlay_Open();
          } else if (event.key.key == SDLK_P) {
            if (SceneInspector_HasSelection()) {
              const bool inspector_owned_pause =
                  HostInput_InspectorOwnsPause();
              HostInput_CloseInspectorSelection();
              if (!inspector_owned_pause) HostInput_TogglePause();
            } else {
              HostInput_TogglePause();
            }
          } else if (event.key.key == SDLK_T) {
            HostInput_ToggleTurbo();
          } else if (event.key.key == SDLK_F3) {
            if (!event.key.repeat) {
              const SettingDesc *inspector = Settings_Find("scene_inspector");
              SettingChangeResult result = Settings_SetLong(
                  inspector, !g_settings.scene_inspector);
              char settings_path[kHostPathCapacity];
              UserDataFile(settings_path, sizeof settings_path, "settings.ini");
              if (result > kSettingChange_Unchanged &&
                  !Settings_Save(settings_path))
                fprintf(stderr,
                        "[scene-inspector] could not save settings.ini\n");
              fprintf(stderr, "[scene-inspector] %s (%s)\n",
                      g_settings.scene_inspector
                          ? "enabled — click the game to inspect"
                          : "disabled",
                      Settings_ChangeResultName(result));
            }
          } else if (event.key.key == SDLK_MINUS ||
                     event.key.key == SDLK_KP_MINUS) {
            if (!event.key.repeat)
              HostDevTools_AdjustHudOutputScale(-25);
          } else if (event.key.key == SDLK_EQUALS ||
                     event.key.key == SDLK_PLUS ||
                     event.key.key == SDLK_KP_PLUS) {
            if (!event.key.repeat)
              HostDevTools_AdjustHudOutputScale(25);
          } else if (event.key.key == SDLK_F5) {
            (void)RuntimeSettings_HandleAction(Settings_Find("save_state"));
          } else if (event.key.key == SDLK_F7) {
            (void)RuntimeSettings_HandleAction(Settings_Find("load_state"));
          } else if (event.key.key == SDLK_F9) {
            /* Cycle 4:3 -> widescreen RAW -> widescreen FULL, for capturing
             * before/after comparison shots without a settings UI. Requires
             * booting with ExtendedAspectRatio set: the wide framebuffer and
             * window are sized once at boot, so an authentic-booted run has no
             * margins to reveal and stays pinned to 4:3. Shift+F9 retains the
             * long-standing diagnostic dump command. Ignore key-repeat so one
             * physical press advances exactly one preset. */
            if (event.key.repeat) {
              /* no-op */
            } else if (event.key.mod & SDL_KMOD_SHIFT) {
              DumpDiagState("hotkey");
            } else if (!g_ws_active) {
              fprintf(stderr, "[display] F9 needs ExtendedAspectRatio "
                      "(e.g. 16:9) in config.ini; staying 4:3\n");
            } else {
              /* A1 (followup doc): Settings_CycleDisplayMode now routes
               * through Settings_SetLong, whose FinishChange fires
               * OnRuntimeSettingChanged — that observer already re-derives
               * the logical presentation and sets g_paused_redraw_pending for
               * kSettingCat_Display. Doing it again here would re-mutate the
               * renderer redundantly. */
              int m = Settings_CycleDisplayMode();
              fprintf(stderr, "[display] mode %d/%d -> %s\n", m + 1,
                      kDisplayMode_PresetCount, Settings_DisplayModeName(m));
            }
          } else if (event.key.key == SDLK_F6) {
            /* Level warp: stage the game's own sim->act transition to the raw
             * registry target seeded by AR_WARP=<region_hex><map_hex>. The low byte is $19,
             * not a uniform act number (e.g. Kasandora act 2 is 0303). Press
             * from a transition-capable state; see docs/manual.md + docs/SEAMS.md. */
            (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
          } else if (event.key.key == SDLK_F2 || event.key.key == SDLK_C) {
            /* C is the one-hand alias for F2, deliberately NOT gated on
             * `!event.key.repeat`: holding it fires on every key repeat, which
             * is how you sweep a glitch that only shows for a frame or two
             * without knowing its game-frame up front. Each press is still a
             * full snapshot (~21 MB of .ppm alone), so a long hold writes GBs
             * — for a REPLAY prefer the deterministic sweep, which captures
             * exact frames and costs nothing to repeat:
             *   AR_INPUT_REPLAY=<rec> AR_SHOT_EVERY=1 AR_SHOT_FROM=a AR_SHOT_TO=b
             *   AR_INPUT_REPLAY=<rec> AR_VRAMDUMP_GF=g1,g2,...
             * Use the key to find the moment, those to pin it. */
            /* On-demand FULL snapshot — each press writes a unique set of files
             * tagged with the game-frame: WRAM + VRAM + CGRAM + OAM (via
             * ActRaiser_FullSnapshot) plus a .ppm screenshot. Lets several
             * moments be grabbed while driving the game manually so the
             * internals (esp. VRAM, where the bridge tiles live) can be watched
             * change over time alongside the picture. */
            /* If F9 and F2 were queued in the same paused host iteration,
             * render the new preset before capturing it. */
            HostDevTools_TakeFullSnapshot();
          } else if (event.key.key == SDLK_D && !event.key.repeat) {
            if (event.key.mod & SDL_KMOD_SHIFT) {
              if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
                fprintf(stderr, "[diorama] layer dump requires an action stage "
                        "($18=%02x)\n", g_ram[kActRaiserWram_MapGroup]);
              } else {
                g_diorama_dump_pending = true;
                fprintf(stderr, "[diorama] layer capture armed for next frame\n");
              }
            } else {
              /* Route through the descriptor so the hotkey, the menu, and
               * settings.ini stay one path — the change callback does the
               * geometry rebind. */
              const SettingDesc *mode = Settings_Find("diorama_mode");
              if (mode && !Settings_IsAvailable(mode)) {
                fprintf(stderr, "[diorama] requires the new renderer\n");
              } else if (mode) {
                Settings_SetLong(mode, !g_settings.diorama_mode);
                fprintf(stderr, "[diorama] %s\n",
                        g_settings.diorama_mode ? "ON" : "OFF");
              }
            }
          } else if (g_settings.diorama_mode && !event.key.repeat &&
                     event.key.key >= SDLK_1 && event.key.key <= SDLK_5) {
            /* Layer visibility hotkeys, gated behind diorama so the digits
             * stay free otherwise. Order matches the on-screen back-to-front
             * stack: 1 backdrop, 2 BG2, 3 BG1, 4 sprites, 5 HUD. */
            static const char *const kLayerKeys[] = {
              "diorama_layer_backdrop", "diorama_layer_bg2",
              "diorama_layer_bg1", "diorama_layer_obj", "diorama_layer_bg3",
            };
            int index = (int)(event.key.key - SDLK_1);
            const SettingDesc *row = Settings_Find(kLayerKeys[index]);
            long value = 0;
            if (row && Settings_GetLong(row, &value)) {
              Settings_SetLong(row, !value);
              fprintf(stderr, "[diorama] %s %s\n", row->label,
                      value ? "hidden" : "shown");
              HostInput_RequestPausedRedraw();
            }
          } else {
            HostInput_HandleKeyboard((int)event.key.scancode, true,
                                     event.key.repeat != 0);
          }
          break;
        case SDL_EVENT_TEXT_INPUT:
          if (SettingsOverlay_IsOpen())
            (void)SettingsOverlay_HandleText(event.text.text);
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          /* The manual reader is modal and takes the mouse whole -- click to
           * turn, drag to pan, wheel to zoom. Checked before every camera and
           * inspector path below, all of which are gated on the overlay being
           * CLOSED and so would otherwise silently ignore the reader. */
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          /* Diorama owns right-drag (orbit) and middle-click (reset) while it
           * is on screen; §8.7 disables click-inspect in diorama for v1
           * because the flat hit-testing does not follow the tilted planes. */
          if (!SettingsOverlay_IsOpen() &&
              !RenderComparison_FreezesGameplay() &&
              Diorama_IsActiveThisFrame()) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              Diorama_SetDragging(true);
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              Diorama_ResetCamera();
          } else if (!SettingsOverlay_IsOpen() &&
                     !RenderComparison_FreezesGameplay() &&
                     Sim3DCamera_ControlsAvailable(
                         g_sim3d_textures_ready)) {
            if (event.button.button == SDL_BUTTON_RIGHT)
              Sim3DCamera_SetDragging(true);
            else if (event.button.button == SDL_BUTTON_MIDDLE)
              HostInput_ResetSim3DCamera();
          } else if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
            if (event.button.button == SDL_BUTTON_RIGHT) {
              HostInput_CloseInspectorSelection();
            } else if (event.button.button == SDL_BUTTON_LEFT) {
              /* SDL3 mouse event coordinates are floats; the hit-testing works
               * at SNES-pixel granularity, so truncating to int is exact. */
              int event_x = (int)event.button.x;
              int event_y = (int)event.button.y;
              int output_x = 0, output_y = 0;
              if (!HostDisplay_WindowPointToOutput(
                      event_x, event_y, &output_x, &output_y) ||
                  !SettingsOverlay_BeginDebugPanelDrag(
                      output_x, output_y))
                (void)HostDevTools_InspectWindowPoint(event_x, event_y);
            }
          }
          break;
        case SDL_EVENT_MOUSE_MOTION:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          if (!RenderComparison_FreezesGameplay() &&
              Diorama_IsDragging() && Diorama_IsActiveThisFrame()) {
            Diorama_AdjustCamera(event.motion.xrel * Diorama_DragRadPerPx(),
                                 event.motion.yrel * Diorama_DragRadPerPx(),
                                 0.0f);
          } else if (!RenderComparison_FreezesGameplay() &&
                     Sim3DCamera_IsDragging() &&
                     Sim3DCamera_ControlsAvailable(
                         g_sim3d_textures_ready)) {
            HostInput_AdjustSim3DCamera(
                event.motion.xrel * Diorama_DragRadPerPx(),
                event.motion.yrel * Diorama_DragRadPerPx(),
                0.0f);
          } else if (SettingsOverlay_IsDebugPanelDragging()) {
            int output_x = 0, output_y = 0;
            if (HostDisplay_WindowPointToOutput(
                    (int)event.motion.x, (int)event.motion.y,
                    &output_x, &output_y))
              SettingsOverlay_DragDebugPanel(output_x, output_y);
          }
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          /* Wheel up zooms in, i.e. decreases the camera distance. */
          if (!SettingsOverlay_IsOpen() &&
              !RenderComparison_FreezesGameplay() &&
              Diorama_IsActiveThisFrame())
            Diorama_AdjustCamera(0.0f, 0.0f,
                                 -event.wheel.y * Diorama_ZoomStep());
          else if (!SettingsOverlay_IsOpen() &&
                   !RenderComparison_FreezesGameplay() &&
                   Sim3DCamera_ControlsAvailable(
                       g_sim3d_textures_ready))
            HostInput_AdjustSim3DCamera(
                0.0f, 0.0f, -event.wheel.y * Diorama_ZoomStep());
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (ManualReader_IsOpen()) {
            (void)ManualReader_HandleMouse(&event);
            break;
          }
          if (event.button.button == SDL_BUTTON_RIGHT) {
            Diorama_SetDragging(false);
            Sim3DCamera_SetDragging(false);
          }
          if (event.button.button == SDL_BUTTON_LEFT)
            SettingsOverlay_EndDebugPanelDrag();
          break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
          /* An armed binding row consumes the raw pad event regardless of the
           * active menu device — mirrors the keyboard capture at the KEY_DOWN
           * case above, so a gamepad-bind row can be captured even when the
           * menu's active device is the keyboard (input_device=Keyboard). */
          if (SettingsOverlay_IsOpen() &&
              SettingsOverlay_HandleCaptureEvent(&event))
            break;
          /* While the menu owns the screen the pad drives menu NAVIGATION, not
           * the game — but only when the pad is a menu-active device. */
          if (SettingsOverlay_IsOpen()) {
            if (HostInput_MenuGamepadIsActive())
              (void)SettingsOverlay_HandleGamepadEvent(&event);
            break;
          }
          InputMap_HandleEvent(&event);
          break;
        case SDL_EVENT_KEY_UP:
          if (SettingsOverlay_IsOpen()) {
            if (HostInput_MenuKeyboardIsActive())
              (void)SettingsOverlay_HandleKey(event.key.key, false, false);
          } else {
            HostInput_HandleKeyboard((int)event.key.scancode, false, false);
          }
          break;
      }
    }
}

/* Keep automated runs bounded in either presentation path. This used to be
 * checked only inside the headless branch, which meant an otherwise identical
 * real-compositor capture could not exit cleanly after writing its artifact. */
static bool DevTools_ShouldAutoQuit(void) {
  static int quit_frames = kUninitializedEnvironmentOption;
  if (quit_frames == kUninitializedEnvironmentOption) {
    const char *value = getenv("AR_QUIT_FRAMES");
    quit_frames = value ? atoi(value) : -1;
  }
  extern int snes_frame_counter;
  return quit_frames > 0 && snes_frame_counter >= quit_frames;
}

/* The frame loop: pump events, then either service a host pause, step uncapped
 * (headless), or advance the M6 fixed-timestep accumulator. */
static void AppRunMainLoop(AppBoot *app) {
  /* SDL requires its 2D render API on the main thread, so all rendering runs
   * synchronously here through HostDisplay_SubmitFrame. The fixed-timestep
   * accumulator still owns emulated tick rate; vsync controls presentation. */

  bool running = true;
  uint64_t last_tick = SDL_GetTicks();  /* headless-only pacing (§3.6) */
  const uint32 emulation_frame_interval_ms =
      (uint32)(kHostDisplayEmulationFrameIntervalNs /
               kNanosecondsPerMillisecond);

  /* M6/§3.1,§3.3: fixed-timestep accumulator, non-headless only. */
  static const int kMaxCatchupFrames = 3;     /* spiral-of-death cap, §3.1 */
  uint64_t accumulator = 0;
  uint64_t last_time_ns = SDL_GetTicksNS();
  const HostDisplayPresentMode emulated_frame_present_mode =
      HostDisplay_EmulatedFramePresentMode(
          app->headless, app->headless_video);
  while (running) {
    AppLoop_PumpEvents(app, &running);

    if (RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None ||
        SessionFatal_Requested()) {
      running = false;
      continue;
    }

    HostInput_ApplyAnalogCamera();
    ActRaiser_SetAuthenticCaptureEnabled(
        HostInput_RenderComparisonCaptureRequired());
    HostInput_UpdateRenderComparison();

    /* Host-owned pauses do not issue the game's native SPC $F2 command. The
     * coordinator above freezes authentic music, replacement music, and SFX. */
    const bool host_paused =
        HostInput_IsPaused() || SettingsOverlay_IsOpen() ||
        HostInput_RenderComparisonOwnsPause();
    ApplyHostAudioPause(host_paused);
    AudioPresentationPolicy_SetAuthentic(
        RenderComparison_UsesAuthenticAudio());

    if (host_paused) {
      /* §3.4: don't accumulate wall-clock time spent paused — otherwise
       * unpausing would fire a burst of catch-up ticks. last_time_ns is
       * re-stamped every paused iteration below, so it's always "just now"
       * by the time the game actually unpauses. */
      accumulator = 0;
      /* R17/C2: a pause can last minutes, and a settings change applied during
       * it (ScheduledSettings_ApplyIfDue runs below, on the first unpaused
       * iteration, BEFORE any tick can fire) re-derives geometry. Drop the
       * retained slot so the first iteration after unpausing cannot
       * re-present a pre-pause frame at pre-change geometry. */
      HostDisplay_InvalidatePresentHistory();
      /* Advance hold-to-accelerate value stepping. Wall-clock scheduled, so
       * it is correct at whatever rate this loop runs; a value it changes
       * sets g_paused_redraw_pending, which the redraw below honors. */
      SettingsOverlay_Tick();
      /* Re-render the emulated frame only when something changed (a settings
       * edit, a resize); it is not re-rendered per iteration. */
      HostInput_RedrawPausedFrameIfNeeded();
      /* Re-present unconditionally to keep the window alive while paused. The
       * present is paced by HostDisplay_SubmitFrame's selected host cadence.
       * game_tick=false: no tick ran, so this must not capture a new image
       * endpoint or advance pair timing. */
      bool presented = false;
      if (!app->headless && !g_window_hidden) {
        const HostDisplayPresentMode present_mode =
            SettingsOverlay_IsOpen()
                ? kHostDisplayPresent_Menu
                : kHostDisplayPresent_Paused;
        presented = HostDisplay_SubmitFrame(
            present_mode, kPresentationFrameGenerationPhaseNone);
      }
      /* Pacing comes from the present itself (vsync block or the selected
       * software throttle in HostDisplay_SubmitFrame), so menu input polling
       * and repaints follow Refresh rate too. The fixed sleep remains only as
       * the anti-spin fallback when nothing presents (hidden window,
       * headless). */
      if (!presented) SDL_Delay(emulation_frame_interval_ms);
      last_time_ns = SDL_GetTicksNS();
      continue;
    }

    ScheduledSettings_ApplyIfDue();

    if (app->headless) {
      /* Headless mode is uncapped by default and advances exactly one tick per
       * outer iteration. Oracle/replay tooling depends on it running as fast as
       * the CPU allows. */
      RunOneEmulatedTick(&running);
      RunPostTickHousekeeping();
      DrawAndPresentFrame(emulated_frame_present_mode,
                          kPresentationFrameGenerationPhaseNone);

      if (DevTools_ShouldAutoQuit()) running = false;
      /* AR_PACE=1: throttle headless to ~60fps for real-time listening and
       * observation. The default turbo path advances the serialized APU target
       * with each game tick, so handshake timing remains emulated-time
       * deterministic even when those ticks run faster than wall time. */
      static int pace = kUninitializedEnvironmentOption;
      if (pace == kUninitializedEnvironmentOption)
        pace = getenv("AR_PACE") ? 1 : 0;
      if (pace) {
        uint64_t now = SDL_GetTicks();
        uint64_t elapsed = now - last_tick;
        if (elapsed < emulation_frame_interval_ms)
          SDL_Delay((Uint32)(emulation_frame_interval_ms - elapsed));
        last_tick = SDL_GetTicks();
      }
      continue;
    }

    /* M6/§3.1: fixed-timestep accumulator (non-headless only). The game
     * thread is no longer paced by a fixed millisecond delay here —
     * HostDisplay_SubmitFrame owns the present wait, and this wall-clock
     * accumulator owns the emulated tick rate independently. */
    {
      const uint64_t emulation_frame_interval_ns =
          HostDisplayPacing_SourceFrameIntervalNs(
              kHostDisplayEmulationFrameIntervalNs,
              Diorama_IsActiveThisFrame(),
              g_settings.gpu_interp_enabled,
              (InterpolationSourceRate)g_settings.gpu_interp_source_rate);
      uint64_t now_ns = SDL_GetTicksNS();
      uint64_t dt = now_ns - last_time_ns;
      last_time_ns = now_ns;
      accumulator += dt;
      /* Spiral-of-death cap (§3.1) with Limit-aware headroom. Refresh=Limit
       * sleeps on this thread, so at Limit <~30fps
       * one deliberate present interval exceeds three emulation ticks and the
       * fixed cap would discard wall time EVERY iteration, permanently
       * slowing the game (~58.3Hz at Limit=25; exactly 60.000Hz at 20 — the
       * audio-drift rate the emulation interval protects). Allow one limit
       * interval + one tick so the intentional sleep always banks; genuine
       * hitches beyond that still clamp. No other presentation mode contributes
       * headroom to this emulation-side safety bound. */
      const uint64_t catchup_cap_ns =
          HostDisplay_CatchupCapNs(
              emulation_frame_interval_ns, kMaxCatchupFrames);
      if (accumulator > catchup_cap_ns) accumulator = catchup_cap_ns;

      bool produced_frame = false;
      while (accumulator >= emulation_frame_interval_ns) {
        RunOneEmulatedTick(&running);
        accumulator -= emulation_frame_interval_ns;
        produced_frame = true;
      }
      if (DevTools_ShouldAutoQuit()) running = false;

      /* R17/C4: the sub-tick phase, taken AFTER the drain — whatever wall-clock
       * time has accrued toward the next tick but has not yet produced one.
       * This is the quantity present-time interpolation used to reconstruct
       * from its own clock divided by an EMA of the tick period; here it is
       * exact, and it cannot be corrupted by presents because presents do not
       * write the accumulator. The drain loop's own exit condition guarantees
       * the range. */
      SDL_assert(accumulator < emulation_frame_interval_ns);
      const float alpha =
          (float)accumulator /
          (float)emulation_frame_interval_ns;

      if (produced_frame) RunPostTickHousekeeping();

      /* R17/C5: the render rate is now independent of the tick rate.
       *
       * Presents used to fire ONLY when the drain produced a tick, which pinned
       * the present rate at (or below) 60.0988Hz no matter what the display or
       * the user's Refresh-rate setting said, and left every wall-clock-driven
       * present-side animation sampled at exactly the tick rate. For scroll
       * interpolation that was fatal rather than merely coarse: with one present
       * per capture, the phase was always ~0, so the feature could not do
       * anything at all.
       *
       * Between ticks the emulated state has not changed, so there is nothing
       * to re-capture. Re-compositing is still required at the selected host
       * cadence: presentation-owned camera/effect time may have advanced, and
       * the FPS counter promises completed host presents rather than emulation
       * updates. Frame interpolation is a separate optional transformation of
       * that retained tick; disabling it passes
       * kPresentationFrameGenerationPhaseNone and must never
       * collapse Vsync/Uncapped/Limit/Unlimited presentation back to ~60 Hz. */
      bool presented = false;
      if (!g_window_hidden) {
        if (produced_frame) {
          DrawAndPresentFrame(emulated_frame_present_mode, alpha);
          presented = true;
        } else if (HostDisplay_TryRepresentFrame(
                       alpha,
                       g_diorama_frame_active,
                       g_settings.gpu_interp_enabled,
                       HostInput_IsPausedRedrawPending())) {
          presented = true;
        }
      }
      /* INVARIANT: every iteration either presents (normally blocking on vsync
       * or a guaranteed-nonzero throttle; explicit Unlimited presentation is
       * the opt-in exception) or yields. One unconditional
       * line, not a property of the branch structure above — four independent
       * reviewers found this exact hole in an earlier draft where the sleep was
       * attached to the hidden-window arm, and "the code happens to fall through
       * to a sleep" is precisely the kind of structural invariant this codebase
       * has now lost five times. The no-present/no-sleep counter must stay 0. */
      HostDisplay_YieldIfNoPresent(
          presented, g_window_hidden, produced_frame);
    }
  }
}

/* Teardown, in strict reverse-dependency order: everything owning a texture, a
 * shader, or render state goes before the renderer that created it. */
static int AppShutdown(AppBoot *app, char **argv) {
  const bool fatal_session = SessionFatal_Requested();
  bool settings_flush_failed = false;
  bool save_flush_failed = false;

  /* A fatal request can arrive before the normal delayed camera-settings
   * flush. Persist the complete live registry once; comparison state is not a
   * setting and therefore remains session-only. Diagnostic replays keep their
   * existing no-write contract inside Settings_Save. */
  if (fatal_session) {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
    settings_flush_failed = !Settings_Save(settings_path);
  }

  /* Rendering is synchronous, so nothing can be mid-render during the reverse-
   * dependency teardown below. Flush only game-originated battery changes on
   * exit. Deliberate session-only editor changes re-sync the save-system shadow;
   * Restart/Exit after one must not turn it into a persistent edit. Skip the
   * flush during replay so a replayed run never mutates the active save (see
   * the auto-persist note above — it would break the next replay's alignment). */
  if (!InputReplay_ShouldProtectSaveData()) {
    SaveError error = {{0}};
    if (!SaveSystem_AutoPersistIfChanged(&error)) {
      save_flush_failed = true;
      fprintf(stderr, "[saves] shutdown flush failed: %s\n", error.message);
    }
  }
  DumpDiagState(fatal_session
                    ? "fatal"
                    : RuntimeSettings_LifecycleRequest() ==
                              kRuntimeLifecycle_Restart
                          ? "restart" : "exit");
  SimPhase0Trace_Close();
  SimRenderMetadata_TraceClose();
  ActRaiserActionBg_Shutdown();

  /* Stop the sole audio producer before reading observer-owned capture state
   * or removing subscriptions. The run directory remains live for reports. */
  HostAudio_Shutdown();
  SfxCensus_Report();
  NativeAudioTrace_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  NativeAudioTrace_Shutdown();
  HdReplacementHost_Shutdown();
  PresentRendererResources_Reset();
  DioramaFrameGeneration_Shutdown();
  Diorama_Shutdown(&g_render_device);
  DestroyDioramaTextures();
  ArRenderDevice_DestroyTexture(&g_render_device, g_sim_obj_atlas_texture);
  g_sim_obj_atlas_texture = ArRenderTexture_Invalid();
  g_sim3d_billboard_renderer_ready = false;
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    ArRenderDevice_DestroyTexture(
        &g_render_device, g_sim3d_layer_textures[plane]);
    g_sim3d_layer_textures[plane] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(&g_render_device, g_sim3d_flat_texture);
  g_sim3d_flat_texture = ArRenderTexture_Invalid();
  ManualReader_DestroyTextures();
  SettingsOverlay_Destroy();
  /* Release the game coroutine's stack mapping / fiber. Safe here: the game
   * thread is this thread and the main loop has exited, so nothing can be
   * running on that stack. */
  ActRaiser_DestroyGameCoroutine();
  InputMap_Shutdown();
  RuntimeDiagnostics_Unbind();
  SnesShutdown();
  ArRenderDevice_DestroyTexture(&g_render_device, g_hud_obj_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, g_hud_bg_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, g_authentic_texture);
  g_hud_obj_texture = ArRenderTexture_Invalid();
  g_hud_bg_texture = ArRenderTexture_Invalid();
  g_authentic_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(&g_render_device, g_texture);
  g_texture = ArRenderTexture_Invalid();
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    free(g_diorama_layer_pixels[plane]);
    g_diorama_layer_pixels[plane] = NULL;
  }
  /* Owns a full-window render target plus a GPU shader and render state, and
   * all three must go before the renderer that created them. */
  CrtPost_Shutdown(&g_render_device);
  ArSdlRenderBackend_Destroy(&g_render_device);
  SDL_DestroyWindow(g_window);
  if (fatal_session) {
    char message[1536];
    snprintf(
        message, sizeof(message),
        "%s\n\nThe game has closed to avoid continuing in a broken state.%s%s",
        SessionFatal_Message(),
        settings_flush_failed
            ? "\n\nWarning: settings.ini could not be updated."
            : "",
        save_flush_failed
            ? "\n\nWarning: the latest battery save could not be written. "
              "Check free disk space and folder permissions before restarting."
            : "");
    fprintf(stderr, "[fatal-session] shutdown complete%s%s\n",
            settings_flush_failed ? "; settings write failed" : "",
            save_flush_failed ? "; battery save write failed" : "");
    if (!app->headless &&
        !SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, "ActRaiser Recompiled closed safely",
            message, NULL)) {
      fprintf(stderr, "[fatal-session] could not show error dialog: %s\n",
              SDL_GetError());
    }
  }
  SDL_Quit();
  free(app->rom_data);

  if (RuntimeSettings_LifecycleRequest() == kRuntimeLifecycle_Restart) {
    fprintf(stderr, "[lifecycle] restarting process\n");
#ifdef _WIN32
    _execvp(argv[0], (const char *const *)argv);
#else
    execvp(argv[0], argv);
#endif
    fprintf(stderr, "[lifecycle] restart failed: %s\n", strerror(errno));
    return 1;
  }
  if (fatal_session) return 1;
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  AppBoot app = {0};
  int rc = AppBoot_ParseArgs(&app, argc, argv);
  if (rc >= 0) return rc;

  AppBoot_ResolveDisplayAndSettings(&app);
  AppBoot_ArmDiagnostics();
  rc = AppBoot_CreateVideo(&app);
  if (rc >= 0) return rc;
  AppBoot_InstallSubsystems(&app);
  AppBoot_StartGame(&app);
  AppRunMainLoop(&app);
  return AppShutdown(&app, argv);
}
