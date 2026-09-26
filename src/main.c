#include "snesrecomp/support/utf8_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <SDL3/SDL_main.h> /* SDL supplies UTF-8 argv from the wide command line. */
#include <process.h>
#else
#include <unistd.h>
#endif

#include "actraiser/actraiser_action_bg.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser_game.h"
#include "actraiser/actraiser_rtl.h"
#include "audio/audio_presentation_policy.h"
#include "app/config.h"
#include "constants.h"
#include "render/crt_post.h"
#include "dev/dev_automation.h"
#include "dev/host_dev_tools.h"
#include "dev/host_runtime_diagnostics.h"
#include "dev/native_audio_trace.h"
#include "dev/oracle_trace.h"
#include "dev/sfx_census.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "present/display_geometry.h"
#include "app/forced_input.h"
#include "replacements/hd_replacement_host.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "actraiser/regional/actraiser_actor_art.h"
#include "audio/audio_session.h"
#include "host/host_ppu_output.h"
#include "host/host_display.h"
#include "host/host_display_pacing.h"
#include "host/host_frame_surfaces.h"
#include "host/host_input.h"
#include "host/host_localization.h"
#include "host/host_video.h"
#include "app/ini_upgrade_apply.h"
#include "app/input_map.h"
#include "app/input_replay.h"
#include "manual/manual_reader.h"
#include "app/performance_metrics.h"
#include "platform/sdl/font_coverage_cli.h"
#include "app/settings_session.h"
#include "platform/sdl/text_preview_cli.h"
#include "app/portable_paths.h"
#include "present/present.h"
#include "present/presentation_frame_generation.h"
#include "present/presentation_textures.h"
#include "present/render_comparison.h"
#include "present/render_preparation.h"
#include "app/run_dir.h"
#include "app/runtime_diagnostics.h"
#include "app/runtime_settings.h"
#include "save/save_system.h"
#include "save/save_slot_host.h"
#include "save/save_paths.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "app/scheduled_settings.h"
#include "app/session_fatal.h"
#include "app/session_recovery.h"
#include "app/settings.h"
#include "settings_overlay/settings_overlay.h"
#include "settings_overlay/regional/regional_host.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_frame_capture.h"
#include "sim/sim_render_metadata.h"
#include "sim/town/sim_town_ground_art.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_map_build.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/host/launcher.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/support/file.h"
#include "app/user_data_dir.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
enum {
  kDefaultPowerOnSramFill = 0x60,
  kUninitializedEnvironmentOption = -2,
};
/* Reverse-domain app identifier: compositors key window grouping and icon
 * lookup off this, and a shipped .desktop file must share its basename. */
#define AR_APP_IDENTIFIER "dev.quintet-enix.actraiser-recomp"
#define AR_APP_VERSION "0.1.0-dev"
static bool s_window_hidden;  /* true while MINIMIZED or HIDDEN: skip present */

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



/* ---------------------------------------------------------------------------
 * Boot decomposition.
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

  HostRuntimeDiagnostics_InitTrace();

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
  HostLocalization_PublishInstalledPacks();
  Settings_InitWithFile(settings_path);
  HostDisplay_ResolveVideoGeometry(false);

  /* Display presets depend on whether the resolved aspect selected a wide
   * budget. Finalize only after g_ws_active/g_ws_extra are authoritative. */
  Settings_FinalizeDisplayMode();
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

/* Overlay, world map, diorama manifest, the injected overlay hooks (layer editor,
 * manual), input, and music. Injection rather than direct calls is what keeps
 * settings_overlay.c testable with no renderer at all -- see settings_overlay.h. */
