#include "snesrecomp/support/utf8_fs.h"

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
#include <SDL3/SDL_main.h> /* SDL supplies UTF-8 argv from the wide command line. */
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) sr_mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "action/action_obj_apron.h"
#include "actraiser/actraiser_action_bg.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser_game.h"
#include "actraiser/actraiser_rtl.h"
#include "audio/audio_presentation_policy.h"
#include "app/config.h"
#include "constants.h"
#include "render/crt_post.h"
#include "dev/host_dev_tools.h"
#include "dev/native_audio_trace.h"
#include "dev/oracle_trace.h"
#include "dev/scene_inspector.h"
#include "dev/sfx_census.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "present/display_geometry.h"
#include "app/forced_input.h"
#include "present/frame_slot.h"
#include "replacements/hd_replacement_host.h"
#include "host/font_resources.h"
#include "host/regional_media_files.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "actraiser/regional/actraiser_actor_art.h"
#include "host/host_audio.h"
#include "host/host_display.h"
#include "host/host_display_pacing.h"
#include "host/host_frame_surfaces.h"
#include "host/host_input.h"
#include "host/host_video.h"
#include "host/parallel_work.h"
#include "app/ini_upgrade_apply.h"
#include "app/input_map.h"
#include "app/input_replay.h"
#include "localization/language_pack.h"
#include "localization/pack_discovery.h"
#include "manual/manual_reader.h"
#include "replacements/music_replacements.h"
#include "audio/native_audio_extension.h"
#include "audio/native_audio_mixer.h"
#include "app/performance_metrics.h"
#include "platform/sdl/font_coverage_cli.h"
#include "platform/sdl/render_sdl.h"
#include "platform/sdl/settings_persistence_sdl.h"
#include "platform/sdl/text_preview_cli.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include "app/portable_paths.h"
#include "present/present.h"
#include "present/presentation_frame_generation.h"
#include "present/presentation_textures.h"
#include "render/localized_text_presenter.h"
#include "present/render_comparison.h"
#include "present/render_preparation.h"
#include "app/run_dir.h"
#include "app/runtime_diagnostics.h"
#include "app/runtime_settings.h"
#include "save/save_system.h"
#include "save/save_slot_manager.h"
#include "save/save_paths.h"
#include "randomizer/randomizer.h"
#include "host/campaign_identity.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/scheduled_settings.h"
#include "app/session_fatal.h"
#include "app/session_recovery.h"
#include "app/settings.h"
#include "settings_overlay/settings_overlay.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/voxels/sim_background_voxels.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_render_metadata.h"
#include "sim/town/sim_town_canvas.h"
#include "sim/town/sim_town_ground_art.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_map_build.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/host/launcher.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/support/file.h"
#include "app/user_data_dir.h"

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
static bool s_window_hidden;  /* true while MINIMIZED or HIDDEN: skip present */

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

static ArUiLocale RecoveryLocale(void) {
  const char *override = getenv("AR_INTERFACE_LANGUAGE");
  return override && override[0] ? ArUiCatalog_ParseLocale(override)
      : (ArUiLocale)g_settings.interface_language;
}

void NORETURN Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  // Startup failures also happen in unattended replay/font qualification. A
  // modal dialog there prevents the process from exiting or reporting failure.
  const char *headless = getenv("AR_HEADLESS");
  if (!headless || !headless[0] || headless[0] == '0') {
    char message[kSessionRecoveryCapacity];
    const ArUiLocale locale = RecoveryLocale();
    const bool formatted = SessionRecovery_Format(message, sizeof(message), locale,
        kSessionFailure_Startup, error, false, false);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
        SessionRecovery_Title(locale, kSessionFailure_Startup),
        formatted ? message : ArUiCatalog_Text(locale, "recovery.startup", error), NULL);
  }
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
  PerformanceMetrics_Add(kPerformanceCount_Ticks, 1);
  return true;
}

/* One emulated tick: sample input, run the recompiled game logic, apply
 * turbo's extra same-input sub-frames (§3.2 — unchanged mechanism, just
 * relocated so it fires once per emulated tick instead of once per outer
 * host iteration), and the AR_PERF/SNESRECOMP_APU_PROFILE instrumentation that measures
 * it (§3.5 — "wrap the per-tick RtlRunFrame"). Called once per outer
 * iteration by the headless loop (§3.6) and 0-N times per outer iteration by
 * the non-headless fixed-timestep accumulator loop (§3.1). */
static void RunOneEmulatedTickWork(bool *stop_running) {
  static int perf_on = -1;
  if (perf_on < 0) perf_on = getenv("AR_PERF") ? 1 : 0;
  uint64_t perf_t0 = perf_on ? SDL_GetTicks() : 0;
  /* SNESRECOMP_APU_PROFILE=<ms>: per-frame APU-stall attribution. Any game frame whose
   * wall time reaches the threshold (default 8 ms; the flag value overrides
   * when >= 2) prints one [apuprof] line splitting the frame into lock-wait
   * vs SPC catch-up vs handshake-spin vs upload vs music-hook time. */
  static int apuprof_ms = kUninitializedEnvironmentOption;
  if (apuprof_ms == kUninitializedEnvironmentOption) {
    /* RtlApuProfileIsEnabled caches its own answer in the runner, so it is a
     * separate module's older read of this variable -- it may agree with the
     * environment and still not be a promise about THIS getenv result. Gate
     * the parse on the pointer being parsed. */
    const char *profile = getenv("SNESRECOMP_APU_PROFILE");
    apuprof_ms = (RtlApuProfileIsEnabled() && profile && profile[0])
        ? atoi(profile) : -1;
    if (apuprof_ms >= 0 && apuprof_ms < 2) apuprof_ms = 8;
  }
  uint64_t apuprof_t0 = 0;
  unsigned long apuprof_push0 = 0;
  uint64_t apuprof_loop0 = 0;
  if (apuprof_ms > 0) {
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
    static uint64_t last_cu_calls, last_cu_cycles;
    static unsigned last_gf;
    uint64_t t1 = SDL_GetTicks();
    uint64_t dt = t1 - perf_t0;
    run_ms_sum += dt;
    if (dt > run_ms_max) run_ms_max = dt;
    win_frames++;
    if (!win_start) win_start = t1;
    if (t1 - win_start >= kPerformanceReportIntervalMs) {
      uint64_t cc, cy;
      snes_catchup_stats(&cc, &cy);
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
      last_cu_calls = cc;
      last_cu_cycles = cy;
      last_gf = gf;
      win_start = t1;
      run_ms_sum = 0;
      run_ms_max = 0;
      win_frames = 0;
    }
  }
}

static void RunOneEmulatedTick(bool *stop_running) {
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_Emulation);
  RunOneEmulatedTickWork(stop_running);
  PerformanceMetrics_End(performance);
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
/* Producer-side work is owned here, not by the pure SIM classifier or the
 * render backend. This group is independent of presentation's helpers; both
 * stages synchronously join, so their jobs cannot oversubscribe one another. */
static HostParallelWork *s_town_pixel_work;
static bool s_town_pixel_work_attempted;

static void DispatchTownPixelRows(void *context, size_t count,
    SimBackgroundRowRange range, void *work) {
  (void)context;
  if (!s_town_pixel_work_attempted) {
    s_town_pixel_work_attempted = true;
    s_town_pixel_work = HostParallelWork_Create(3);
  }
  HostParallelWork_Run(s_town_pixel_work, count, 64, range, work);
}

