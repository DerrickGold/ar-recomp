#include "app/game_loop.h"

#include <stdlib.h>
#include <SDL3/SDL.h>

#include "app/input_replay.h"
#include "app/performance_metrics.h"
#include "app/runtime_settings.h"
#include "app/scheduled_settings.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "app/settings_session.h"
#include "audio/audio_presentation_policy.h"
#include "audio/audio_session.h"
#include "constants.h"
#include "dev/dev_automation.h"
#include "dev/host_dev_tools.h"
#include "dev/host_runtime_diagnostics.h"
#include "dev/oracle_trace.h"
#include "diorama/diorama.h"
#include "diorama/diorama_performance.h"
#include "host/host_display.h"
#include "host/host_display_pacing.h"
#include "host/host_input.h"
#include "host/host_localization.h"
#include "host/host_ppu_output.h"
#include "host/host_video.h"
#include "manual/manual_reader.h"
#include "present/present.h"
#include "present/presentation_frame_generation.h"
#include "present/presentation_textures.h"
#include "present/render_comparison.h"
#include "present/render_preparation.h"
#include "render/crt_post.h"
#include "replacements/hd_replacement_host.h"
#include "save/save_slot_host.h"
#include "settings_overlay/settings_overlay.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_frame_capture.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/game/types.h"

/* The window survives game resets; its visibility does too. */
static bool s_window_hidden;
enum { kUninitializedEnvironmentOption = -2 };

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
  const HostRuntimeTickProfile profile = HostRuntimeDiagnostics_BeginTick();

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
  HostRuntimeDiagnostics_EndTick(profile);
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
static void DrawAndPresentFrame(HostDisplayPresentMode present_mode,
                                float alpha) {
  const uint64_t profile = HostRuntimeDiagnostics_BeginDraw();
  RtlDrawPpuFrame();
  DioramaPerformanceScope host_post_performance = {0};
  if (Diorama_IsActiveThisFrame())
    host_post_performance =
        DioramaPerformance_Begin(kDioramaPerformance_HostPost);
  /* This annotation is reused by frame submission after the host work below. */
  SimFrameData sim;
  SimFrameCapture_Produce(&sim);
  DevAutomation_ArmScheduledDioramaDump();
  HostDevTools_ServiceDioramaDump();
  DioramaPerformance_End(host_post_performance);
  HostInput_MarkFrameDrawn();
  HostRuntimeDiagnostics_EndDraw(profile);

  DevAutomation_CaptureScheduledScreenshot();

  if (present_mode != kHostDisplayPresent_None) {
    (void)HostDisplay_SubmitFrame(present_mode, alpha, &sim);
  }
}

/* Host-side work that follows one or more completed emulation ticks. Catch-up
 * still coalesces it to one pass, but retained-frame redraws do not run it:
 * host presentation can outpace emulation (dramatically in Unlimited), and
 * multiplying SRAM scans or host/APU policy checks by presentation throughput
 * both wastes work and contaminates the rendering measurement. */
static void RunPostTickHousekeeping(void) {
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_Housekeeping);
  AudioSession_AfterTicks();

  DevAutomation_AfterTicks();

  Diorama_FlushSettingsIfDirty();
  Sim3DCamera_FlushSettingsIfDirty();
  SettingsSession_PollWrites();

  SaveSlotHost_AfterTicks();
  PerformanceMetrics_End(performance);
}



/* Drop resource caches before checking the rebuilt feature set. A retained
 * frame carries old texture handles, so it must be discarded after reset. */
static void AppLoop_HandleGraphicsReset(const GameSessionConfig *config, Uint32 event_type) {
  HostDisplay_ResetVsyncPacing();
  if (event_type == SDL_EVENT_RENDER_DEVICE_RESET) {
    CrtPost_Shutdown(&g_render_device);
    Diorama_ResetRendererResources(&g_render_device);
    PresentationTextures_HandleDeviceReset();
  }
  ManualReader_DestroyTextures();
  HdReplacementHost_ReloadTextures();
  if (!SettingsOverlay_ReloadTextures(config->rom_data, config->rom_size)) {
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

/* Event routing stays flat; helpers own resource reset and modal input
 * precedence. Device add/remove and key release always reach their owners. */
static void AppLoop_PumpEvents(const GameSessionConfig *config, bool *running) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (HostInput_HandleEvent(&event))
      continue;
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
      if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && g_window &&
          event.window.windowID == SDL_GetWindowID(g_window))
        HostInput_LogStatus("focus-gained");
      HostDisplay_ResetVsyncPacing();
      HostInput_RequestPausedRedraw();
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (g_window && event.window.windowID == SDL_GetWindowID(g_window))
        HostInput_LogStatus("focus-lost");
      break;

    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
      AppLoop_HandleGraphicsReset(config, event.type);
      break;
    case SDL_EVENT_RENDER_DEVICE_LOST:
      SessionFatal_RequestKind(kSessionFailure_GraphicsLost,
                               "graphics device lost: %s", SDL_GetError());
      break;

    }
  }
}

/* The frame loop: pump events, then either service a host pause, step uncapped
 * (headless), or advance the M6 fixed-timestep accumulator. */
void GameLoop_Run(const GameSessionConfig *config) {
  /* SDL requires its 2D render API on the main thread, so all rendering runs
   * synchronously here through HostDisplay_SubmitFrame. The fixed-timestep
   * accumulator still owns emulated tick rate; vsync controls presentation. */

  bool running = true;
  bool logged_input_ready = false;
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
          config->headless, config->headless_video);
  while (running) {
    const bool pipeline_log = HostRuntimeDiagnostics_PipelineLoggingEnabled();
    PerformanceMetrics_Configure(g_settings.performance_overlay != 0 || pipeline_log,
        g_settings.performance_overlay != 0 || pipeline_log);
    const PerformanceScope events = PerformanceMetrics_Begin(kPerformance_Events);
    AppLoop_PumpEvents(config, &running);
    PerformanceMetrics_End(events);
    if (!logged_input_ready) {
      logged_input_ready = true;
      HostInput_LogStatus("event-loop-ready");
    }

    if (HostLocalization_ExitRequested() ||
        RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None ||
        SessionFatal_Requested()) {
      running = false;
      continue;
    }

    HostInput_ApplyAnalogCamera();
    HostPpuOutput_SetAuthenticEnabled(
        HostInput_RenderComparisonCaptureRequired());
    HostInput_UpdateRenderComparison();

    /* Host-owned pauses do not issue the game's native SPC $F2 command. The
     * coordinator above freezes authentic music, replacement music, and SFX. */
    const bool host_paused =
        HostInput_IsPaused() || SettingsOverlay_IsOpen() ||
        HostInput_RenderComparisonOwnsPause();
    AudioSession_SetPaused(host_paused);
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
      if (!config->headless && !s_window_hidden) {
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
    if (RuntimeSettings_LifecycleRequest() != kRuntimeLifecycle_None)
      continue; /* An import/reset must not execute another old-session tick. */

    if (config->headless) {
      /* Headless mode is uncapped by default and advances exactly one tick per
       * outer iteration. Oracle/replay tooling depends on it running as fast as
       * the CPU allows. */
      bool stop_requested = false;
      RunOneEmulatedTick(&stop_requested);
      if (stop_requested) running = false;
      RunPostTickHousekeeping();
      DrawAndPresentFrame(emulated_frame_present_mode,
                          kPresentationFrameGenerationPhaseNone);

      if (DevAutomation_ShouldQuit()) running = false;
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
      if (DevAutomation_ShouldQuit()) running = false;
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
