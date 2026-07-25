/* Host-side display policy and presentation cadence.
 *
 * SDL3 rendering remains synchronous on the SDL_Init/main thread. FrameSlot
 * is still the D6 boundary: this module may read live host settings to decide
 * when and how to present, while present.c receives only immutable slot data
 * and never reaches back into live game/PPU state.
 *
 * The retained frame and its previous-scroll snapshot are deliberately owned
 * together here. A between-ticks re-present must reuse that exact pair and the
 * shared deadline; capturing again would refresh the timestamp, collapse the
 * interpolation phase, and recreate the inert-render-clock bug R17 fixed. */
#include "host_display.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "diorama_scroll_math.h"
#include "frame_slot.h"
#include "host_display_pacing.h"
#include "present.h"
#include "present_cadence_metrics.h"
#include "settings.h"

extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern int g_snes_width;
extern int g_snes_height;
extern int g_ws_extra;

const uint64_t kHostDisplayEmulationFrameIntervalNs = 16639267ull;

enum {
  kDeadlineResyncIntervalCount = 2,
  kPerformanceReportIntervalMs = 1000,
};
static const uint64_t kDisplayPropertyPollIntervalNs = 1000000000ull;

typedef struct RetainedPresentFrame {
  FrameSlot slot;
  DioramaScrollSnapshot previous_scroll;
  bool valid;
} RetainedPresentFrame;

static DioramaScrollSnapshot s_previous_scroll;
static RetainedPresentFrame s_retained_frame;
static uint64_t s_present_deadline_ns;
static unsigned long s_tick_present_count;
static unsigned long s_represent_count;
static float s_maximum_represent_alpha;
static unsigned long s_no_present_no_sleep_iteration_count;

static bool RunningUnderGamescope(void) {
  static bool initialized;
  static bool running_under_gamescope;
  if (!initialized) {
    initialized = true;
    const char *display = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    running_under_gamescope = display && display[0];
    if (running_under_gamescope) {
      fprintf(stderr,
              "[display] gamescope detected — deferring frame pacing "
              "to the compositor (its refresh report is advisory)\n");
    }
  }
  return running_under_gamescope;
}

static HostDisplayPacingOptions CurrentPacingOptions(void) {
  return (HostDisplayPacingOptions){
      .refresh_mode = g_settings.refresh_mode,
      .frame_limit_fps = g_settings.frame_limit_fps,
      .host_refresh_hz = Settings_HostRefreshHz(),
      .compositor_managed = RunningUnderGamescope(),
  };
}

/* Advance an absolute deadline so scheduler oversleep is absorbed by the
 * following interval instead of permanently lowering the achieved rate. */
static void ThrottlePresent(uint64_t interval_ns) {
  uint64_t now_ns = SDL_GetTicksNS();
  if (!interval_ns) {
    s_present_deadline_ns = now_ns;
    return;
  }
  if (s_present_deadline_ns == 0 ||
      now_ns > s_present_deadline_ns +
                   kDeadlineResyncIntervalCount * interval_ns) {
    s_present_deadline_ns = now_ns + interval_ns;
    return;
  }
  if (now_ns < s_present_deadline_ns) {
    SDL_DelayNS(s_present_deadline_ns - now_ns);
    now_ns = SDL_GetTicksNS();
  }
  s_present_deadline_ns += interval_ns;
  if (s_present_deadline_ns < now_ns)
    s_present_deadline_ns = now_ns + interval_ns;
}

static uint64_t PresentIntervalNs(HostDisplayPresentMode mode) {
  const HostDisplayPacingOptions options = CurrentPacingOptions();
  switch (mode) {
    case kHostDisplayPresent_Menu:
      return HostDisplayPacing_UiIntervalNs(
          options, kHostDisplayEmulationFrameIntervalNs);
    case kHostDisplayPresent_Paused:
      return HostDisplayPacing_PausedIntervalNs(
          options, kHostDisplayEmulationFrameIntervalNs);
    case kHostDisplayPresent_GameTick:
    default:
      return HostDisplayPacing_GameIntervalNs(
          options, kHostDisplayEmulationFrameIntervalNs);
  }
}