static void DrawAndPresentFrame(HostDisplayPresentMode present_mode,
                                float alpha) {
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
  PerformanceScope pipeline = PerformanceMetrics_Begin(kPerformance_WorldMap);
  SimWorldMap_BuildIfNeeded(
      g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  PerformanceMetrics_End(pipeline);
  /* This annotation is reused by frame submission after the host work below. */
  SimFrameData sim;
  pipeline = PerformanceMetrics_Begin(kPerformance_Metadata);
  {
    SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram,
                         RtlGameRunner());
    SimRenderMetadata_CaptureFrame(
        &sim, g_ram, g_settings.sim3d_mode,
        g_settings.sim3d_world_navigation,
        Settings_Sim3DRequestedFeatures(),
        g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
    SimRenderMetadata_CaptureSkyPalaceFrame(&sim, g_ram,
        g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
    Sim3DTuning tuning = BuildSim3DTuning();
    Sim3D_AnnotateFrame(&sim, &tuning);
    SimWorldNavigationCapture_Capture(&sim, RtlGameRunner());
    PerformanceMetrics_End(pipeline);
    pipeline = PerformanceMetrics_Begin(kPerformance_TownCanvas);
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
        have_town_ppu_view ? &town_cgram : NULL,
        DispatchTownPixelRows, NULL);
    sim.town_canvas_serial = SimTownCanvas_Serial();
    sim.background_voxel_serial = SimBackgroundVoxels_Serial();
    PerformanceMetrics_End(pipeline);
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
   *   state vs bug state frame by frame.
   * AR_SHOT_REQUIRE_COMPOSITE=1: fail the run instead of using raw PPU fallback. */
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
      /* The parse is gated by the local pointer, never by the static that
       * recorded the test: a static is shared storage, and only the pointer
       * itself is evidence that it is safe to dereference. */
      const char *value = getenv("AR_SHOT_AT_GF");
      shot_at_enabled = false;
      shot_at = 0u;
      if (value && value[0]) {
        shot_at_enabled = true;
        shot_at = (unsigned)strtoul(value, NULL, 0);
      }
      value = getenv("AR_SHOT_EVERY");
      shot_series_enabled = false;
      shot_every = 0u;
      if (value && value[0]) {
        shot_series_enabled = true;
        shot_every = (unsigned)strtoul(value, NULL, 0);
        if (!shot_every) shot_every = 1u;
      }
      value = getenv("AR_SHOT_FROM");
      shot_from = value ? (unsigned)strtoul(value, NULL, 0) : 0u;
      value = getenv("AR_SHOT_TO");
      shot_to = value
          ? (unsigned)strtoul(value, NULL, 0) : UINT_MAX;
      schedule_initialized = true;
    }
    const unsigned gf =
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    int want = 0;
    char fname[320];
    fname[0] = 0;
    if (shot_at_enabled && !shot_done && gf >= shot_at) {
      shot_done = true;
      want = 1;
      RunDirFile(fname, sizeof(fname), "shot.ppm");
    } else if (shot_series_enabled && gf >= shot_from && gf <= shot_to &&
               (gf % shot_every) == 0) {
      want = 1;
      RunDirFile(fname, sizeof(fname), "shot_%u.ppm", gf);
    }
    if (want) {
      const char *strict = getenv("AR_SHOT_REQUIRE_COMPOSITE");
      const bool require_composite = strict && strict[0] && strcmp(strict, "0");
      FILE *pf = sr_fopen(fname, "wb");
      if (pf) {
        DevToolsCaptureResult shot_size =
            HostDevTools_WriteFramebufferPpm(pf, require_composite);
        const bool closed = fclose(pf) == 0;
        if (!closed) shot_size.kind = kDevToolsCapture_Failed;
        if (shot_size.kind == kDevToolsCapture_Failed) {
          fprintf(stderr, "[shot] capture=failed path=%s\n", fname);
          if (require_composite)
            SessionFatal_Request("Required final-composite screenshot could not be captured.");
        }
        int margin_left = 0;
        int margin_right = 0;
        ActRaiser_LiveMargins(&margin_left, &margin_right);
        fprintf(stderr, "[shot] %s at gf=%u (%dx%d) margins=%d/%d mode=%s capture=%s\n",
                fname, gf, shot_size.width, shot_size.height,
                margin_left, margin_right,
                Settings_DisplayModeName(g_settings.display_mode),
                DevToolsCaptureKind_Name(shot_size.kind));
      } else {
        fprintf(stderr, "[shot] capture=failed cannot open %s\n", fname);
        if (require_composite)
          SessionFatal_Request("Required screenshot file could not be opened.");
      }
    }
  }

  if (present_mode != kHostDisplayPresent_None) {
    (void)HostDisplay_SubmitFrame(present_mode, alpha, &sim);
  }
}

/* Host-side work that follows one or more completed emulation ticks. Catch-up
 * still coalesces it to one pass, but retained-frame redraws do not run it:
 * host presentation can outpace emulation (dramatically in Unlimited), and
 * multiplying SRAM scans or host/APU policy checks by presentation throughput
 * both wastes work and contaminates the rendering measurement. */
static SettingsPersistence *s_settings_writer;
static SaveSlots s_save_slots;
static bool s_managed_slots;

static bool SlotValidateActive(void *context,SaveError *error);
static bool SlotBeforeCommit(void *context,SaveError *error) {
  SaveSlots *slots=context;
  if(slots->records[slots->active].checkpoint_required &&
      slots->records[slots->active].ever_saved && !slots->first_write_in_progress &&
      !SlotValidateActive(context,error))return false;
  return SaveSlots_BeforeCommit(context,error);
}
static void SlotDidCommit(void *context,const uint8_t *image) {
  SaveSlots_DidCommit(context,image);
}
static bool SlotValidateActive(void *context,SaveError *error) {
  SaveSlots *slots = context;
  SaveSlotDetails details;
  if(!SaveSlots_ObserveCheckpoints(slots,error))return false;
  if(SaveSlotManager_Inspect(slots,slots->active,&details))return true;
  if (error) *error = details.error;
  return false;
}
static bool SlotScan(SaveSlotCollection *out) {
  if(!out)return false;
  *out=(SaveSlotCollection){.active=s_save_slots.active,
    .writable=s_managed_slots && InputReplay_PolicyChangesAllowed() && !s_save_slots.pending};
  if(!s_managed_slots) {
    snprintf(out->error.message, sizeof(out->error.message),
             "External save: slot switching is unavailable for diagnostic paths and recordings.");
    for(unsigned i=0;i<kSaveSlotCount;++i)out->slots[i].state=kSaveSlot_Unavailable;
    return true;
  }
  if (!out->writable)
    snprintf(out->error.message, sizeof(out->error.message),
             "Save slots are read-only during recording, replay or a pending restart.");
  for (unsigned i = 0; i < kSaveSlotCount; ++i)
    (void)SaveSlotManager_Inspect(&s_save_slots, i, &out->slots[i]);
  return true;
}
static bool SlotDraft(unsigned slot,ArRegionalSession *out,SaveError *error) {
  if(!s_managed_slots || slot>=kSaveSlotCount) {
    snprintf(error->message, sizeof(error->message), "This save slot is unavailable.");
    return false;
  }
  if(s_save_slots.records[slot].prepared)
    return SaveSlotManager_ReadDraft(&s_save_slots,slot,out,error);
  uint8_t id[16];
  if(!HostCampaignIdentity_Create(NULL,id)) {
    snprintf(error->message, sizeof(error->message), "Cannot create a new campaign identity.");
    return false;
  }
  ActRaiserRegionalRulesView current;
  ArRegionalSession baseline;
  const ArRegionalCostPolicy costs={{0}};
  if(!ArRegionalSession_NewGame(&baseline,slot,id,&costs))return false;
  const ArRegionalRules *rules =
      ActRaiserRegional_CopyRulesView(&current) ? &current.requested : &baseline.requested;
  RandomizerConfig recipe=Randomizer_CurrentConfig();
  if(!SaveSlotManager_Draft(out,slot,id,rules,&recipe)) {
    snprintf(error->message, sizeof(error->message), "Cannot prepare this new-game setup.");
    return false;
  }
  return true;
}
static bool SlotDraftView(const ArRegionalSession *draft,ActRaiserRegionalRulesView *out) {
  if(!SaveSlotManager_View(draft,true,out))return false;
  ActRaiserRegionalRulesView current;
  if(ActRaiserRegional_CopyRulesView(&current)) {
    out->artwork_available = current.artwork_available;
    out->sequences_available = current.sequences_available;
    out->actor_artwork_available=current.actor_artwork_available;
  }
  return true;
}
static bool SlotSaveRegionalSettings(void *context,const ArRegionalSession *before,
    const ArRegionalSession *after,SaveError *error) {
  SaveSlots *slots=context;
  if(!InputReplay_PolicyChangesAllowed() || slots->pending || before->slot!=slots->active ||
      RuntimeSettings_LifecycleRequest()!=kRuntimeLifecycle_None) {
    snprintf(error->message, sizeof(error->message),
             "Save routing is not ready for regional settings.");
    return false;
  }
  /* Finish any already-completed native save first. Never take a new gameplay
   * snapshot just because a menu setting changed. */
  if(!SaveSystem_FlushForSwitch(error) || !SaveSystem_ValidateActive(error))return false;
  uint8_t image[kActRaiserSramSize];
  if(SaveSystem_CopyDurableImage(image)) {
    SaveFileFormat format = SaveSystem_ActiveBackend() == kSaveBackend_Ini
        ? kSaveFileFormat_Ini
        : kSaveFileFormat_NativeSrm;
    if (!ArRegionalCampaign_SaveSettings(before, after, format, SaveSystem_ActivePath(), image,
                                         error))
      return false;
    /* The companion is already durable. A failed index refresh is retryable
     * by normal validation; don't report the committed edit as rolled back. */
    SaveError index_error={{0}};
    if(!SaveSlots_ObserveCheckpoints(slots,&index_error))
      fprintf(stderr,"[regional] checkpoint index refresh pending: %s\n",index_error.message);
    return true;
  }
  ArRegionalSession draft;
  const RandomizerConfig recipe=Randomizer_CurrentConfig();
  uint8_t bytes[kSaveSlotDraftCapacity];
  size_t size;
  if(!SaveSlotManager_Draft(&draft,after->slot,after->campaign,&after->requested,&recipe) ||
      !ArRegionalSession_Encode(&draft,bytes,sizeof(bytes),&size)) {
    snprintf(error->message, sizeof(error->message), "Cannot prepare the new-game settings.");
    return false;
  }
  return SaveSlots_UpdateDraft(slots,bytes,size,error);
}
static bool SlotStart(unsigned slot, uint64_t fingerprint, const ArRegionalSession *draft,
                      SaveError *error) {
  ActRaiserRegionalRulesView current;
  if (!s_managed_slots || !InputReplay_PolicyChangesAllowed() || s_save_slots.pending ||
      RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None ||
      (ActRaiserRegional_CopyRulesView(&current) &&
       (current.population_pending || current.miracle_in_progress))) {
    snprintf(error->message, sizeof(error->message),
             "Finish the current game operation before changing saves.");
    return false;
  }
  SaveSlotDetails target;
  if(!SaveSlotManager_Inspect(&s_save_slots,slot,&target)){*error=target.error;return false;}
  if (target.fingerprint != fingerprint) {
    snprintf(error->message, sizeof(error->message),
             "The slot changed. Close and reopen Saves to review it.");
    return false;
  }
  uint8_t bytes[kSaveSlotDraftCapacity];
  size_t size = 0;
  if(draft) {
    ArRegionalSession validated;
    if (draft->slot != slot ||
        !SaveSlotManager_Draft(&validated, slot, draft->campaign, &draft->requested,
                               &draft->randomizer) ||
        !ArRegionalSession_Encode(&validated, bytes, sizeof(bytes), &size)) {
      snprintf(error->message, sizeof(error->message), "The new-game configuration is invalid.");
      return false;
    }
  }
  if(!SaveSystem_FlushForSwitch(error) || !SaveSlots_Flush(&s_save_slots,error))return false;
  char settings_path[kHostPathCapacity];
  const char *path=getenv("AR_SETTINGS_PATH");
  if(!path || !*path)path=UserDataFile(settings_path,sizeof(settings_path),"settings.ini");
  /* Global editor overrides have no destination identity. Disarm them before
   * persisting the restart; manual editor actions remain available per slot. */
  g_settings.save_edit_armed=false;
  if (!Settings_Save(path)) {
    snprintf(error->message, sizeof(error->message),
             "Could not save preferences. The current slot remains active.");
    return false;
  }
  if (!SaveSlots_Request(&s_save_slots, slot, fingerprint, draft ? bytes : NULL, size,
                         (SaveBackend)g_settings.save_backend, error))
    return false;
  RuntimeSettings_RequestPreparedRestart();
  return true;
}