static void AppBoot_InstallSubsystems(AppBoot *app) {
  HostLocalization_Install(app->headless);
  if (!SettingsOverlay_Init(&g_render_device, g_window,
                            app->rom_data, app->rom_size))
    Die("font atlas creation for settings overlay failed");
  HostLocalization_InstallInterfaceFonts();
  /* The world-map image and pure development-builder tables are immutable ROM
   * data. Failure is not fatal: consumers retain the authentic presentation. */
  if (SimWorldMap_Init(app->rom_data, app->rom_size))
    SimWorldMapBuild_Init(app->rom_data, app->rom_size);
  if (!SimTownGroundArt_Init(app->rom_data, app->rom_size))
    fprintf(stderr, "[world-navigation] native town ground unavailable\n");
  if (!Diorama_InitRomBackdrops(app->rom_data, app->rom_size))
    fprintf(stderr, "[diorama] named ROM backdrops unavailable\n");
  HostLocalization_LoadRegionalMedia();
  ActRaiserActorArt_Initialize(ActRaiserRegionalMedia_ActorArt());
  if (!ActRaiserActionBg_InitRoomScenes(app->rom_data, app->rom_size))
    fprintf(stderr, "[action-room-scene] immutable loader unavailable\n");
  /* Per-room diorama layer overrides. Absent file is the normal case and leaves
   * every room drawing as built. */
  Diorama_LoadLayerManifest();
  SettingsOverlay_SetInspectorInfoProvider(
      HostDevTools_FormatInspectorInfo);
  SettingsOverlayRegionalHost_InstallHooks();
  Diorama_InstallLayerEditor();

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

  AudioSession_Install();

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
  HostPpuOutput_Rebind();
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
        : kActRaiserPowerOnWramFill;
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

  SaveSlotHost_AttachBatterySave(app->headless);

  OracleTrace_Init(RtlGameRunner());
  ForcedInput_Init();
  InputReplay_Init();
  SaveSlotHost_InitializeRegionalCampaign();
  SettingsOverlayRegionalHost_InstallPrompts(app->headless);
  if (!InputReplay_SetPolicyDigest(ActRaiserRegional_ReplayDigest, NULL))
    Die("Regional replay identity could not be initialized.");
  SettingsSession_Start();
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

  AudioSession_StartOutput();
  SaveSlotHost_InstallHooks();
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

/* Event routing stays flat; helpers own resource reset and modal input
 * precedence. Device add/remove and key release always reach their owners. */
static void AppLoop_PumpEvents(AppBoot *app, bool *running) {
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

    }
  }
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
    const bool pipeline_log = HostRuntimeDiagnostics_PipelineLoggingEnabled();
    PerformanceMetrics_Configure(g_settings.performance_overlay != 0 || pipeline_log,
        g_settings.performance_overlay != 0 || pipeline_log);
    const PerformanceScope events = PerformanceMetrics_Begin(kPerformance_Events);
    AppLoop_PumpEvents(app, &running);
    PerformanceMetrics_End(events);

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

/* Teardown, in strict reverse-dependency order: everything owning a texture, a
 * shader, or render state goes before the renderer that created it. */
static int AppShutdown(AppBoot *app, char **argv) {
  const bool fatal_session = SessionFatal_Requested();
  const bool settings_flush_failed = !SettingsSession_Finish(fatal_session);
  bool save_flush_failed = !SaveSlotHost_FlushBatterySave();

  DumpDiagState(fatal_session
                    ? "fatal"
                    : RuntimeSettings_LifecycleRequest() ==
                              kRuntimeLifecycle_Restart
                          ? "restart" : "exit");
  SimPhase0Trace_Close();
  SimRenderMetadata_TraceClose();
  SimFrameCapture_Shutdown();
  ActRaiserActionBg_Shutdown();
  ActRaiserActorArt_Shutdown();
  HostLocalization_Shutdown();

  /* Stop the sole audio producer before reading observer-owned capture state
   * or removing subscriptions. The run directory remains live for reports. */
  AudioSession_Shutdown();
  SfxCensus_Report();
  NativeAudioTrace_Report();

  InputReplay_Shutdown();
  OracleTrace_Shutdown();
  NativeAudioTrace_Shutdown();
  HdReplacementHost_Shutdown();
  HostPpuOutput_Reset();
  PresentRendererResources_Reset();
  SimTownGroundArt_Shutdown();
  DioramaFrameGeneration_Shutdown();
  Diorama_Shutdown(&g_render_device);
  ManualReader_DestroyTextures();
  SettingsOverlay_Destroy();
  HostLocalization_ReleaseFonts();
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
  if (!SaveSlotHost_Close()) save_flush_failed = true;

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
  HostRuntimeDiagnostics_ConfigureChecks();
  rc = AppBoot_CreateVideo(&app);
  if (rc >= 0) return rc;
  AppBoot_InstallSubsystems(&app);
  AppBoot_StartGame(&app);
  ActRaiserLocalizationRuntime_ApplySettings();
  if (!HostLocalization_ExitRequested())
    AppRunMainLoop(&app);
  return AppShutdown(&app, argv);
}