void HostDisplay_ApplyWindowMode(void) {
  if (!g_window) return;
  switch (g_settings.window_mode) {
    case kWindowMode_Windowed:
      SDL_SetWindowFullscreen(g_window, false);
      break;
    case kWindowMode_Exclusive: {
      const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
      SDL_SetWindowFullscreenMode(
          g_window, SDL_GetDesktopDisplayMode(display));
      SDL_SetWindowFullscreen(g_window, true);
      break;
    }
    case kWindowMode_Borderless:
    default:
      SDL_SetWindowFullscreenMode(g_window, NULL);
      SDL_SetWindowFullscreen(g_window, true);
      break;
  }
}

static void UpdateRefreshRate(void) {
  if (!g_window) {
    Settings_SetHostRefreshHz(0);
    return;
  }
  const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
  if (!mode) mode = SDL_GetDesktopDisplayMode(display);
  Settings_SetHostRefreshHz(
      mode ? (int)(mode->refresh_rate + 0.5f) : 0);
}

static void UpdatePixelDensity(void) {
  if (!g_window) {
    Settings_SetHostPixelDensity(1.0f);
    return;
  }
  const float density = SDL_GetWindowPixelDensity(g_window);
  Settings_SetHostPixelDensity(density > 0.0f ? density : 1.0f);
}

void HostDisplay_UpdateProperties(void) {
  UpdateRefreshRate();
  UpdatePixelDensity();
}

void HostDisplay_PollProperties(void) {
  static uint64_t next_poll_ns;
  const uint64_t now_ns = SDL_GetTicksNS();
  if (now_ns < next_poll_ns) return;
  next_poll_ns = now_ns + kDisplayPropertyPollIntervalNs;
  UpdateRefreshRate();
}

void HostDisplay_ApplyRefreshVsync(void) {
  if (!g_renderer) return;
  const int requested =
      g_settings.refresh_mode == kRefreshMode_Vsync ? 1 : 0;
  if (!SDL_SetRenderVSync(g_renderer, requested)) {
    fprintf(stderr, "[display] SDL_SetRenderVSync(%d) rejected: %s\n",
            requested, SDL_GetError());
  }
  int actual = 0;
  Settings_SetHostVsyncActive(
      SDL_GetRenderVSync(g_renderer, &actual) && actual != 0);
}

uint64_t HostDisplay_CatchupCapNs(int maximum_catchup_frames) {
  return HostDisplayPacing_CatchupCapNs(
      CurrentPacingOptions(),
      kHostDisplayEmulationFrameIntervalNs,
      maximum_catchup_frames);
}

void HostDisplay_InvalidatePresentHistory(void) {
  memset(&s_previous_scroll, 0, sizeof(s_previous_scroll));
  s_retained_frame.valid = false;
}

static void ReportPresentPerformance(uint32_t render_start_ms,
                                     uint32_t vsync_start_ms) {
  const uint32_t now_ms = SDL_GetTicks();
  const uint32_t render_ms = vsync_start_ms - render_start_ms;
  const uint32_t vsync_ms = now_ms - vsync_start_ms;
  static uint32_t window_start_ms;
  static uint32_t render_sum_ms;
  static uint32_t render_max_ms;
  static uint32_t vsync_sum_ms;
  static uint32_t vsync_max_ms;
  static int window_frame_count;

  render_sum_ms += render_ms;
  if (render_ms > render_max_ms) render_max_ms = render_ms;
  vsync_sum_ms += vsync_ms;
  if (vsync_ms > vsync_max_ms) vsync_max_ms = vsync_ms;
  window_frame_count++;
  if (!window_start_ms) window_start_ms = now_ms;
  if (now_ms - window_start_ms < kPerformanceReportIntervalMs) return;

  fprintf(stderr,
          "[present-perf] frames=%d present-ms avg=%.1f max=%u "
          "vsync-wait avg=%.1f max=%u (no present thread)\n",
          window_frame_count,
          (double)render_sum_ms / window_frame_count,
          render_max_ms,
          (double)vsync_sum_ms / window_frame_count,
          vsync_max_ms);
  window_start_ms = now_ms;
  render_sum_ms = 0;
  render_max_ms = 0;
  vsync_sum_ms = 0;
  vsync_max_ms = 0;
  window_frame_count = 0;
}