static void SlotValidateBoot(void) {
  for(;;) {
    SaveError error={{0}};SaveSlotDetails details;
    bool valid=SaveSlots_ValidateDestination(&s_save_slots,&error);
    if(valid && !SaveSlotManager_Inspect(&s_save_slots,s_save_slots.destination,&details)) {
      error = details.error;
      valid = false;
    }
    if(valid)return;
    const SDL_MessageBoxButtonData buttons[]={
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,0,"Exit"},
      {0,1,"Return to previous slot"},{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,2,"Retry"}};
    char message[512];
    snprintf(message, sizeof(message),
             "Slot %u could not be opened.\n%s\n\nYour saves have been preserved.",
             s_save_slots.destination + 1, error.message);
    SDL_MessageBoxData box = {
      SDL_MESSAGEBOX_ERROR, g_window, "Save recovery", message, 3, buttons, NULL
    };
    int choice = 0;
    if(!SDL_ShowMessageBox(&box,&choice) || choice<=0)Die(message);
    if(choice==1 && !SaveSlots_ReturnToPrevious(&s_save_slots,&error))Die(error.message);
  }
}

static void RunPostTickHousekeeping(void) {
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_Housekeeping);
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
  const SettingsPersistenceReport written = SettingsPersistence_TakeReport(s_settings_writer);
  PerformanceMetrics_RecordBatch(PerformanceMetrics_Epoch(), kPerformance_SettingsWrite,
      written.elapsed_ns, written.maximum_ns, written.writes);
  if (written.failed)
    fprintf(
        stderr,
        "[settings] latest settings write failed; it will be retried by the next save or on exit\n");

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
    const PerformanceScope save_scope = PerformanceMetrics_Begin(kPerformance_SaveWrite);
    const bool saved = SaveSystem_AutoPersistIfChanged(&error);
    PerformanceMetrics_End(save_scope);
    if (!saved) {
      if (!write_error_reported)
        fprintf(stderr, "[saves] auto-persist failed: %s\n", error.message);
      write_error_reported = true;
      const uint64_t now_ms = SDL_GetTicks();
      if (!first_write_failure_ms) first_write_failure_ms = now_ms;
      if (now_ms - first_write_failure_ms >= 5000) {
        SessionFatal_RequestKind(kSessionFailure_BatterySave,
            "battery auto-persist failed for five seconds: %s; path: %s",
            error.message, SaveSystem_ActivePath());
      }
    } else {
      write_error_reported = false;
      first_write_failure_ms = 0;
    }
  }
  PerformanceMetrics_End(performance);
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
    SessionFatal_RequestKind(kSessionFailure_AudioDevice,
        "audio stream rejected by device: %s",
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
  bool localization_exit_requested;
  Snes *snes;
} AppBoot;

static bool RegionalContinuePrompt(void *context, ActRaiserRegionalContinueNotice notice) {
  const AppBoot *app = context;
  if (!app || app->headless)
    return false; /* Never silently acknowledge or wait on an invisible menu. */
  const char *body = notice == kActRaiserRegionalContinue_Estimate
      ? "overlay.region.legacy_estimate"
      : notice == kActRaiserRegionalContinue_LoadFailed ? "overlay.region.continue_failed"
                                                        : "overlay.region.adoption_failed";
  const char *accept = notice == kActRaiserRegionalContinue_Estimate ?
      "overlay.region.acknowledge" : "overlay.decision.retry";
  if (!SettingsOverlay_BeginDecision("overlay.region.continue_title", body, accept)) return false;
  SettingsOverlayDecisionResult result;
  do {
    ActRaiser_YieldToHost();
    result = SettingsOverlay_TakeDecisionResult();
  } while (result == kOverlayDecision_Pending);
  return result == kOverlayDecision_Accepted;
}

static bool RegionalPopulationPrompt(void *context,ActRaiserRegionalPopulationNotice notice,
    ArRegionalSource source,bool gameplay_profile,const uint16_t removed[6]) {
  const AppBoot *app=context;
  if(!app || app->headless)return false;
  bool opened;
  if(notice==kActRaiserRegionalPopulation_Confirm) {
    char body[2048];
    if(!SettingsOverlayRegions_PopulationConfirmation((ArUiLocale)g_settings.interface_language,
        source,removed,body,sizeof(body)))return false;
    if(gameplay_profile) {
      const size_t used=strlen(body);
      const char *scope = ArUiCatalog_Text((ArUiLocale)g_settings.interface_language,
                                           "overlay.region.menu.confirm_gameplay", NULL);
      const int written=snprintf(body+used,sizeof(body)-used,"\n\n%s",scope);
      if(written<0 || (size_t)written>=sizeof(body)-used)return false;
    }
    opened=SettingsOverlay_BeginDecisionText("overlay.region.population_label",body,
        "overlay.region.population_accept");
  } else {
    const char *key = notice == kActRaiserRegionalPopulation_Complete
        ? "overlay.region.population_complete"
        : notice == kActRaiserRegionalPopulation_NamePending
        ? "overlay.region.population_name_pending"
        : "overlay.region.population_failed";
    opened = SettingsOverlay_BeginNotice("overlay.region.population_label", key,
                                         "overlay.region.acknowledge");
  }
  if(!opened)return false;
  SettingsOverlayDecisionResult result;
  do {
    ActRaiser_YieldToHost();
    result=SettingsOverlay_TakeDecisionResult();
  } while(result==kOverlayDecision_Pending);
  return result==kOverlayDecision_Accepted;
}

