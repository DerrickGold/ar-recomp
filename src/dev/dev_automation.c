#include "dev/dev_automation.h"

#include <limits.h>
#include <math.h>
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
#include "diorama/diorama_frame_generation.h"
#include "present/presentation_frame_generation.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/support/utf8_fs.h"

enum { kUnreadEnvironmentOption = -2 };

typedef struct ScheduledAction {
  long at;
  bool fired;
} ScheduledAction;

static ScheduledAction warp = {.at = kUnreadEnvironmentOption};
static ScheduledAction diorama = {.at = kUnreadEnvironmentOption};

/* Queried on the runner owner; consumed on main after it acknowledges stop.
 * A due state-changing action must drain the old scene, even when routine
 * persistence maintenance can safely leave owned frames queued. */
static bool ScheduledActionDue(ScheduledAction *action, const char *option,
                               unsigned gf) {
  if (action->at == kUnreadEnvironmentOption) {
    const char *value = getenv(option);
    action->at = value && value[0] ? strtol(value, NULL, 0) : -1;
  }
  return !action->fired && action->at >= 0 &&
      gf != kActRaiserPowerOnGameFrame && gf >= (unsigned)action->at;
}

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
 * AR_SHOT_REQUIRE_COMPOSITE=1: fail the run instead of using raw PPU fallback.
 * AR_SHOT_PHASE=0..1: capture a fixed interpolated phase instead of the endpoint.
 * Pair with AR_FRAME_CAPTURE_TICK_CLOCK=1 in headless-video tests so file IO
 * does not expire motion pairs; this does not alter live game/host clocks. */
static bool schedule_initialized;
static bool shot_done;
static bool shot_at_enabled;
static bool shot_series_enabled;
static unsigned shot_at;
static unsigned shot_every;
static unsigned shot_from;
static unsigned shot_to;
static float shot_phase = kPresentationFrameGenerationPhaseNone;

static void InitScreenshotSchedule(void) {
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
    value = getenv("AR_SHOT_PHASE");
    if (value && value[0]) {
      char *end = NULL;
      const float phase = strtof(value, &end);
      if (end == value || *end || !isfinite(phase) || phase < 0 || phase > 1)
        SessionFatal_Request("AR_SHOT_PHASE must be a number from 0 to 1.");
      else
        shot_phase = phase;
    }
    schedule_initialized = true;
  }
}

bool DevAutomation_RequiresHostService(void) {
  InitScreenshotSchedule();
  const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if (ScheduledActionDue(&warp, "AR_WARP_AT", gf) ||
      ScheduledActionDue(&diorama, "AR_DIORAMA_AT", gf))
    return true;
  if ((shot_at_enabled && !shot_done && gf >= shot_at) ||
      (shot_series_enabled && gf >= shot_from && gf <= shot_to && gf % shot_every == 0))
    return true;
  const char *list = getenv("AR_DIORAMA_DUMP_GF");
  for (const char *at = list; at && *at;) {
    if ((unsigned)strtoul(at, NULL, 0) == gf) return true;
    const char *comma = strchr(at, ',');
    at = comma ? comma + 1 : NULL;
  }
  return false;
}

void DevAutomation_CaptureScheduledScreenshot(const struct FrameSlot *uploaded_frame) {
  InitScreenshotSchedule();
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
          HostDevTools_WriteFramebufferPpmAtPhase(pf, require_composite, shot_phase, uploaded_frame);
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
      fprintf(stderr, "[shot-state] gf=%u room=%02x%02x bg1=%d,%d bg2=%d,%d phase=%.3f generated=%03x gpu=%03x\n",
              gf, g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap],
              (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX),
              (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY),
              (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraX),
              (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg2CameraY),
              (double)shot_phase, DioramaFrameGeneration_GeneratedPlaneMask(),
              DioramaFrameGeneration_GpuPlaneMask());
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
  const unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if (ScheduledActionDue(&warp, "AR_WARP_AT", gf)) {
    warp.fired = true;
    (void)RuntimeSettings_HandleAction(Settings_Find("warp_now"));
  }

  /* AR_DIORAMA_AT=<gameframe>: flip Diorama 3D on once the game-frame counter
   * reaches the value, through the same descriptor path the D hotkey uses.
   * Booting straight into diorama changes the widescreen margin budget and
   * changes the rendered baseline, so a visual-regression run should replay
   * flat into the stage and only then switch. Canonical input is host-tick
   * ordered; the game-frame value here is only the deterministic trigger. */
  if (ScheduledActionDue(&diorama, "AR_DIORAMA_AT", gf)) {
    diorama.fired = true;
    const SettingDesc *mode = Settings_Find("diorama_mode");
    if (mode && Settings_IsAvailable(mode) && !g_settings.diorama_mode) {
      Settings_SetLong(mode, 1);
      fprintf(stderr, "[diorama] ON via AR_DIORAMA_AT at gf=%u\n", gf);
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