bool HostDisplay_SubmitFrame(HostDisplayPresentMode mode, float alpha) {
  if (!g_renderer || !g_texture) return false;

  static bool performance_initialized;
  static bool performance_enabled;
  if (!performance_initialized) {
    performance_initialized = true;
    performance_enabled = getenv("AR_PERF") != NULL;
  }

  const bool game_tick = mode == kHostDisplayPresent_GameTick;
  FrameSlot slot;
  FrameSlot_Capture(&slot);
  const uint32_t render_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  PresentUpload(&slot);

  /* Preserve the previous-tick pairing before extraction advances the live
   * snapshot. Re-presenting must reuse this retained pair and must never call
   * FrameSlot_Capture, whose fresh timestamp would collapse interpolation. */
  if (game_tick) {
    s_retained_frame.previous_scroll = s_previous_scroll;
    s_retained_frame.slot = slot;
    s_retained_frame.valid = true;
  }
  PresentComposite(
      &slot,
      game_tick ? &s_previous_scroll : NULL,
      game_tick ? alpha : kInterpPhaseNone);
  if (game_tick)
    FrameSlot_ExtractScrollSnapshot(&slot, &s_previous_scroll);

  const uint32_t vsync_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  ThrottlePresent(PresentIntervalNs(mode));
  SDL_RenderPresent(g_renderer);
  if (game_tick) s_tick_present_count++;
  if (performance_enabled)
    ReportPresentPerformance(render_start_ms, vsync_start_ms);
  return true;
}

bool HostDisplay_TryRepresentFrame(float alpha,
                                   bool diorama_frame_active,
                                   bool interpolation_enabled,
                                   bool redraw_pending) {
  if (!s_retained_frame.valid ||
      !diorama_frame_active ||
      !interpolation_enabled ||
      redraw_pending ||
      !DioramaScrollPairIsInterpolable(
          &s_retained_frame.slot,
          &s_retained_frame.previous_scroll) ||
      !g_renderer ||
      !g_texture) {
    return false;
  }

  SDL_assert(
      s_retained_frame.previous_scroll.timestamp_ns <
      s_retained_frame.slot.timestamp_ns);
  SDL_assert(s_retained_frame.slot.diorama_active);
  SDL_assert(alpha >= 0.0f && alpha < 1.0f);
  if (s_retained_frame.slot.snes_width != g_snes_width ||
      s_retained_frame.slot.snes_height != g_snes_height ||
      s_retained_frame.slot.ws_extra != g_ws_extra) {
    SDL_assert(!"retained slot geometry disagrees with live geometry");
    s_retained_frame.valid = false;
    return false;
  }

  PresentComposite(
      &s_retained_frame.slot,
      &s_retained_frame.previous_scroll,
      alpha);
  ThrottlePresent(PresentIntervalNs(kHostDisplayPresent_GameTick));
  SDL_RenderPresent(g_renderer);
  s_represent_count++;
  if (alpha > s_maximum_represent_alpha)
    s_maximum_represent_alpha = alpha;
  return true;
}

void HostDisplay_YieldIfNoPresent(bool presented,
                                  bool window_hidden,
                                  bool produced_frame) {
  if (presented) return;
  if (!window_hidden && produced_frame)
    s_no_present_no_sleep_iteration_count++;
  SDL_Delay(1);
}

PresentCadenceMetrics PresentCadence_GetMetrics(void) {
  return (PresentCadenceMetrics){
      .tick_present_count = s_tick_present_count,
      .represent_count = s_represent_count,
      .maximum_represent_alpha = s_maximum_represent_alpha,
      .no_present_no_sleep_iteration_count =
          s_no_present_no_sleep_iteration_count,
  };
}