/* Argument parsing, the portable-bundle chdir, the per-run artifact dir, the
 * shipped-defaults ini upgrade, the config layer, and the ROM read.
 * Returns a process exit code on failure, or -1 to continue booting. */
static int AppBoot_ParseArgs(AppBoot *app, int argc, char **argv) {
  app->rom_path = NULL;
  app->config_path = NULL;
  int rom_argument=0,config_argument=0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      app->config_path = argv[++i];
      config_argument = i;
    } else if (argv[i][0] != '-') {
      app->rom_path = argv[i];
      rom_argument = i;
    }
  }

  /* Desktop launchers resolve portable/custom/global storage once and pass
   * AR_USER_DATA_DIR. Honor it before folder-bundle anchoring; application
   * resources and writable player data may live in different directories. */
  static char rom_abs[kHostPathCapacity], config_abs[kHostPathCapacity],
      executable_abs[kHostPathCapacity];
  const char *data_root=SDL_getenv_unsafe("AR_USER_DATA_DIR");
  const bool explicit_data=data_root && *data_root;
  const bool folder_bundle=PortablePaths_IsBundle();
  if(explicit_data || folder_bundle) {
    /* Preserve launch arguments for an eventual exec after changing CWD. */
    const char *executable_name=strrchr(argv[0],'/');
#ifdef _WIN32
    const char *separator=strrchr(argv[0],'\\');
    if(separator && (!executable_name || separator>executable_name))executable_name=separator;
#endif
    if(executable_name) {
      if(!snesrecomp_abspath(argv[0],executable_abs,sizeof(executable_abs)))return 1;
      argv[0]=executable_abs;
    } else if (snesrecomp_exe_dir_path(argv[0], executable_abs, sizeof(executable_abs)) &&
               sr_path_exists(executable_abs))
      argv[0]=executable_abs;
    if(app->rom_path) {
      if(!snesrecomp_abspath(app->rom_path,rom_abs,sizeof(rom_abs)))return 1;
      app->rom_path = rom_abs;
      argv[rom_argument] = rom_abs;
    }
    if(app->config_path) {
      if(!snesrecomp_abspath(app->config_path,config_abs,sizeof(config_abs)))return 1;
      app->config_path = config_abs;
      argv[config_argument] = config_abs;
    }
    if(explicit_data) {
      SaveError error={{0}};char absolute_root[kHostPathCapacity];
      if(!snesrecomp_abspath(data_root,absolute_root,sizeof(absolute_root)) ||
          !SavePaths_EnsureDirectory(absolute_root,&error) || sr_utf8_chdir(absolute_root)) {
        fprintf(stderr, "[storage] Cannot use the selected data directory: %s\n", data_root);
        return 1;
      }
      /* Restart inherits this absolute root, including when the original
       * explicit path was relative to the caller's working directory. */
      if(SDL_setenv_unsafe("AR_USER_DATA_DIR",absolute_root,1)!=0)return 1;
    } else snesrecomp_anchor_to_exe_dir();
    if (!app->rom_path && folder_bundle &&
        snesrecomp_exe_dir_path("user-rom.sfc", rom_abs, sizeof(rom_abs)))
      app->rom_path=rom_abs;
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
  /* The launcher has resolved the runtime working directory (utils/ in a
   * bundle). Catalog scanning does not move it to the executable directory. */
  ArLanguagePackCatalog *catalog = calloc(1, sizeof(*catalog));
  SettingsLocalizationPack *choices = calloc(kSettingsLocalizationMaximumPacks, sizeof(*choices));
  if (catalog && choices &&
      ArLanguagePackCatalog_ScanDesktop(catalog, "game-assets/languages/packs")) {
    for (size_t i = 0; i < catalog->count; ++i) {
      const ArLanguagePackCatalogEntry *entry = &catalog->entries[i];
      snprintf(choices[i].id, sizeof(choices[i].id), "%s", entry->metadata.package_id);
      snprintf(choices[i].name, sizeof(choices[i].name), "%s", entry->metadata.display_name);
      snprintf(choices[i].locale, sizeof(choices[i].locale), "%s", entry->metadata.locale);
      snprintf(choices[i].manifest, sizeof(choices[i].manifest), "%s", entry->manifest);
    }
    if (!Settings_SetLocalizationPacks(choices, catalog->count))
      fprintf(stderr, "[localization] installed pack catalog has conflicting identities\n");
  }
  free(choices);
  free(catalog);
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
  { const char *e = getenv("SNESRECOMP_ENTRY_MX_CHECK");
    g_sr_entry_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* SNESRECOMP_MX_HISTORY=1: per-PC runtime m/x histogram + live misdecode anomaly trap. */
  { const char *e = getenv("SNESRECOMP_MX_HISTORY");
    g_sr_mx_history_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (g_sr_mx_history_enabled) atexit(sr_mx_history_dump); }
  /* SNESRECOMP_EXIT_MX_CHECK=1: per-function EXIT m/x check — fires when a function's runtime
   * exit (m,x) differs from what the emitter told its callers (exit-mx
   * misdecode, e.g. $03:9156). SNESRECOMP_EXIT_STACK_CHECK=1: per-function EXIT stack-balance
   * check — fires when a paired frame's RTS/RTL drifts S (e.g. $01:B8CF).
   * Symmetric twins of SNESRECOMP_ENTRY_MX_CHECK; name the culprit at its own return. */
  { const char *e = getenv("SNESRECOMP_EXIT_MX_CHECK");
    g_sr_exit_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  { const char *e = getenv("SNESRECOMP_EXIT_STACK_CHECK");
    g_sr_exit_stack_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }
  /* SNESRECOMP_CALL_MX_CHECK=1: per-CALL-SITE m/x invariant check — fires at every JSR/JSL
   * when runtime (m,x) disagrees with what the decoder statically knew at
   * that exact instruction. Catches (m,x) corruption from ANYWHERE upstream
   * of a call (not just decode-time mistakes SNESRECOMP_ENTRY_MX_CHECK/SNESRECOMP_EXIT_MX_CHECK cover),
   * narrowed to the first call site downstream of the corruption. */
  { const char *e = getenv("SNESRECOMP_CALL_MX_CHECK");
    g_sr_call_mx_check_enabled = (e && e[0] && e[0] != '0') ? 1 : 0; }

  /* SNESRECOMP_TRAP_FUNCTION=<substring>: dump the recomp call stack the first time a matching
   * function is entered (finds the dispatch chain into a misdecode variant). */
  { const char *e = getenv("SNESRECOMP_TRAP_FUNCTION");
    g_sr_trap_function = (e && e[0]) ? e : 0; }

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
    HostVideo_Create(kWindowTitle, app->headless_video);
    PresentationTextures_Create();
    HdReplacementHost_LoadTextures();
    /* Deliberately last in the video setup so focus lands on a window that is
     * fully configured, and skipped for headless_video (that window is
     * SDL_WINDOW_HIDDEN and must never steal focus from a batch run). */
    if (!app->headless_video) HostVideo_TakeFocus();
  }
  return -1;
}

static ArHostFontResources s_font_resources;
static ArHostRegionalMediaFiles s_regional_media;

static void LoadRegionalMedia(void) {
  static const char *const donors[]={"us","jp","eu-en","de","fr"};
  for(unsigned i=0;i<sizeof(donors)/sizeof(donors[0]);++i) {
    char path[128],error[192];
    snprintf(path,sizeof(path),"game-assets/regions/%s.armedia",donors[i]);
    if(!sr_path_exists(path))continue;
    if (!ArHostRegionalMediaFiles_Load(&s_regional_media, path, (ArRegionalMediaRelease)(i + 1),
                                       error, sizeof(error))) {
      fprintf(stderr, "[regional-media] %s: %s; US graphics retained\n", path, error);
      continue;
    }
    const ArRegionalMediaView *view =
        ArHostRegionalMediaFiles_View(&s_regional_media, (ArRegionalMediaRelease)(i + 1));
    if(!ActRaiserRegionalMedia_AddDonor(view)) {
      fprintf(stderr, "[regional-media] %s: donor does not match filename; US graphics retained\n",
              path);
      continue;
    }
    fprintf(stderr,"[regional-media] loaded %s (%zu reviewed resources)\n",donors[i],view->count);
  }
}

static ArFontResourceId RegisterLocalizedFont(
    void *context, const char *manifest, const char *member,
    char *error, size_t capacity) {
  (void)context;
  char path[1024];
  if (member && !strcmp(member, "builtin:actraiser-sans"))
    snprintf(path, sizeof(path),
             "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf");
  else if (!member || !strncmp(member, "builtin:", 8) ||
           !ArLanguagePack_ResolveMemberPath(manifest, member, path, sizeof(path))) {
    if (error && capacity) snprintf(error, capacity, "font member is unavailable");
    return 0;
  }
  return ArHostFontResources_RegisterFile(&s_font_resources, path, error, capacity);
}

static void RetireLocalizedFont(void *context, ArFontResourceId font) {
  (void)context;
  ArHostFontResources_Retire(&s_font_resources, font);
}

static bool PrepareLocalizedFont(void *context,
                                 const ArTextPresentationFont *font,
                                 char *error, size_t error_capacity) {
  return ArLocalizedTextPresenter_PrepareFont(context, font, error,
                                              error_capacity);
}

static void DiscardPreparedLocalizedFont(void *context) {
  ArLocalizedTextPresenter_DiscardPreparedFont(context);
}

static void ExplainLegacyLanguagePack(void *context, const char *manifest,
                                      const ArLanguagePackMetadata *metadata,
                                      bool native) {
  AppBoot *app = context;
  const ArUiLocale locale = (ArUiLocale)g_settings.interface_language;
  const char *instructions = ArUiCatalog_Text(
      locale,
      native ? "localization.upgrade.native" : "localization.upgrade.pack",
      NULL);
  char message[4096];
  const ArUiTextArgument arguments[] = {{"name", metadata->display_name},
                                        {"instructions", instructions},
                                        {"path", manifest}};
  if (!ArUiCatalog_Format(
          message, sizeof(message),
          ArUiCatalog_Text(locale, "localization.upgrade.message", NULL),
          arguments, 3))
    snprintf(message, sizeof(message), "%s\n%s\n%s", metadata->display_name,
             instructions, manifest);
  fprintf(stderr, "[localization] %s\n", message);
  if (app->headless)
    return;
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1,
       ArUiCatalog_Text(locale, "localization.upgrade.continue", NULL)},
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0,
       ArUiCatalog_Text(locale, "localization.upgrade.exit", NULL)}};
  const SDL_MessageBoxData dialog = {
      .flags = SDL_MESSAGEBOX_WARNING,
      .window = g_window,
      .title = ArUiCatalog_Text(locale, "localization.upgrade.title", NULL),
      .message = message,
      .numbuttons = 2,
      .buttons = buttons};
  int answer = -1;
  if (!SDL_ShowMessageBox(&dialog, &answer))
    fprintf(stderr, "[localization] cannot show upgrade window: %s\n",
            SDL_GetError());
  app->localization_exit_requested = answer != 1;
}

