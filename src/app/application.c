#include "app/application.h"
#include "app/game_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "app/config.h"
#include "app/ini_upgrade_apply.h"
#include "app/portable_paths.h"
#include "app/run_dir.h"
#include "app/scheduled_settings.h"
#include "app/session_fatal.h"
#include "app/session_recovery.h"
#include "app/settings.h"
#include "app/user_data_dir.h"
#include "constants.h"
#include "dev/host_runtime_diagnostics.h"
#include "host/host_display.h"
#include "host/host_localization.h"
#include "host/host_video.h"
#include "save/save_paths.h"
#include "save/save_system.h"
#include "snesrecomp/host/launcher.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/types.h"
#include "snesrecomp/support/file.h"
#include "snesrecomp/support/utf8_fs.h"

static const char kWindowTitle[] = "ActRaiser (Recompiled)";
/* Keep the compositor identity in sync with the shipped .desktop basename. */
#define AR_APP_IDENTIFIER "dev.quintet-enix.actraiser-recomp"
#define AR_APP_VERSION "0.1.0-dev"

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

typedef struct Application {
  const char *rom_path;
  const char *config_path;
  uint8_t *rom_data;
  size_t rom_size;
  bool headless;        /* no window/renderer; PPU emulation still runs */
  bool headless_video;  /* headless, but with a hidden-window renderer */
  bool video;           /* !headless || headless_video */
} Application;

/* Argument parsing, the portable-bundle chdir, the per-run artifact dir, the
 * shipped-defaults ini upgrade, the config layer, and the ROM read.
 * Returns a process exit code on failure, or -1 to continue booting. */
static int Application_ParseArgs(Application *app, int argc, char **argv) {
  app->rom_path = NULL;
  app->config_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      app->config_path = argv[++i];
    } else if (argv[i][0] != '-') {
      app->rom_path = argv[i];
    }
  }

  /* Desktop launchers resolve portable/custom/global storage once and pass
   * AR_USER_DATA_DIR. Honor it before folder-bundle anchoring; application
   * resources and writable player data may live in different directories. */
  static char rom_abs[kHostPathCapacity], config_abs[kHostPathCapacity];
  const char *data_root = SDL_getenv_unsafe("AR_USER_DATA_DIR");
  const bool explicit_data = data_root && *data_root;
  const bool folder_bundle = PortablePaths_IsBundle();
  if (explicit_data || folder_bundle) {
    /* Resolve inputs before changing the data-root working directory. */
    if (app->rom_path) {
      if (!snesrecomp_abspath(app->rom_path, rom_abs, sizeof(rom_abs))) return 1;
      app->rom_path = rom_abs;
    }
    if (app->config_path) {
      if (!snesrecomp_abspath(app->config_path, config_abs, sizeof(config_abs))) return 1;
      app->config_path = config_abs;
    }
    if (explicit_data) {
      SaveError error = {{0}};
      char absolute_root[kHostPathCapacity];
      if (!snesrecomp_abspath(data_root, absolute_root, sizeof(absolute_root)) ||
          !SavePaths_EnsureDirectory(absolute_root, &error) || sr_utf8_chdir(absolute_root)) {
        fprintf(stderr, "[storage] Cannot use the selected data directory: %s\n", data_root);
        return 1;
      }
      /* Child tools inherit the resolved root even if the caller used a
       * relative path. In-process game resets retain this working directory. */
      if (SDL_setenv_unsafe("AR_USER_DATA_DIR", absolute_root, 1) != 0) return 1;
    } else snesrecomp_anchor_to_exe_dir();
    if (!app->rom_path && folder_bundle &&
        snesrecomp_exe_dir_path("user-rom.sfc", rom_abs, sizeof(rom_abs)))
      app->rom_path = rom_abs;
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
static void Application_ResolveDisplayAndSettings(Application *app) {
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
  const bool ws_headless = getenv("AR_WS_HEADLESS") && getenv("AR_WS_HEADLESS")[0]
                          && getenv("AR_WS_HEADLESS")[0] != '0';
  HostDisplay_SetWidescreenRuntimeAllowed(!app->headless || ws_headless);
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

/* Process-owned SDL, window and renderer. The window/renderer
 * body is skipped for a pure-headless run; a headless_video run takes it with a
 * hidden window so the present path still executes for frame capture.
 * Returns a process exit code if SDL_Init fails, or -1 to continue booting. */
static int Application_CreateVideo(Application *app) {
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
    /* Deliberately last in the video setup so focus lands on a window that is
     * fully configured, and skipped for headless_video (that window is
     * SDL_WINDOW_HIDDEN and must never steal focus from a batch run). */
    if (!app->headless_video) HostVideo_TakeFocus();
  }
  return -1;
}

static int Application_Shutdown(Application *app, GameSessionResult result) {
  const bool fatal_session = SessionFatal_Requested();
  const bool settings_flush_failed = result.settings_failed;
  const bool save_flush_failed = result.save_failed;
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
  return result.exit_code;
}

int Application_Run(int argc, char **argv) {
  Application app = {0};
  int rc = Application_ParseArgs(&app, argc, argv);
  if (rc >= 0) return rc;

  Application_ResolveDisplayAndSettings(&app);
  HostRuntimeDiagnostics_ConfigureChecks();
  rc = Application_CreateVideo(&app);
  if (rc >= 0) {
    free(app.rom_data);
    return rc;
  }
  /* Process-scoped actions stay consumed across game resets. */
  ScheduledSettings_Init();
  const GameSessionConfig config = {
    .rom_data = app.rom_data,
    .rom_size = app.rom_size,
    .headless = app.headless,
    .headless_video = app.headless_video,
  };
  GameSessionResult result;
  do {
    result = GameSession_Run(&config);
  } while (result.restart);
  return Application_Shutdown(&app, result);
}
