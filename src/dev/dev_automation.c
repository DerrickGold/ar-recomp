#include "dev/dev_automation.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "actraiser/actraiser_rtl.h"
#include "app/run_dir.h"
#include "app/runtime_settings.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "dev/host_dev_tools.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/support/utf8_fs.h"

enum { kUnreadEnvironmentOption = -2 };

/* AR_DIORAMA_DUMP_GF=<gf>[,<gf>...]: arm the Shift+D layer dump from a replay
 * instead of the keyboard, so a diorama frame can be inspected headlessly.
 * The PNGs keep the captured ALPHA, which is what makes F4's half-add
 * annotation verifiable without looking at the screen. Same shape as
 * AR_VRAMDUMP_GF. */
void DevAutomation_ArmScheduledDioramaDump(void) {
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
          HostDevTools_ArmDioramaDump();
          break;
        }
        const char *comma = strchr(at, ',');
        at = comma ? comma + 1 : NULL;
      }
    }
  }
}

/* Framebuffer capture to PPM (works headless — g_pixels is always populated).
 * AR_SHOT_AT_GF=N      : one shot to saves/shot.ppm at game-frame >= N.
 * AR_SHOT_EVERY=N      : a SERIES — saves/shot_<gf>.ppm every N game-frames,
 *   optionally bounded by AR_SHOT_FROM / AR_SHOT_TO. Lets us compare steady
 *   state vs bug state frame by frame.
 * AR_SHOT_REQUIRE_COMPOSITE=1: fail the run instead of using raw PPU fallback. */
void DevAutomation_CaptureScheduledScreenshot(void) {
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

void DevAutomation_AfterTicks(void) {
  /* AR_WARP_AT=<gameframe>: fire the AR_WARP target automatically once the
   * 16-bit game-frame counter reaches the value. Headless runs can't press
   * F6; used e.g. to sweep the warp table capturing each level's music src
   * (AR_MUSICLOG). Same transition-capable-state caveats as F6. */
  {
    static long warp_at = kUnreadEnvironmentOption;
    static bool warp_fired;
    if (warp_at == kUnreadEnvironmentOption) {
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
      if (gf != kActRaiserPowerOnGameFrame && gf >= (unsigned)warp_at) {
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
    static long diorama_at = kUnreadEnvironmentOption;
    static bool diorama_fired;
    if (diorama_at == kUnreadEnvironmentOption) {
      const char *at = getenv("AR_DIORAMA_AT");
      diorama_at = (at && at[0]) ? strtol(at, NULL, 0) : -1;
    }
    if (diorama_at >= 0 && !diorama_fired) {
      const unsigned gf =
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      /* $0088 is $5555-filled before the game initialises it; ignore that
       * boot sentinel or every target fires on frame 0. */
      if (gf != kActRaiserPowerOnGameFrame &&
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
}

/* Keep automated runs bounded in either presentation path. This used to be
 * checked only inside the headless branch, which meant an otherwise identical
 * real-compositor capture could not exit cleanly after writing its artifact. */
bool DevAutomation_ShouldQuit(void) {
  static int quit_frames = kUnreadEnvironmentOption;
  if (quit_frames == kUnreadEnvironmentOption) {
    const char *value = getenv("AR_QUIT_FRAMES");
    quit_frames = value ? atoi(value) : -1;
  }
  return quit_frames > 0 && snes_frame_counter >= quit_frames;
}