/* Overlay, world map, diorama manifest, the injected overlay hooks (layer editor,
 * manual), input, and music. Injection rather than direct calls is what keeps
 * settings_overlay.c testable with no renderer at all -- see settings_overlay.h. */
static void AppBoot_InstallSubsystems(AppBoot *app) {
  static ArTextBackend localized_text_backend;
  ArSdlTextBackend_Init(&localized_text_backend);
  ArLocalizedTextPresenter_SetBackend(&localized_text_backend);
  const ArFontResources font_resources = ArHostFontResources_Provider(&s_font_resources);
  ArLocalizedTextPresenter_SetFontResources(&font_resources);
  ArLanguagePackIo pack_io;
  ArLanguagePackFileIo_Init(&pack_io);
  const char *native_manifest = getenv("AR_LOCALIZATION_NATIVE_PACK");
  if (!native_manifest || !native_manifest[0])
    native_manifest = "game-assets/languages/native-us/pack.ini";
  const ActRaiserLocalizationPackHost pack_host = {
      .struct_size = sizeof(pack_host),
      .abi_version = ACTRAISER_LOCALIZATION_PACK_HOST_ABI_VERSION,
      .io = pack_io,
      .native_manifest = native_manifest,
      .require_v2 = true,
      .context = app,
      .legacy_pack = ExplainLegacyLanguagePack,
  };
  ActRaiserLocalizationRuntime_SetPackHost(&pack_host);
  const ArTextPresentationHost text_host = {
      .struct_size = sizeof(text_host),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .context = &g_render_device,
      .prepare_font = PrepareLocalizedFont,
      .register_font = RegisterLocalizedFont,
      .retire_font = RetireLocalizedFont,
      .discard_prepared_font = DiscardPreparedLocalizedFont,
  };
  ActRaiserLocalizationRuntime_SetPresentationHost(&text_host);
  if (!SettingsOverlay_Init(&g_render_device, g_window,
                            app->rom_data, app->rom_size))
    Die("font atlas creation for settings overlay failed");
  /* Interface text has its own font/cache lifetime, independent of whichever
   * game language pack is selected. Resources are resolved by this host. */
  char ui_font_error[kArTextRasterErrorCapacity] = {0};
  const ArFontResourceId ui_primary = ArTextBackend_IsReady(&localized_text_backend)
      ? ArHostFontResources_RegisterFile(
            &s_font_resources,
            "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf",
            ui_font_error, sizeof(ui_font_error)) : 0;
  const ArFontResourceId ui_fallbacks[] = {
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansJP-Bold.otf",
          ui_font_error, sizeof(ui_font_error)) : 0,
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansArabic-Bold.ttf",
          ui_font_error, sizeof(ui_font_error)) : 0,
      ui_primary ? ArHostFontResources_RegisterFile(
          &s_font_resources, "game-assets/fonts/noto/NotoSansHebrew-Bold.ttf",
          ui_font_error, sizeof(ui_font_error)) : 0};
  const size_t ui_fallback_count = sizeof(ui_fallbacks) / sizeof(ui_fallbacks[0]);
  bool ui_fonts_ready = ui_primary != 0;
  for (size_t i = 0; i < ui_fallback_count; ++i)
    ui_fonts_ready = ui_fonts_ready && ui_fallbacks[i] != 0;
  const ArTextBackendConfig ui_fonts = {
      .struct_size = sizeof(ui_fonts),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = "system-interface",
      .resources = font_resources, .primary_font = ui_primary,
      .fallback_fonts = ui_fallbacks, .fallback_font_count = ui_fallback_count,
      .font_revision = 2, .cached_size_capacity = 16,
  };
  if (ArTextBackend_IsReady(&localized_text_backend) &&
      ArRenderDevice_IsReady(&g_render_device) &&
      (!ui_fonts_ready ||
       !SettingsOverlay_SetTextBackend(&localized_text_backend, &ui_fonts,
                                        ui_font_error, sizeof(ui_font_error))))
    fprintf(stderr, "[settings-menu] Unicode font unavailable; keeping native interface: %s\n",
            ui_font_error);
  ArHostFontResources_Retire(&s_font_resources, ui_primary);
  for (size_t i = 0; i < ui_fallback_count; ++i)
    ArHostFontResources_Retire(&s_font_resources, ui_fallbacks[i]);
  /* The world-map image and pure development-builder tables are immutable ROM
   * data. Failure is not fatal: consumers retain the authentic presentation. */
  if (SimWorldMap_Init(app->rom_data, app->rom_size))
    SimWorldMapBuild_Init(app->rom_data, app->rom_size);
  if (!SimTownGroundArt_Init(app->rom_data, app->rom_size))
    fprintf(stderr, "[world-navigation] native town ground unavailable\n");
  if (!Diorama_InitRomBackdrops(app->rom_data, app->rom_size))
    fprintf(stderr, "[diorama] named ROM backdrops unavailable\n");
  LoadRegionalMedia();
  ActRaiserActorArt_Initialize(ActRaiserRegionalMedia_ActorArt());
  if (!ActRaiserActionBg_InitRoomScenes(app->rom_data, app->rom_size))
    fprintf(stderr, "[action-room-scene] immutable loader unavailable\n");
  /* Per-room diorama layer overrides. Absent file is the normal case and leaves
   * every room drawing as built. */
  Diorama_LoadLayerManifest();
  SettingsOverlay_SetInspectorInfoProvider(
      HostDevTools_FormatInspectorInfo);
  static const SettingsOverlayRegionalHooks kRegionalHooks = {
    .copy = ActRaiserRegional_CopyRulesView,
    .request = ActRaiserRegional_RequestProfile,
    .difficulty = ActRaiserRegional_RequestDifficultyChoice,
    .setting = ActRaiserRegional_RequestRules,
    .preview_setting = ActRaiserRegional_PreviewRules,
    .preview = ActRaiserRegional_PreviewProfile,
  };
  SettingsOverlay_SetRegionalHooks(&kRegionalHooks);
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
      FILE *f = sr_fopen(wp0, "rb");
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
    SaveError error = {{0}};
    const char *native_path = getenv("AR_SAVE_NATIVE_PATH");
    const char *ini_path = getenv("AR_SAVE_INI_PATH");
    s_managed_slots=!app->headless && !(native_path && *native_path) && !(ini_path && *ini_path) &&
        !getenv("AR_SAVE_BACKEND") && !getenv("AR_INPUT_REPLAY") && !getenv("AR_INPUT_RECORD");
    SaveBackend backend=(SaveBackend)g_settings.save_backend;
    if(s_managed_slots) {
      if(!SaveSlots_Open(&s_save_slots,saves_dir,backend,&error))Die(error.message);
      SlotValidateBoot();
      if (!SaveSlots_Paths(&s_save_slots, s_save_slots.destination, save_srm, save_ini,
                           sizeof(save_srm)))
        Die("Save slot path is too long.");
      native_path = save_srm;
      ini_path = save_ini;
      backend=SaveSlots_DestinationBackend(&s_save_slots);
    }
    if (!native_path || !native_path[0]) {
      UserDataFile(save_srm, sizeof save_srm, "saves/save.srm");
      native_path = save_srm;
    }
    if (!ini_path || !ini_path[0]) {
      UserDataFile(save_ini, sizeof save_ini, "saves/save.ini");
      ini_path = save_ini;
    }
    if (!SaveSystem_Attach(g_sram, (size_t)g_sram_size,
                           backend,
                           native_path, ini_path, &error))
      Die(error.message);
    if (!SaveSystem_SetStorageRoot(saves_dir, s_managed_slots ? (int)s_save_slots.destination : -1,
                                   &error))
      Die(error.message);
    snprintf(legacy_srm, sizeof(legacy_srm), "%s/%s.srm",
             saves_dir, RtlGameIdentifier());
    if(!s_managed_slots && !SaveSystem_MigrateLegacyNative(legacy_srm,&error))Die(error.message);
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
    if(s_managed_slots)g_settings.save_edit_armed=false;
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
  if (!ActRaiserRegional_InitializeSlot(s_managed_slots ? s_save_slots.destination : 0,
                                        HostCampaignIdentity_Create, NULL))
    Die("Regional campaign storage could not be initialized; saves preserved.");
  if(s_managed_slots) {
    SaveSlotDetails details;
    if (!SaveSlotManager_Inspect(&s_save_slots, s_save_slots.destination, &details))
      Die(details.error.message);
    if(details.state==kSaveSlot_Empty && details.prepared) {
      ArRegionalSession draft;
      SaveError error = { { 0 } };
      if (!SaveSlotManager_ReadDraft(&s_save_slots, s_save_slots.destination, &draft, &error) ||
          !ActRaiserRegional_StageNewGame(&draft))
        Die("Cannot stage the prepared new game; saves preserved.");
    }
  }
  ActRaiserRegional_SetContinuePrompt(RegionalContinuePrompt, app);
  ActRaiserRegional_SetPopulationPrompt(RegionalPopulationPrompt, app);
  if (!InputReplay_SetPolicyDigest(ActRaiserRegional_ReplayDigest, NULL))
    Die("Regional replay identity could not be initialized.");
  /* A replay must not mutate the player's configuration, for the same reason it
   * refuses to persist SRAM. Set from the same predicate so the two protections
   * cannot drift apart. */
  Settings_SetPersistenceEnabled(!InputReplay_ShouldProtectSaveData());
  if (!InputReplay_ShouldProtectSaveData()) {
    s_settings_writer = SettingsPersistence_Create();
    const SettingsSaveHost host = SettingsPersistence_Host(s_settings_writer);
    Settings_SetSaveHost(&host);
    if (!s_settings_writer)
      fprintf(stderr,
              "[settings] background writer unavailable; using durable synchronous saves\n");
  }
  ScheduledSettings_Init();

  /* Do not silently run a debug replay from power-on when its requested start
   * state cannot be restored. Runner snapshots omit the recompiled CPU and
   * suspended game continuation; even a native-font restore is unsafe. */
  { const char *ls = getenv("AR_LOADSTATE");
    if (ls && ls[0]) {
      Die("AR_LOADSTATE is unsupported: debug snapshots do not restore "
          "recompiled game execution. No snapshot was loaded. Unset "
          "AR_LOADSTATE and use a battery save with an input recording.");
    } }
  /* Bind cold-boot state before starting asynchronous audio production, whose
   * demand callback can advance APU state even before the first game tick.
   * Unsupported restores fail before any replay header is written. */
  if (!InputReplay_BeginSession(RtlGameRunner(), RtlGameIdentifier()))
    Die(InputReplay_LastError());

  if (!HostAudio_Init(Settings_AudioFrequencyHz(), g_settings.audio_samples,
                      g_settings.audio_master_volume,
                      g_settings.audio_enabled)) {
    Die("The selected audio output could not be opened. Check the system "
        "output device, then restart the game. You can also change the "
        "audio buffer or sample-rate setting before launching again.");
  }
  if(s_managed_slots) {
    SaveError error={{0}};
    if(!SaveSlots_Acknowledge(&s_save_slots,&error))Die(error.message);
    const SaveStorageHooks storage = { &s_save_slots, SlotBeforeCommit, SlotDidCommit,
                                       SlotValidateActive };
    SaveSystem_SetStorageHooks(&storage);
    ActRaiserRegional_SetSettingsWriter(SlotSaveRegionalSettings,&s_save_slots);
  }
  const SettingsOverlaySaveSlotHooks slots = { SlotScan, SlotDraft, SaveSlotManager_Edit,
                                               SlotDraftView, SlotStart };
  SettingsOverlay_SetSaveSlotHooks(&slots);
}

