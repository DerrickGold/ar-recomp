#include "app/game_loop.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <SDL3/SDL.h>

#include "actraiser/actraiser_rtl.h"
#include "actraiser_game.h"
#include "app/input_replay.h"
#include "app/input_map.h"
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
#include "host/frame_producer.h"
#include "host/frame_queue.h"
#include "host/frame_playout.h"
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
#include "sim/sim_world_map.h"
#include "actraiser/actraiser_sim_menu.h"
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
static void RunOneEmulatedTickWork(uint32 live_inputs, int multiplier,
                                   bool *stop_running) {
  const HostRuntimeTickProfile profile = HostRuntimeDiagnostics_BeginTick();

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
  if (multiplier > 1 && !*stop_running) {
    for (int tf = 1; tf < multiplier && !SessionFatal_Requested() &&
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

typedef struct TickJob {
  uint32 inputs;
  int multiplier;
  bool stop_requested;
} TickJob;

static void RunTickOnOwner(void *context) {
  TickJob *job = context;
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_Emulation);
  RunOneEmulatedTickWork(job->inputs, job->multiplier, &job->stop_requested);
  PerformanceMetrics_End(performance);
}

static TickJob LatchTickJob(void) {
  return (TickJob){
    .inputs = HostInput_SampleLiveInputs(),
    .multiplier = HostInput_IsTurbo() ? g_settings.turbo_multiplier : 1,
  };
}

static void RunOneEmulatedTick(bool *stop_running) {
  TickJob job = LatchTickJob();
  if (HostFrameProducer_Enabled()) {
    /* Even boot, paused transitions and synchronous scenes use this owner. Never
     * resume a Windows fiber or POSIX ucontext on a different thread. */
    if (!HostFrameProducer_Submit(RunTickOnOwner, &job)) {
      SessionFatal_Request("Frame producer rejected a synchronous tick.");
      *stop_running = true;
      return;
    }
    HostFrameProducer_Wait();
  } else {
    RunTickOnOwner(&job);
  }
  *stop_running = job.stop_requested;
}

static void DestroyProducerCoroutine(void *unused) {
  (void)unused;
  ActRaiser_DestroyGameCoroutine();
}

typedef struct StreamJob {
  HostFrameQueue *queue;
  atomic_uint_fast64_t input_sample; /* low 32 input bits, high 32 sample ms */
  uint64_t next_ns, interval_ns, service_deadline_ns;
  HostFramePlayout playout;
  uint8_t map_group, map_number;
  uint32_t underlay_serial;
  bool sim_town;
  unsigned frames, full_waits;
  bool stopped, transition, maintenance, measure_cpu;
} StreamJob;

static void ProduceFrameStream(void *context) {
  StreamJob *stream = context;
  stream->frames = stream->full_waits = 0;
  stream->maintenance = false;
  while (!HostFrameQueue_PauseRequested(stream->queue)) {
    const uint64_t now = SDL_GetTicksNS();
    if (now < stream->next_ns) {
      const uint64_t left = stream->next_ns - now;
      SDL_DelayNS(left < 1000000 ? left : 1000000);
      continue;
    }
    HostFramePacket *packet = HostFrameQueue_BeginWrite(stream->queue);
    if (!packet) { ++stream->full_waits; SDL_Delay(1); continue; }
    packet->started_ns = SDL_GetTicksNS();
    const uint64_t cpu_start = stream->measure_cpu ? HostFrameProducer_ThreadCpuTimeNs() : 0;
    packet->source_ns = stream->next_ns;
    const uint64_t sample = atomic_load_explicit(&stream->input_sample, memory_order_acquire);
    const uint32_t age_ms = (uint32_t)SDL_GetTicks() - (uint32_t)(sample >> 32);
    packet->input_ns = packet->started_ns - (uint64_t)age_ms * 1000000;
    TickJob tick = {.inputs = HostInput_ResolveActionInputs((uint32_t)sample),
        .multiplier = 1};
    RunTickOnOwner(&tick);
    AudioSession_AfterTicks();
    stream->stopped = tick.stop_requested || DevAutomation_ShouldQuit();
    if (SessionFatal_Requested()) { stream->stopped = true; break; }
    /* Every room change crosses the host boundary before the next capture.
     * New resources, settings, unsupported views and Mode 7 stay synchronous
     * until a supported, fully uploaded endpoint establishes ownership. */
    if (g_ram[kActRaiserWram_MapGroup] != stream->map_group ||
        g_ram[kActRaiserWram_CurrentMap] != stream->map_number ||
        (!stream->sim_town && !Diorama_IsActiveThisFrame()) ||
        (stream->sim_town && (ActRaiserSimMenu_OwnsInput() ||
                             ActRaiserSimMenu_OwnsPresentation()))) {
      stream->transition = true;
      break;
    }
    const uint64_t profile = HostRuntimeDiagnostics_BeginDraw();
    SrPpuBgPacket *background_target = stream->sim_town ? NULL :
        HostFramePacket_BackgroundTarget(packet);
    SimFrameInputs *sim_inputs = stream->sim_town ? HostFramePacket_SimTarget(packet) : NULL;
    if (stream->sim_town ? !sim_inputs : !background_target) {
      HostRuntimeDiagnostics_EndDraw(profile);
      SessionFatal_Request("Frame stream could not allocate source packet storage.");
      stream->stopped = true;
      break;
    }
    ActRaiser_SetBackgroundPacketTarget(background_target);
    RtlDrawPpuFrame();
    SimFrameData sim;
    if (sim_inputs) SimFrameCapture_ProduceOwned(&sim, sim_inputs, stream->underlay_serial);
    else SimFrameCapture_Produce(&sim);
    const PerformanceScope capture = PerformanceMetrics_Begin(kPerformance_Capture);
    FrameSlot_Capture(&packet->frame, &sim);
    packet->frame.timestamp_ns = packet->source_ns;
    packet->frame.sim_inputs = sim_inputs;
    PerformanceMetrics_End(capture);
    if (!HostFramePacket_Supports(&packet->frame)) {
      HostRuntimeDiagnostics_EndDraw(profile);
      stream->transition = true;
      break;
    }
    const uint64_t copy_start = SDL_GetTicksNS();
    if (!HostFramePacket_OwnPixels(packet)) {
      HostRuntimeDiagnostics_EndDraw(profile);
      SessionFatal_Request("Frame stream could not own the captured surfaces.");
      stream->stopped = true;
      break;
    }
    packet->copy_ns = SDL_GetTicksNS() - copy_start;
    packet->tick = snes_frame_counter;
    packet->completed_ns = SDL_GetTicksNS();
    const uint64_t cpu_end = cpu_start ? HostFrameProducer_ThreadCpuTimeNs() : 0;
    packet->cpu_ns = cpu_end >= cpu_start ? cpu_end - cpu_start : 0;
    HostRuntimeDiagnostics_EndDraw(profile);
    HostFrameQueue_Publish(stream->queue);
    ++stream->frames;
    stream->next_ns += stream->interval_ns;
    if (stream->stopped) break;
    if (ScheduledSettings_IsDue() || DevAutomation_RequiresHostService()) break;
    /* Offer routine maintenance just after publishing a source, when the
     * owner has a full inter-tick idle window. A main-thread timer could stop
     * it just before the next tick while main was about to wait for present. */
    if (stream->service_deadline_ns &&
        SDL_GetTicksNS() >= stream->service_deadline_ns) {
      stream->maintenance = true;
      break;
    }
  }
  ActRaiser_SetBackgroundPacketTarget(NULL);
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
static void FinishFrameCapture(uint64_t profile, SimFrameData *sim) {
  DioramaPerformanceScope host_post_performance = {0};
  if (Diorama_IsActiveThisFrame())
    host_post_performance =
        DioramaPerformance_Begin(kDioramaPerformance_HostPost);
  /* This annotation is reused by frame submission after the host work below. */
  SimFrameCapture_Produce(sim);
  DevAutomation_ArmScheduledDioramaDump();
  HostDevTools_ServiceDioramaDump();
  DioramaPerformance_End(host_post_performance);
  HostInput_MarkFrameDrawn();
  HostRuntimeDiagnostics_EndDraw(profile);
}

static bool DrawAndPresentFrame(HostDisplayPresentMode present_mode,
                                float alpha) {
  const uint64_t profile = HostRuntimeDiagnostics_BeginDraw();
  RtlDrawPpuFrame();
  SimFrameData sim;
  FinishFrameCapture(profile, &sim);

  const bool presented = present_mode != kHostDisplayPresent_None &&
      HostDisplay_SubmitFrame(present_mode, alpha, &sim);
  if (!presented) DevAutomation_CaptureScheduledScreenshot(NULL);
  return presented;
}

/* Host-side work that follows one or more completed emulation ticks. Catch-up
 * still coalesces it to one pass, but retained-frame redraws do not run it:
 * host presentation can outpace emulation (dramatically in Unlimited), and
 * multiplying SRAM scans or host/APU policy checks by presentation throughput
 * both wastes work and contaminates the rendering measurement. */
/* Requires an idle runner owner, but preserves captured scene/resources. This
 * may run with independently owned future packets still in the display queue.
 * Keep warps, captures and settings application at the full drained boundary. */
static void RunPersistenceHousekeeping(void) {
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_Housekeeping);
  Diorama_FlushSettingsIfDirty();
  Sim3DCamera_FlushSettingsIfDirty();
  SettingsSession_PollWrites();

  SaveSlotHost_AfterTicks();
  PerformanceMetrics_End(performance);
}

static void RunPostTickHousekeeping(void) {
  AudioSession_AfterTicks();
  DevAutomation_AfterTicks();
  RunPersistenceHousekeeping();
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
/* Supplying a NULL buffer lets libc choose its own size. Deck's glibc uses
 * 4 KiB even when asked for 4 MiB, putting disk flushes back in the measured
 * loop. Own the diagnostic storage through fclose; ordinary playback allocates
 * nothing here. Longer traces still flush normally when their buffer fills. */
static FILE *OpenBufferedDiagnosticTrace(const char *path, size_t size,
                                        char **storage) {
  *storage = NULL;
  if (!path) return NULL;
  FILE *file = fopen(path, "w");
  if (!file) return NULL;
  char *buffer = malloc(size);
  if (!buffer || setvbuf(file, buffer, _IOFBF, size) != 0) {
    fclose(file);
    free(buffer);
    fprintf(stderr, "[frame-trace] could not allocate buffering for %s\n", path);
    return NULL;
  }
  *storage = buffer;
  return file;
}

void GameLoop_Run(const GameSessionConfig *config) {
  /* SDL presentation/upload stay on main. The action producer publishes owned
   * packets on the source clock and lends no live state to presentation.
   * Unsupported scenes retain the synchronous fixed-timestep accumulator. */

  bool running = true;
  unsigned delay_permille = kHostFramePlayoutDefaultDelayPermille;
  /* A zero override retains the old newest-completed policy for A/B traces. */
  int native_delay_us = -1;
  const char *native_delay_option = getenv("AR_FRAME_STREAM_NATIVE_DELAY_US");
  if (native_delay_option) {
    char *end;
    const long value = strtol(native_delay_option, &end, 10);
    if (end != native_delay_option && !*end && value >= 0 && value <= 33000)
      native_delay_us = (int)value;
  }
  const char *delay_option = getenv("AR_FRAME_STREAM_DELAY_PERMILLE");
  if (delay_option) {
    char *end;
    const unsigned long value = strtoul(delay_option, &end, 10);
    if (end != delay_option && !*end && value >= 1000 && value <= 3000)
      delay_permille = (unsigned)value;
  }
  const char *stream_option = getenv("AR_FRAME_STREAM");
  /* Enabled for ordinary play; AR_FRAME_STREAM=0 keeps a diagnostic fallback.
   * Headless replay/oracle runs retain their deterministic synchronous path. */
  const bool stream_enabled = !config->headless &&
      (!stream_option || strcmp(stream_option, "0") != 0);
  if (stream_enabled)
    fprintf(stderr, "[frame-producer] bounded frame pipeline %s\n",
        HostFrameProducer_Init(DestroyProducerCoroutine, NULL)
            ? "enabled" : "unavailable; synchronous fallback");
  const char *sim_stream_option = getenv("AR_SIM_FRAME_STREAM");
  const char *world_oracle_option = getenv("AR_WORLDMAP_HLE_COMPARE");
  const char *sim_metadata_trace = getenv("AR_SIM3D_D1_TRACE");
  /* Town streaming keeps the same native source clock. Diagnostics needing
   * live CPU transactions or fully prepared capture metadata stay synchronous. */
  const bool sim_stream_enabled = (!sim_stream_option || strcmp(sim_stream_option, "0")) &&
      (!world_oracle_option || !world_oracle_option[0] || !strcmp(world_oracle_option, "0")) &&
      (!sim_metadata_trace || !sim_metadata_trace[0]);
  StreamJob stream = {0};
  atomic_init(&stream.input_sample, 0);
  bool stream_pending = false;
  bool stream_done = false;
  /* Diagnostic isolation of periodic ownership handoffs. Host events and
   * scheduled service still pause production even when this timer is zero. */
  uint64_t service_interval_ns = 1000000000;
  const char *service_option = getenv("AR_FRAME_STREAM_SERVICE_MS");
  if (service_option) {
    char *end;
    const unsigned long ms = strtoul(service_option, &end, 10);
    if (end != service_option && !*end && ms <= 10000)
      service_interval_ns = (uint64_t)ms * 1000000;
  }
  uint64_t stream_owner_service_ns = 0;
  uint64_t stream_endpoint_ns = 0, stream_report_ns = 0;
  uint64_t stream_epoch = 0;
  uint64_t stream_frames = 0, stream_last_source_ns = 0;
  double stream_work_ms = 0, stream_copy_ms = 0, stream_bytes = 0, stream_age_ms = 0;
  unsigned stream_event = 0;
  int stream_tick = 0, cadence_tick = 0;
  uint64_t cadence_epoch = 0;
  const char *trace_path = getenv("AR_FRAME_STREAM_TRACE");
  char *stream_trace_buffer = NULL;
  FILE *stream_trace = OpenBufferedDiagnosticTrace(
      stream_enabled ? trace_path : NULL, 1024 * 1024, &stream_trace_buffer);
  if (stream_trace) {
    fprintf(stream_trace,
        "start_ns,complete_ns,endpoint_ns,alpha,target_ns,tick,interval_ns,interpolation,epoch\n");
  }
  const char *pacing_trace_path = getenv("AR_FRAME_PACING_TRACE");
  char *pacing_trace_buffer = NULL;
  FILE *pacing_trace = OpenBufferedDiagnosticTrace(
      stream_enabled ? pacing_trace_path : NULL, 4 * 1024 * 1024,
      &pacing_trace_buffer);
  stream.measure_cpu = pacing_trace != NULL;
  HostDisplay_EnablePresentTrace(pacing_trace != NULL);
  if (pacing_trace) {
    fprintf(pacing_trace,
            "loop_ns,prepare_ns,ready_ns,draw_ns,complete_ns,deadline_ns,upload_ns,uploads,draw_"
            "work_ns,swap_ns,vector_wait_ns,queue_before,queue_after,presented,tick,epoch,source_"
            "ns,producer_start_ns,producer_complete_ns,input_ns,pump_ns,input_events_ns,owner_poll_"
            "ns,submit_start_ns,backend_flush_ns,backend_acquire_ns,backend_submit_ns,producer_cpu_"
            "ns,playout_target_ns,sample_ns,interval_ns,interpolation,pacing_source,draw_cpu_ns\n");
  }
  uint64_t trace_producer_start = 0, trace_producer_complete = 0, trace_input = 0;
  uint64_t trace_producer_cpu = 0;
  const char *sync_trace_path = getenv("AR_FRAME_SYNC_TRACE");
  char *sync_trace_buffer = NULL;
  FILE *sync_trace = OpenBufferedDiagnosticTrace(
      !config->headless ? sync_trace_path : NULL, 4 * 1024 * 1024, &sync_trace_buffer);
  if (sync_trace) fprintf(sync_trace,
      "sample_ns,complete_ns,interval_ns,remainder_ns,alpha,tick,produced,interpolation,source_ns,epoch,tick_delta,pacing_source,target_ns,map_group,map_number,presentation_sample_ns\n");
  if (stream_enabled && HostFrameProducer_Enabled()) {
    stream.queue = HostFrameQueue_Create();
    if (!stream.queue) {
      /* No game tick has run yet, so abandoning the owner here cannot migrate
       * a live coroutine. Allocation failure preserves the normal renderer. */
      HostFrameProducer_Shutdown();
      fprintf(stderr, "[frame-producer] packet allocation failed; synchronous fallback\n");
    }
  }
  bool logged_input_ready = false;
  uint64_t last_tick = SDL_GetTicks();  /* headless-only pacing (§3.6) */
  const uint32 emulation_frame_interval_ms =
      (uint32)(kHostDisplayEmulationFrameIntervalNs /
               kNanosecondsPerMillisecond);

  /* M6/§3.1,§3.3: fixed-timestep accumulator, non-headless only. */
  static const int kMaxCatchupFrames = 3;     /* spiral-of-death cap, §3.1 */
  uint64_t accumulator = 0;
  uint64_t sync_source_ns = 0, sync_epoch = 0;
  int sync_last_tick = 0, sync_pacing = -1;
  bool sync_reset = true;
  uint64_t last_time_ns = SDL_GetTicksNS();
  const HostDisplayPresentMode emulated_frame_present_mode =
      HostDisplay_EmulatedFramePresentMode(
          config->headless, config->headless_video);
  while (running) {
    if (stream_pending) {
      const uint64_t trace_loop_ns = pacing_trace ? SDL_GetTicksNS() : 0;
      if (SessionFatal_Requested()) {
        HostFrameQueue_RequestPause(stream.queue);
        HostFrameProducer_Wait();
        stream_pending = false;
        running = false;
        continue;
      }
      /* Handle game input and presentation controls without live runner reads.
       * Other events wait for the producer to acknowledge ownership. */
      SDL_PumpEvents();
      const uint64_t trace_pump_end = pacing_trace ? SDL_GetTicksNS() : 0;
      /* PumpEvents adds this bookkeeping marker even when nobody supplied
       * input. It is not a reason to interrupt the independent source clock. */
      SDL_FlushEvent(SDL_EVENT_POLL_SENTINEL);
      SDL_Event pending_event;
      bool has_event = false;
      while (SDL_PeepEvents(&pending_event, 1, SDL_PEEKEVENT,
                 SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        const unsigned type = pending_event.type;
        /* These notifications are ignored by the ordinary event loop too. */
        const bool ignored = (type >= SDL_EVENT_JOYSTICK_AXIS_MOTION &&
            type <= SDL_EVENT_JOYSTICK_UPDATE_COMPLETE) ||
            type == SDL_EVENT_GAMEPAD_UPDATE_COMPLETE ||
            type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE ||
            type == SDL_EVENT_WINDOW_MOUSE_ENTER || type == SDL_EVENT_WINDOW_MOUSE_LEAVE ||
            type == SDL_EVENT_WINDOW_MOVED || type == SDL_EVENT_WINDOW_EXPOSED ||
            type == SDL_EVENT_WINDOW_OCCLUDED;
        /* Focus notifications do not change runner state or display timing. */
        const bool focus = type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
            type == SDL_EVENT_WINDOW_FOCUS_LOST;
        if (focus && g_window && pending_event.window.windowID == SDL_GetWindowID(g_window))
          HostInput_LogStatus(type == SDL_EVENT_WINDOW_FOCUS_GAINED ? "focus-gained" : "focus-lost");
        if (!ignored && !focus &&
            !HostInput_TryHandleStreamEvent(&pending_event, stream.sim_town)) {
          has_event = true;
          break;
        }
        SDL_PeepEvents(&pending_event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
      }
      atomic_store_explicit(&stream.input_sample,
          ((uint64_t)(uint32_t)SDL_GetTicks() << 32) | InputMap_State(), memory_order_release);
      /* Only the classified event above requests ownership. A second peek
       * races asynchronously posted gamepad updates: even an ignored sensor
       * or update-complete notification could otherwise pause the stream. */
      const uint64_t trace_events_end = pacing_trace ? SDL_GetTicksNS() : 0;
      if (has_event) stream_event = pending_event.type;
      if (has_event || SessionFatal_Requested())
        HostFrameQueue_RequestPause(stream.queue);
      stream_done = stream_done || HostFrameProducer_Poll();
      if (stream_done && stream.maintenance &&
          !has_event && !stream.stopped && !stream.transition &&
          !ScheduledSettings_IsDue() && !DevAutomation_RequiresHostService()) {
        /* A periodic persistence pass needs ownership, not an empty playout
         * queue. Draining future endpoints here held the producer idle until
         * their display time and made the next source tick start late. Poll's
         * acknowledgement makes runner reads safe; Resume preserves every
         * owned packet and the existing source/presentation clocks. */
        const uint64_t service_start_ns = SDL_GetTicksNS();
        RunPersistenceHousekeeping();
        const uint64_t service_end_ns = SDL_GetTicksNS();
        fprintf(stderr, "[stream-service] frames=%u full-waits=%u event=0 elapsed-ms=%.3f "
            "retained=%u work-ms=%.3f resume-slack-ms=%.3f tick=%d\n",
            stream.frames, stream.full_waits,
            (double)(service_end_ns - stream_owner_service_ns) / 1e6,
            HostFrameQueue_ReadyCount(stream.queue),
            (double)(service_end_ns - service_start_ns) / 1e6,
            ((double)stream.next_ns - (double)service_end_ns) / 1e6,
            snes_frame_counter);
        if (SessionFatal_Requested()) continue;
        stream_owner_service_ns = service_end_ns;
        stream.service_deadline_ns = service_interval_ns
            ? service_end_ns + service_interval_ns : 0;
        HostFrameQueue_Resume(stream.queue);
        if (!HostFrameProducer_Submit(ProduceFrameStream, &stream)) {
          SessionFatal_Request("Frame producer rejected its maintenance resume.");
          continue;
        }
        stream_done = false;
      }
      const uint64_t present_start_ns = SDL_GetTicksNS();
      const uint64_t preparation_ns =
          !stream.playout.interpolate && stream.playout.delay_ns
              ? HostDisplay_NativeFrameSampleTime(present_start_ns)
              : HostFramePlayout_PreparationTime(stream.playout, present_start_ns,
                                                 HostDisplay_NextPresentationDeadline());
      const uint64_t target_ns = HostFramePlayout_Target(stream.playout, preparation_ns);
      const HostFramePacket *packet;
      /* Keep future endpoints queued. A latest-frame mailbox advances the
       * interpolation pair too early and then repeatedly clamps/holds it. */
      unsigned ready = HostFrameQueue_ReadyCount(stream.queue);
      const unsigned trace_queue_before = ready;
      unsigned trace_uploads = 0;
      if (!stream.playout.interpolate) {
        /* No interpolation history is needed. Discard stale captures before
         * upload, not after doing their CPU/GPU preparation. Ticks still ran. */
        while (ready > 1 &&
            (packet = HostFrameQueue_Peek(stream.queue, 1)) &&
            HostFramePlayout_AcceptsPacket(stream.playout, packet->source_ns, target_ns)) {
          HostFrameQueue_Release(stream.queue);
          --ready;
        }
      }
      while (ready && HostFramePlayout_NeedsEndpoint(
                 stream.playout, stream_endpoint_ns, target_ns) &&
             (packet = HostFrameQueue_Read(stream.queue)) &&
             HostFramePlayout_AcceptsPacket(stream.playout, packet->source_ns, target_ns)) {
        --ready;
        if (!HostDisplay_StageOwnedFrame(&packet->frame)) {
          HostFrameQueue_RequestPause(stream.queue);
          SessionFatal_Request("Frame pipeline could not upload its owned endpoint.");
          break;
        }
        stream_endpoint_ns = packet->source_ns;
        stream_tick = packet->tick;
        if (pacing_trace) {
          ++trace_uploads;
          trace_producer_start = packet->started_ns;
          trace_producer_complete = packet->completed_ns;
          trace_input = packet->input_ns;
          trace_producer_cpu = packet->cpu_ns;
        }
        const uint64_t now = SDL_GetTicksNS();
        if (!stream_report_ns) stream_report_ns = now;
        stream_frames++;
        stream_last_source_ns = packet->source_ns;
        stream_work_ms += (double)(packet->completed_ns - packet->started_ns) / 1e6;
        stream_copy_ms += (double)packet->copy_ns / 1e6;
        stream_bytes += (double)packet->copied_bytes / (1024 * 1024);
        stream_age_ms += (double)(now - packet->source_ns) / 1e6;
        HostFrameQueue_Release(stream.queue);
        if (now - stream_report_ns >= 1000000000) {
          fprintf(stderr, "[frame-stream] captures-hz=%.3f work-ms=%.3f copy-ms=%.3f "
              "copy-MiB=%.3f upload-age-ms=%.3f source-age-ms=%.3f\n",
              stream_frames * 1e9 / (double)(now - stream_report_ns),
              stream_work_ms / stream_frames, stream_copy_ms / stream_frames,
              stream_bytes / stream_frames, stream_age_ms / stream_frames,
              (double)(now - stream_last_source_ns) / 1e6);
          stream_report_ns = now;
          stream_frames = 0;
          stream_work_ms = stream_copy_ms = stream_bytes = stream_age_ms = 0;
        }
      }
      if (!stream_done || HostFrameQueue_Read(stream.queue)) {
        const uint64_t trace_ready_ns = pacing_trace ? SDL_GetTicksNS() : 0;
        const uint64_t sample_now_ns = SDL_GetTicksNS();
        const uint64_t sample_ns =
            !stream.playout.interpolate && stream.playout.delay_ns ? preparation_ns : sample_now_ns;
        const uint64_t present_target_ns = HostFramePlayout_Target(stream.playout, sample_ns);
        /* An early draw must not clamp an old pair just because the producer
         * has not reached the output-time endpoint yet. Keep servicing it
         * until that endpoint arrives or the output deadline itself is due. */
        const bool await_endpoint = HostFramePlayout_AwaitEndpoint(
            stream.playout, stream_endpoint_ns, present_target_ns,
            sample_now_ns, sample_ns);
        const float phase = HostFramePlayout_Phase(
            stream.playout, stream_endpoint_ns, present_target_ns);
        const bool presented = !await_endpoint && HostDisplay_TryRepresentFrame(
            phase, true, stream.playout.interpolate, false);
        if (presented && !stream.playout.interpolate && stream_endpoint_ns) {
          PerformanceMetrics_Add(kPerformanceCount_NativePresents, 1);
          if (cadence_epoch == stream_epoch) {
            if (stream_tick == cadence_tick)
              PerformanceMetrics_Add(kPerformanceCount_SourceHolds, 1);
            else if (stream_tick > cadence_tick + 1)
              PerformanceMetrics_Add(kPerformanceCount_SourceSkips, stream_tick - cadence_tick - 1);
          }
          cadence_epoch = stream_epoch;
          cadence_tick = stream_tick;
        }
        /* Capture completion before either CSV write; a phase-trace flush
         * used to inflate the next pacing row's apparent backend duration. */
        const uint64_t trace_complete_ns = (stream_trace || pacing_trace)
            ? SDL_GetTicksNS() : 0;
        if (presented && stream_trace)
          fprintf(stream_trace, "%llu,%llu,%llu,%.8f,%llu,%d,%llu,%d,%llu\n",
              (unsigned long long)present_start_ns, (unsigned long long)trace_complete_ns,
              (unsigned long long)stream_endpoint_ns, phase,
              (unsigned long long)present_target_ns, stream_tick,
              (unsigned long long)stream.interval_ns, stream.playout.interpolate,
              (unsigned long long)stream_epoch);
        /* Preserve slow idle iterations too, so a delayed event pump/owner
         * acknowledgement is not mistaken for an unexplained sleep gap. */
        if (pacing_trace && (presented || trace_uploads ||
                            trace_ready_ns - trace_loop_ns > 1000000)) {
          const HostDisplayPresentTrace trace = await_endpoint
              ? (HostDisplayPresentTrace){0} : HostDisplay_LastPresentTrace();
          fprintf(
              pacing_trace,
              "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%u,%u,%d,%d,%llu,%llu,%llu,%"
              "llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%llu\n",
              (unsigned long long)trace_loop_ns, (unsigned long long)present_start_ns,
              (unsigned long long)trace_ready_ns, (unsigned long long)trace.draw_start_ns,
              (unsigned long long)trace_complete_ns, (unsigned long long)trace.deadline_ns,
              (unsigned long long)(trace_ready_ns - present_start_ns), trace_uploads,
              (unsigned long long)trace.draw_ns, (unsigned long long)trace.swap_ns,
              (unsigned long long)trace.vector_wait_ns, trace_queue_before,
              HostFrameQueue_ReadyCount(stream.queue), presented, stream_tick,
              (unsigned long long)stream_epoch, (unsigned long long)stream_endpoint_ns,
              (unsigned long long)trace_producer_start, (unsigned long long)trace_producer_complete,
              (unsigned long long)trace_input, (unsigned long long)(trace_pump_end - trace_loop_ns),
              (unsigned long long)(trace_events_end - trace_pump_end),
              (unsigned long long)(present_start_ns - trace_events_end),
              (unsigned long long)trace.submit_start_ns, (unsigned long long)trace.backend_flush_ns,
              (unsigned long long)trace.backend_acquire_ns,
              (unsigned long long)trace.backend_submit_ns, (unsigned long long)trace_producer_cpu,
              (unsigned long long)present_target_ns, (unsigned long long)sample_ns,
              (unsigned long long)stream.interval_ns, stream.playout.interpolate,
              HostDisplay_PacingSource(), (unsigned long long)trace.draw_cpu_ns);
        }
        if (trace_complete_ns) {
          const uint64_t trace_write_ns = SDL_GetTicksNS() - trace_complete_ns;
          if (trace_write_ns > 1000000)
            fprintf(stderr, "[frame-trace-write] start-ns=%llu elapsed-ms=%.3f\n",
                (unsigned long long)trace_complete_ns, (double)trace_write_ns / 1e6);
        }
        HostDisplay_YieldIfNoPresent(presented, false, false);
        continue;
      }
      stream_pending = false;
      fprintf(stderr, "[stream-service] frames=%u full-waits=%u event=%u elapsed-ms=%.3f\n",
          stream.frames, stream.full_waits, stream_event,
          (double)(SDL_GetTicksNS() - stream_owner_service_ns) / 1e6);
      stream_event = 0;
      RunPostTickHousekeeping();
      HostInput_MarkFrameDrawn();
      DevAutomation_ArmScheduledDioramaDump();
      HostDevTools_ServiceDioramaDump();
      DevAutomation_CaptureScheduledScreenshot(NULL);
      if (stream.transition && !SessionFatal_Requested()) {
        stream.next_ns = stream_endpoint_ns = 0;
        HostDisplay_SetProducerPacing(false);
        HostDisplay_InvalidatePresentHistory();
        DrawAndPresentFrame(emulated_frame_present_mode, 0.0f);
      }
      if (stream.stopped || SessionFatal_Requested()) { running = false; continue; }
      accumulator = 0;
      sync_reset = true;
      last_time_ns = SDL_GetTicksNS();
    }
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
      HostDisplay_SetProducerPacing(false);
      stream.next_ns = stream_endpoint_ns = 0;
      /* §3.4: don't accumulate wall-clock time spent paused — otherwise
       * unpausing would fire a burst of catch-up ticks. last_time_ns is
       * re-stamped every paused iteration below, so it's always "just now"
       * by the time the game actually unpauses. */
      accumulator = 0;
      sync_reset = true;
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

    const uint64_t source_interval_ns = HostDisplayPacing_SourceFrameIntervalNs(
        kHostDisplayEmulationFrameIntervalNs, Diorama_IsActiveThisFrame(),
        g_settings.gpu_interp_enabled,
        (InterpolationSourceRate)g_settings.gpu_interp_source_rate);
    const bool source_interpolation = Diorama_IsActiveThisFrame() && g_settings.gpu_interp_enabled;
    const uint64_t source_now_ns = SDL_GetTicksNS();
    if (stream.queue && stream.playout.interval_ns &&
        (stream.interval_ns != source_interval_ns ||
         stream.playout.interpolate != source_interpolation ||
         (stream.next_ns && source_now_ns > stream.next_ns + 3 * source_interval_ns))) {
      /* A changed source clock or long host interruption invalidates the old
       * interpolation pair. Establish a fresh synchronous capture before
       * streaming again; never reinterpret that pair with the new period. */
      stream.next_ns = stream_endpoint_ns = 0;
      HostDisplay_InvalidatePresentHistory();
    }
    stream.interval_ns = source_interval_ns;
    stream.playout = HostFramePlayout_Create(source_interval_ns,
        source_interpolation, delay_permille);
    if (!stream.playout.interpolate) {
      if (native_delay_us >= 0) stream.playout.delay_ns = (uint64_t)native_delay_us * 1000;
      if (g_settings.refresh_mode == kRefreshMode_Unlimited) stream.playout.delay_ns = 0;
    }
    /* Below-source software limits intentionally coalesce several ticks into
     * one draw. Keep that established accumulator behavior; a bounded image
     * queue must not throttle the game clock down to the requested render rate. */
    const bool low_render_limit = g_settings.refresh_mode == kRefreshMode_Limit &&
        (uint64_t)g_settings.frame_limit_fps < kNanosecondsPerSecond / source_interval_ns;
    if (stream.queue && !low_render_limit && !s_window_hidden && !HostInput_IsTurbo() &&
        (Diorama_IsActiveThisFrame() || (sim_stream_enabled &&
            ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                g_ram[kActRaiserWram_CurrentMap]) &&
            !ActRaiserSimMenu_OwnsInput() && !ActRaiserSimMenu_OwnsPresentation())) &&
        !g_settings.scene_inspector && HostDisplay_CanPresentDuringProduction()) {
      HostDisplay_SetProducerPacing(true);
      stream.sim_town = !Diorama_IsActiveThisFrame();
      stream.underlay_serial = stream.sim_town ? SimWorldMap_Serial() : 0;
      stream.map_group = g_ram[kActRaiserWram_MapGroup];
      stream.map_number = g_ram[kActRaiserWram_CurrentMap];
      const uint64_t now = SDL_GetTicksNS();
      if (!stream.next_ns) {
        stream.next_ns = now;
        ++stream_epoch;
      }
      atomic_store_explicit(&stream.input_sample,
          ((uint64_t)(uint32_t)SDL_GetTicks() << 32) | InputMap_State(), memory_order_release);
      stream.stopped = stream.transition = false;
      stream_owner_service_ns = now;
      stream.service_deadline_ns = service_interval_ns ? now + service_interval_ns : 0;
      HostFrameQueue_Resume(stream.queue);
      if (!HostFrameProducer_Submit(ProduceFrameStream, &stream)) {
        SessionFatal_Request("Frame producer rejected its bounded stream.");
        continue;
      }
      stream_pending = true;
      stream_done = false;
      continue;
    }
    stream.next_ns = stream_endpoint_ns = 0;

    HostDisplay_SetProducerPacing(false);

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
      const bool catchup_limited = accumulator > catchup_cap_ns;
      if (catchup_limited) accumulator = catchup_cap_ns;
      const int pacing_source = HostDisplay_PacingSource();
      const bool native_sync = !(g_diorama_frame_active && g_settings.gpu_interp_enabled);
      const uint64_t sample_ns = HostDisplay_NativeFrameSampleTime(now_ns);
      if (catchup_limited) sync_reset = true;
      if (sync_reset || pacing_source != sync_pacing) {
        ++sync_epoch;
        sync_last_tick = 0;
        sync_reset = false;
      }
      sync_pacing = pacing_source;
      const uint64_t target_ns = now_ns;
      const int tick_before = snes_frame_counter;

      bool produced_frame = false;
      while (running && accumulator >= emulation_frame_interval_ns) {
        bool stop_requested = false;
        RunOneEmulatedTick(&stop_requested);
        if (stop_requested) running = false;
        accumulator -= emulation_frame_interval_ns;
        produced_frame = true;
      }
      if (produced_frame) sync_source_ns = now_ns - accumulator;
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
          presented = DrawAndPresentFrame(emulated_frame_present_mode, alpha);
        } else if (HostDisplay_TryRepresentFrame(
                       alpha,
                       g_diorama_frame_active,
                       g_settings.gpu_interp_enabled,
                       HostInput_IsPausedRedrawPending())) {
          presented = true;
        }
      }
      /* Count the selected native source at every completed present, including
       * held ticks. Trace source schedule and completion separately; this is
       * software timeline age, not physical input/display latency. */
      if (presented && native_sync && sync_source_ns) {
        PerformanceMetrics_Add(kPerformanceCount_NativePresents, 1);
        if (sync_last_tick) {
          if (snes_frame_counter == sync_last_tick)
            PerformanceMetrics_Add(kPerformanceCount_SourceHolds, 1);
          else if (snes_frame_counter > sync_last_tick + 1)
            PerformanceMetrics_Add(kPerformanceCount_SourceSkips, snes_frame_counter - sync_last_tick - 1);
        }
        sync_last_tick = snes_frame_counter;
      }
      if (presented && sync_trace) {
        const uint64_t completed_ns = SDL_GetTicksNS();
        fprintf(sync_trace, "%llu,%llu,%llu,%llu,%.8f,%d,%d,%d,%llu,%llu,%d,%d,%llu,%u,%u,%llu\n",
            (unsigned long long)now_ns, (unsigned long long)completed_ns,
            (unsigned long long)emulation_frame_interval_ns,
            (unsigned long long)accumulator, alpha, snes_frame_counter,
            produced_frame, !native_sync, (unsigned long long)sync_source_ns,
            (unsigned long long)sync_epoch, snes_frame_counter - tick_before,
            pacing_source, (unsigned long long)target_ns,
            g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap],
            (unsigned long long)sample_ns);
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
  if (stream_pending) HostFrameQueue_RequestPause(stream.queue);
  HostFrameProducer_Shutdown();
  HostFrameQueue_Destroy(stream.queue);
  if (stream_trace) fclose(stream_trace);
  if (pacing_trace) fclose(pacing_trace);
  free(stream_trace_buffer);
  free(pacing_trace_buffer);
  HostDisplay_EnablePresentTrace(false);
  if (sync_trace) fclose(sync_trace);
  free(sync_trace_buffer);
  HostDisplay_SetProducerPacing(false);
  HostDisplay_InvalidatePresentHistory();
}