/* Drop resource caches before checking the rebuilt feature set. A retained
 * frame carries old texture handles, so it must be discarded after reset. */
static void AppLoop_HandleGraphicsReset(AppBoot *app, Uint32 event_type) {
  HostDisplay_ResetVsyncPacing();
  if (event_type == SDL_EVENT_RENDER_DEVICE_RESET) {
    CrtPost_Shutdown(&g_render_device);
    Diorama_ResetRendererResources(&g_render_device);
    PresentationTextures_HandleDeviceReset();
  }
  ManualReader_DestroyTextures();
  HdReplacementHost_ReloadTextures();
  if (!SettingsOverlay_ReloadTextures(app->rom_data, app->rom_size)) {
    SessionFatal_RequestKind(
        kSessionFailure_GraphicsReset,
        "overlay resources could not be restored after graphics reset: %s",
        SDL_GetError());
  }
  PresentRendererResources_Reset();
  {
    RenderFeatureMask prepared_features = 0;
    if (!RenderPreparation_Prepare(&g_render_device, &prepared_features) ||
        !Settings_RenderCapabilitiesRetained(prepared_features))
      SessionFatal_RequestKind(
          kSessionFailure_GraphicsReset,
          "graphics reset could not restore the prepared feature set; "
          "restart the game to recheck hardware support");
  }
  HostInput_RequestPausedRedraw();
  HostDisplay_InvalidatePresentHistory();
}

/* Capture wins over hotkeys; then the active menu device gets first use.
 * Suppression applies only to key-down. Key-up remains in the event pump so
 * previously accepted keys can always be released. */
static void AppLoop_HandleKeyDown(const SDL_Event *event) {
  if (SettingsOverlay_HandleCaptureEvent(event))
    return;
  if (HostInput_KeyboardIsSuppressed())
    return;
  if (SettingsOverlay_IsOpen()) {
    if (HostInput_MenuKeyboardIsActive()) {
      bool was_open = true;
      bool consumed = SettingsOverlay_HandleKey(event->key.key, true,
                                                event->key.repeat != 0);
      if (was_open && !SettingsOverlay_IsOpen())
        HostInput_ClearHeld();
      if (consumed)
        return;
    } else {
      return;
    }
  }
  if (!event->key.repeat &&
      (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_F1)) {
    HostInput_ClearHeld();
    SettingsOverlay_Open();
  } else if (event->key.key == SDLK_P) {
    if (SceneInspector_HasSelection()) {
      const bool inspector_owned_pause = HostInput_InspectorOwnsPause();
      HostInput_CloseInspectorSelection();
      if (!inspector_owned_pause)
        HostInput_TogglePause();
    } else {
      HostInput_TogglePause();
    }
  } else if (event->key.key == SDLK_T) {
    HostInput_ToggleTurbo();
  } else if (event->key.key == SDLK_F3) {
    if (!event->key.repeat) {
      const SettingDesc *inspector = Settings_Find("scene_inspector");
      SettingChangeResult result =
          Settings_SetLong(inspector, !g_settings.scene_inspector);
      char settings_path[kHostPathCapacity];
      UserDataFile(settings_path, sizeof settings_path, "settings.ini");
      if (result > kSettingChange_Unchanged && !Settings_Save(settings_path))
        fprintf(stderr, "[scene-inspector] could not save settings.ini\n");
      fprintf(stderr, "[scene-inspector] %s (%s)\n",
              g_settings.scene_inspector ? "enabled — click the game to inspect"
                                         : "disabled",
              Settings_ChangeResultName(result));
    }
  } else if (event->key.key == SDLK_MINUS || event->key.key == SDLK_KP_MINUS) {
    if (!event->key.repeat)
      HostDevTools_AdjustHudOutputScale(-25);
  } else if (event->key.key == SDLK_EQUALS || event->key.key == SDLK_PLUS ||
             event->key.key == SDLK_KP_PLUS) {
    if (!event->key.repeat)
      HostDevTools_AdjustHudOutputScale(25);
  } else if (event->key.key == SDLK_F5) {
    (void)RuntimeSettings_HandleAction(Settings_Find("save_state"));
  } else if (event->key.key == SDLK_F7) {
    (void)RuntimeSettings_HandleAction(Settings_Find("load_state"));
  } else if (event->key.key == SDLK_F9) {
    if (event->key.repeat) {
    } else if (event->key.mod & SDL_KMOD_SHIFT) {
      DumpDiagState("hotkey");
    } else if (!g_ws_active) {
      fprintf(stderr, "[display] F9 needs ExtendedAspectRatio "
                      "(e.g. 16:9) in config.ini; staying 4:3\n");
    } else {
      int m = Settings_CycleDisplayMode();
      fprintf(stderr, "[display] mode %d/%d -> %s\n", m + 1,
              kDisplayMode_PresetCount, Settings_DisplayModeName(m));
    }
  } else if (event->key.key == SDLK_F6) {
    (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
  } else if (event->key.key == SDLK_F2 || event->key.key == SDLK_C) {
    HostDevTools_TakeFullSnapshot();
  } else if (event->key.key == SDLK_D && !event->key.repeat) {
    if (event->key.mod & SDL_KMOD_SHIFT) {
      if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
        fprintf(stderr,
                "[diorama] layer dump requires an action stage "
                "($18=%02x)\n",
                g_ram[kActRaiserWram_MapGroup]);
      } else {
        g_diorama_dump_pending = true;
        fprintf(stderr, "[diorama] layer capture armed for next frame\n");
      }
    } else {
      const SettingDesc *mode = Settings_Find("diorama_mode");
      if (mode && !Settings_IsAvailable(mode)) {
        fprintf(stderr, "[diorama] requires the new renderer\n");
      } else if (mode) {
        Settings_SetLong(mode, !g_settings.diorama_mode);
        fprintf(stderr, "[diorama] %s\n",
                g_settings.diorama_mode ? "ON" : "OFF");
      }
    }
  } else if (g_settings.diorama_mode && !event->key.repeat &&
             event->key.key >= SDLK_1 && event->key.key <= SDLK_5) {
    static const char *const kLayerKeys[] = {
        "diorama_layer_backdrop", "diorama_layer_bg2", "diorama_layer_bg1",
        "diorama_layer_obj",      "diorama_layer_bg3",
    };
    int index = (int)(event->key.key - SDLK_1);
    const SettingDesc *row = Settings_Find(kLayerKeys[index]);
    long value = 0;
    if (row && Settings_GetLong(row, &value)) {
      Settings_SetLong(row, !value);
      fprintf(stderr, "[diorama] %s %s\n", row->label,
              value ? "hidden" : "shown");
      HostInput_RequestPausedRedraw();
    }
  } else {
    HostInput_HandleKeyboard((int)event->key.scancode, true,
                             event->key.repeat != 0);
  }
}

/* Manual-reader mouse input is modal. Otherwise camera controls precede
 * flat scene inspection; mouse-up releases drags independently of eligibility.
 */
static void AppLoop_HandleMouse(const SDL_Event *event) {
  switch (event->type) {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
        Diorama_IsActiveThisFrame()) {
      if (event->button.button == SDL_BUTTON_RIGHT)
        Diorama_SetDragging(true);
      else if (event->button.button == SDL_BUTTON_MIDDLE)
        Diorama_ResetCamera();
    } else if (!SettingsOverlay_IsOpen() &&
               !RenderComparison_FreezesGameplay() &&
               Sim3DCamera_ControlsAvailable(g_sim3d_textures_ready)) {
      if (event->button.button == SDL_BUTTON_RIGHT)
        Sim3DCamera_SetDragging(true);
      else if (event->button.button == SDL_BUTTON_MIDDLE)
        HostInput_ResetSim3DCamera();
    } else if (!SettingsOverlay_IsOpen() && g_settings.scene_inspector) {
      if (event->button.button == SDL_BUTTON_RIGHT) {
        HostInput_CloseInspectorSelection();
      } else if (event->button.button == SDL_BUTTON_LEFT) {
        int event_x = (int)event->button.x;
        int event_y = (int)event->button.y;
        int output_x = 0, output_y = 0;
        if (!HostDisplay_WindowPointToOutput(event_x, event_y, &output_x,
                                             &output_y) ||
            !SettingsOverlay_BeginDebugPanelDrag(output_x, output_y))
          (void)HostDevTools_InspectWindowPoint(event_x, event_y);
      }
    }
    break;
  case SDL_EVENT_MOUSE_MOTION:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!RenderComparison_FreezesGameplay() && Diorama_IsDragging() &&
        Diorama_IsActiveThisFrame()) {
      Diorama_AdjustCamera(event->motion.xrel * Diorama_DragRadPerPx(),
                           event->motion.yrel * Diorama_DragRadPerPx(), 0.0f);
    } else if (!RenderComparison_FreezesGameplay() &&
               Sim3DCamera_IsDragging() &&
               Sim3DCamera_ControlsAvailable(g_sim3d_textures_ready)) {
      HostInput_AdjustSim3DCamera(event->motion.xrel * Diorama_DragRadPerPx(),
                                  event->motion.yrel * Diorama_DragRadPerPx(),
                                  0.0f);
    } else if (SettingsOverlay_IsDebugPanelDragging()) {
      int output_x = 0, output_y = 0;
      if (HostDisplay_WindowPointToOutput(
              (int)event->motion.x, (int)event->motion.y, &output_x, &output_y))
        SettingsOverlay_DragDebugPanel(output_x, output_y);
    }
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
        Diorama_IsActiveThisFrame())
      Diorama_AdjustCamera(0.0f, 0.0f, -event->wheel.y * Diorama_ZoomStep());
    else if (!SettingsOverlay_IsOpen() && !RenderComparison_FreezesGameplay() &&
             Sim3DCamera_ControlsAvailable(g_sim3d_textures_ready))
      HostInput_AdjustSim3DCamera(0.0f, 0.0f,
                                  -event->wheel.y * Diorama_ZoomStep());
    break;
  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (ManualReader_IsOpen()) {
      (void)ManualReader_HandleMouse(event);
      break;
    }
    if (event->button.button == SDL_BUTTON_RIGHT) {
      Diorama_SetDragging(false);
      Sim3DCamera_SetDragging(false);
    }
    if (event->button.button == SDL_BUTTON_LEFT)
      SettingsOverlay_EndDebugPanelDrag();
    break;
  }
}

/* Event routing stays flat; helpers own resource reset and modal input
 * precedence. Device add/remove and key release always reach their owners. */
static void AppLoop_PumpEvents(AppBoot *app, bool *running) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
      *running = false;
      break;

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

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      HostDisplay_RecomputeLogicalPresentation();
      HostInput_RequestPausedRedraw();
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN:
      s_window_hidden = true;
      break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_SHOWN:
      s_window_hidden = false;
      HostDisplay_ResetVsyncPacing();
      HostInput_RequestPausedRedraw();
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      HostDisplay_ResetVsyncPacing();
      HostInput_RequestPausedRedraw();
      break;

    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
      AppLoop_HandleGraphicsReset(app, event.type);
      break;
    case SDL_EVENT_RENDER_DEVICE_LOST:
      SessionFatal_RequestKind(kSessionFailure_GraphicsLost,
                               "graphics device lost: %s", SDL_GetError());
      break;
    case SDL_EVENT_KEY_DOWN:
      AppLoop_HandleKeyDown(&event);
      break;
    case SDL_EVENT_TEXT_INPUT:
      if (SettingsOverlay_IsOpen())
        (void)SettingsOverlay_HandleText(event.text.text);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      AppLoop_HandleMouse(&event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
      InputMap_HandleEvent(&event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:

      if (SettingsOverlay_IsOpen() &&
          SettingsOverlay_HandleCaptureEvent(&event))
        break;

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
    static int pipeline_log = -1;
    if (pipeline_log < 0) {
      const char *value = getenv("AR_PIPELINE_PERF");
      pipeline_log = value && value[0] && value[0] != '0';
    }
    PerformanceMetrics_Configure(g_settings.performance_overlay != 0 || pipeline_log,
        g_settings.performance_overlay != 0 || pipeline_log);
    const PerformanceScope events = PerformanceMetrics_Begin(kPerformance_Events);
    AppLoop_PumpEvents(app, &running);
    PerformanceMetrics_End(events);

    if (app->localization_exit_requested ||
        RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None ||
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
      if (!app->headless && !s_window_hidden) {
        const HostDisplayPresentMode present_mode =
            SettingsOverlay_IsOpen()
                ? kHostDisplayPresent_Menu
                : kHostDisplayPresent_Paused;
        presented = HostDisplay_SubmitFrame(
            present_mode, kPresentationFrameGenerationPhaseNone, NULL);
      }
      /* Pacing comes from the present itself (vsync block or the selected
       * software throttle in HostDisplay_SubmitFrame), so menu input polling
       * and repaints follow Refresh rate too. The fixed sleep remains only as
       * the anti-spin fallback when nothing presents (hidden window,
       * headless). */
      if (!presented) {
        const PerformanceScope pacing = PerformanceMetrics_Begin(kPerformance_Pacing);
        SDL_Delay(emulation_frame_interval_ms);
        PerformanceMetrics_End(pacing);
      }
      last_time_ns = SDL_GetTicksNS();
      continue;
    }

    ScheduledSettings_ApplyIfDue();

    if (app->headless) {
      /* Headless mode is uncapped by default and advances exactly one tick per
       * outer iteration. Oracle/replay tooling depends on it running as fast as
       * the CPU allows. */
      bool stop_requested = false;
      RunOneEmulatedTick(&stop_requested);
      if (stop_requested) running = false;
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
        if (elapsed < emulation_frame_interval_ms) {
          const PerformanceScope pacing = PerformanceMetrics_Begin(kPerformance_Pacing);
          SDL_Delay((Uint32)(emulation_frame_interval_ms - elapsed));
          PerformanceMetrics_End(pacing);
        }
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
      while (running && accumulator >= emulation_frame_interval_ns) {
        bool stop_requested = false;
        RunOneEmulatedTick(&stop_requested);
        if (stop_requested) running = false;
        accumulator -= emulation_frame_interval_ns;
        produced_frame = true;
      }
      if (DevTools_ShouldAutoQuit()) running = false;
      // A replay/fatal stop can leave undrained catch-up ticks. They must not
      // execute after its final transaction or become an invalid alpha.
      if (!running) accumulator = 0;

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
      if (!s_window_hidden) {
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
          presented, s_window_hidden, produced_frame);
    }
  }
}

/* Teardown, in strict reverse-dependency order: everything owning a texture, a
 * shader, or render state goes before the renderer that created it. */
static int AppShutdown(AppBoot *app, char **argv) {
  const bool fatal_session = SessionFatal_Requested();
  bool settings_flush_failed = false;
  bool save_flush_failed = false;

  /* Drain accepted settings snapshots before any fatal/failure-recovery save.
   * A normal exit must not newly persist unrelated session-only overrides. */
  const bool settings_save_ok = SettingsPersistence_Destroy(s_settings_writer);
  s_settings_writer = NULL;
  Settings_SetSaveHost(NULL);
  if (!settings_save_ok)
    fprintf(stderr, "[settings] retrying failed settings save during shutdown\n");
  if ((fatal_session || !settings_save_ok) && !InputReplay_ShouldProtectSaveData()) {
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof(settings_path), "settings.ini");
    settings_flush_failed = !Settings_Save(settings_path);
    if (settings_flush_failed)
      fprintf(stderr,
              "[settings] shutdown write failed; recent preferences may not have been saved\n");
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
  HostParallelWork_Destroy(s_town_pixel_work);
  s_town_pixel_work = NULL;
  s_town_pixel_work_attempted = false;
  ActRaiserActionBg_Shutdown();
  ActRaiserActorArt_Shutdown();
  ActRaiserRegionalMedia_ClearDonors();
  ArHostRegionalMediaFiles_Destroy(&s_regional_media);
  ActRaiserLocalizationRuntime_Shutdown();
  ActRaiserLocalizationRuntime_SetPresentationHost(NULL);
  ActRaiserLocalizationRuntime_SetPackHost(NULL);

  /* Stop the sole audio producer before reading observer-owned capture state
   * or removing subscriptions. The run directory remains live for reports. */
  HostAudio_Shutdown();
  MusicReplacements_Shutdown();
  SfxCensus_Report();
  NativeAudioTrace_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  NativeAudioTrace_Shutdown();
  HdReplacementHost_Shutdown();
  PresentRendererResources_Reset();
  SimTownGroundArt_Shutdown();
  DioramaFrameGeneration_Shutdown();
  Diorama_Shutdown(&g_render_device);
  ManualReader_DestroyTextures();
  SettingsOverlay_Destroy();
  ArLocalizedTextPresenter_SetFontResources(NULL);
  if (!ArHostFontResources_Destroy(&s_font_resources))
    fprintf(stderr, "[localized-text] font resources still leased at shutdown\n");
  /* Release the game coroutine's stack mapping / fiber. Safe here: the game
   * thread is this thread and the main loop has exited, so nothing can be
   * running on that stack. */
  ActRaiser_DestroyGameCoroutine();
  InputMap_Shutdown();
  RuntimeDiagnostics_Unbind();
  SnesShutdown();
  PresentationTextures_Destroy();
  HostFrameSurfaces_ReleaseDioramaPlanes();
  /* Owns a full-window render target plus a GPU shader and render state, and
   * all three must go before the renderer that created them. */
  CrtPost_Shutdown(&g_render_device);
  HostVideo_Destroy();
  if (fatal_session) {
    char message[kSessionRecoveryCapacity];
    const ArUiLocale locale = RecoveryLocale();
    const bool formatted = SessionRecovery_Format(message, sizeof(message), locale,
        SessionFatal_Kind(), SessionFatal_Message(), settings_flush_failed, save_flush_failed);
    fprintf(stderr, "[fatal-session] shutdown complete%s%s\n",
            settings_flush_failed ? "; settings write failed" : "",
            save_flush_failed ? "; battery save write failed" : "");
    if (!app->headless &&
        !SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, SessionRecovery_Title(locale, SessionFatal_Kind()),
            formatted ? message : ArUiCatalog_Text(locale, "recovery.generic", NULL), NULL)) {
      fprintf(stderr, "[fatal-session] could not show error dialog: %s\n",
              SDL_GetError());
    }
  }
  SDL_Quit();
  free(app->rom_data);
  if(s_managed_slots) {
    SaveError error={{0}};
    if(!SaveSlots_Flush(&s_save_slots,&error))save_flush_failed=true;
    SaveSlots_Close(&s_save_slots);
  }

  if (RuntimeSettings_LifecycleRequest() == kRuntimeLifecycle_Restart) {
    if(save_flush_failed || settings_flush_failed || fatal_session) {
      fprintf(
          stderr,
          "[lifecycle] restart stopped after a persistence failure; the request is retained for recovery\n");
      return 1;
    }
    fprintf(stderr, "[lifecycle] restarting process\n");
#ifdef _WIN32
    sr_execvp(argv[0], (const char *const *)argv);
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
  if (argc > 1 && !strcmp(argv[1], "--text-preview-v1"))
    return ArSdlTextPreview_Run(argc - 1, argv + 1);
  if (argc > 1 && !strcmp(argv[1], "--font-coverage-v1"))
    return ArSdlFontCoverage_Run(argc - 1, argv + 1);
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
  ActRaiserLocalizationRuntime_ApplySettings();
  if (!app.localization_exit_requested)
    AppRunMainLoop(&app);
  return AppShutdown(&app, argv);
}
