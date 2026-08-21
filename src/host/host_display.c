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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "diorama/diorama_scroll_math.h"
#include "frame_slot.h"
#include "host_display_pacing.h"
#include "present.h"
#include "presentation_geometry.h"
#include "present_cadence_metrics.h"
#include "settings.h"
#include "snes/ppu.h"
#include "constants.h"
#include "widescreen.h"

extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern int g_snes_width;
extern int g_snes_height;
extern bool g_new_ppu;
extern bool g_ws_active;
extern int g_ws_extra;
extern int g_ws_display_extra;
extern uint8_t g_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_bg_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_obj_pixels[
    kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight];

const uint64_t kHostDisplayEmulationFrameIntervalNs = 16639267ull;
int g_active_pixel_aspect = kPixelAspect_Crt43;

enum {
  kDeadlineResyncIntervalCount = 2,
  kPerformanceReportIntervalMs = 1000,
};
static const uint64_t kDisplayPropertyPollIntervalNs =
    kNanosecondsPerSecond;

typedef struct RetainedPresentFrame {
  FrameSlot slot;
  DioramaScrollSnapshot previous_scroll;
  ActionObjInterpolationFrame previous_action_obj;
  bool valid;
} RetainedPresentFrame;

static DioramaScrollSnapshot s_previous_scroll;
static ActionObjInterpolationFrame s_previous_action_obj;
static RetainedPresentFrame s_retained_frame;
static uint64_t s_present_deadline_ns;
static unsigned long s_tick_present_count;
static unsigned long s_represent_count;
static float s_maximum_represent_alpha;
static unsigned long s_no_present_no_sleep_iteration_count;
static HostDisplayFpsCounter s_fps_counter;
static bool s_fps_measurement_active;
static int s_fps_refresh_mode = -1;
static HostDisplayPresentMode s_fps_present_mode = kHostDisplayPresent_None;
static bool s_present_failure_reported;
static int s_active_aspect_x;
static int s_active_aspect_y;
static bool s_widescreen_runtime_allowed;

/* Refresh only the presentation-owned camera portion of a retained SIM frame.
 * The captured game/PPU snapshot, timestamp, interpolation pair, textures, and
 * object metadata remain untouched. This is what lets an uncapped re-present
 * show current mouse orbit rather than either drawing a stale pose or waiting
 * for the next emulation tick. */
static void RefreshRetainedSimCamera(FrameSlot *slot) {
  if (!slot || slot->sim.view != kSimView_Enhanced) return;
  Sim3DCameraPresentationState camera;
  Sim3DCamera_CapturePresentationState(&camera);
  slot->sim.projection_pitch_mrad = (int16_t)camera.pitch_mrad;
  slot->sim.projection_yaw_mrad = (int16_t)camera.yaw_mrad;
  slot->sim.projection_distance_x100 = (uint16_t)camera.distance_x100;
  slot->sim_camera_mode = camera.mode;
  slot->sim_manual_orbit_yaw = camera.orbit_yaw;
  slot->sim_manual_orbit_pitch = camera.orbit_pitch;
}

double HostDisplay_FramesPerSecond(void) {
  return s_fps_measurement_active
      ? HostDisplayPacing_FramesPerSecond(&s_fps_counter)
      : 0.0;
}

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

bool HostDisplay_WindowPointToOutput(int window_x, int window_y,
                                    int *output_x, int *output_y) {
  if (!g_window || !g_renderer) return false;

  int window_width = 0;
  int window_height = 0;
  int output_width = 0;
  int output_height = 0;
  SDL_GetWindowSize(g_window, &window_width, &window_height);
  if (!SDL_GetRenderOutputSize(
          g_renderer, &output_width, &output_height) ||
      window_width <= 0 || window_height <= 0 ||
      output_width <= 0 || output_height <= 0)
    return false;

  /* SDL3 reports mouse coordinates relative to the window (SDL_events.h
   * documents the ORIGIN; it does not state the unit, so treating them as
   * window-client coordinates and rescaling is the conservative reading).
   * Convert to renderer-output pixels; this covers a high-DPI backing scale in
   * one direction and a reduced render resolution in the other.
   *
   * W4-5: the arithmetic lives in host_display_pacing.c so its edge behaviour is
   * unit-tested — round-to-nearest alone overshoots by one pixel on the final
   * row/column when the output is half the window or smaller. */
  if (output_x)
    *output_x = HostDisplayPacing_WindowAxisToOutput(
        window_x, window_width, output_width);
  if (output_y)
    *output_y = HostDisplayPacing_WindowAxisToOutput(
        window_y, window_height, output_height);
  return true;
}

static HostDisplayPacingOptions CurrentPacingOptions(void) {
  return (HostDisplayPacingOptions){
      .refresh_mode = (RefreshMode)g_settings.refresh_mode,
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
    case kHostDisplayPresent_None:
    case kHostDisplayPresent_HeadlessVideo:
      return 0;
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

/* The two frame-production paths must agree on what a completed present is.
 * In particular, a rejected SDL_RenderPresent is neither an FPS sample nor a
 * reason for the outer loop to skip its anti-spin yield. Sampling is dormant
 * while the overlay is hidden, and restarts across cadence changes so the
 * first visible result cannot mix menu/paused/old-refresh timing. */
static bool CompletePresent(HostDisplayPresentMode mode) {
  ThrottlePresent(PresentIntervalNs(mode));
  if (!SDL_RenderPresent(g_renderer)) {
    if (!s_present_failure_reported) {
      fprintf(stderr, "[display] SDL_RenderPresent failed: %s\n",
              SDL_GetError());
      s_present_failure_reported = true;
    }
    return false;
  }
  s_present_failure_reported = false;

  if (!g_settings.show_fps) {
    if (s_fps_measurement_active) {
      HostDisplayPacing_ResetFpsCounter(&s_fps_counter);
      s_fps_measurement_active = false;
    }
    return true;
  }

  if (!s_fps_measurement_active ||
      s_fps_refresh_mode != g_settings.refresh_mode ||
      s_fps_present_mode != mode) {
    HostDisplayPacing_ResetFpsCounter(&s_fps_counter);
    s_fps_measurement_active = true;
    s_fps_refresh_mode = g_settings.refresh_mode;
    s_fps_present_mode = mode;
  }
  HostDisplayPacing_RecordPresent(&s_fps_counter, SDL_GetTicksNS());
  return true;
}

void HostDisplay_SetWidescreenRuntimeAllowed(bool allowed) {
  s_widescreen_runtime_allowed = allowed;
}

static int WindowScaleInPoints(int scale) {
  const float density = Settings_HostPixelDensity();
  if (density <= 1.0f) return scale;
  const int points = (int)((float)scale / density + 0.5f);
  return points > 0 ? points : 1;
}

void HostDisplay_CalculateWindowSize(int scale, int *width, int *height) {
  const int window_height = g_snes_height * scale;
  int window_width = Settings_VisibleWidth() * scale;

  if (g_settings.display_mode == kDisplayMode_43) {
    if (g_active_pixel_aspect == kPixelAspect_Crt43)
      window_width = (window_height * 4 + 1) / 3;
  } else if (g_ws_active &&
             g_active_pixel_aspect == kPixelAspect_Crt43) {
    window_width =
        (window_height * s_active_aspect_x + s_active_aspect_y / 2) /
        s_active_aspect_y;
  }

  if (width) *width = window_width;
  if (height) *height = window_height;
}

void HostDisplay_RecomputeLogicalPresentation(void) {
  if (!g_window || !g_renderer) return;
  PresentationGeometry_ApplyLogical(
      g_renderer, Settings_IgnoreAspectRatio(),
      g_active_pixel_aspect == kPixelAspect_Crt43,
      Settings_VisibleWidth(), g_snes_height);
}

void HostDisplay_ApplyWindowScale(void) {
  if (!g_window || !g_renderer) return;
  const int configured_scale =
      g_settings.window_scale ? g_settings.window_scale : 3;
  const int point_scale = WindowScaleInPoints(configured_scale);
  int window_width;
  int window_height;
  HostDisplay_CalculateWindowSize(
      point_scale, &window_width, &window_height);

  HostDisplay_RecomputeLogicalPresentation();
  if (g_settings.window_mode == kWindowMode_Windowed)
    SDL_SetWindowSize(g_window, window_width, window_height);
}

void HostDisplay_ResolveVideoGeometry(bool apply_runtime_changes) {
  const int previous_display_mode = g_settings.display_mode;
  s_active_aspect_x = Settings_ExtendedAspectX();
  s_active_aspect_y = Settings_ExtendedAspectY();
  g_active_pixel_aspect = g_settings.pixel_aspect;

  int extra_columns = 0;
  if (s_widescreen_runtime_allowed &&
      s_active_aspect_x &&
      s_active_aspect_y) {
    const bool crt_pixel_aspect =
        g_active_pixel_aspect == kPixelAspect_Crt43;
    const long numerator =
        (long)g_snes_height * s_active_aspect_x *
        (crt_pixel_aspect ? 6 : 7);
    const long denominator = 7L * s_active_aspect_y;
    const int internal_width =
        (int)((numerator + denominator - 1) / denominator);
    extra_columns =
        internal_width > kActRaiserAuthenticWidth
            ? (internal_width - kActRaiserAuthenticWidth + 1) / 2
            : 0;
    if (extra_columns > kWsExtraMax) extra_columns = kWsExtraMax;
  }

  g_ws_display_extra = extra_columns;
  if (g_settings.diorama_mode && extra_columns > 0)
    extra_columns = kWsExtraMax;

  g_ws_extra = extra_columns;
  g_ws_active = extra_columns > 0;
  g_snes_width =
      kActRaiserAuthenticWidth + 2 * extra_columns;
  g_new_ppu = g_settings.new_renderer || g_ws_active;

  if (apply_runtime_changes) {
    /* Aspect/PAR changes alter the framebuffer budget, not the selected HLE
     * correction profile. */
    Settings_ReconcileDisplayModeAfterGeometryChange(previous_display_mode);
    memset(g_pixels, 0, sizeof(g_pixels));
    memset(g_hud_bg_pixels, 0, sizeof(g_hud_bg_pixels));
    memset(g_hud_obj_pixels, 0, sizeof(g_hud_obj_pixels));
    ActRaiser_RebindPpuOutputSurfaces();
    HostDisplay_ApplyWindowScale();
    HostDisplay_InvalidatePresentHistory();
  }

  fprintf(stderr,
          "[video-geometry] %s %s -> %d extra columns/side "
          "(render width %d, %s PPU)\n",
          s_active_aspect_x
              ? (s_active_aspect_y == 9 ? "16:9" : "16:10")
              : "4:3",
          g_active_pixel_aspect == kPixelAspect_Crt43
              ? "4:3-PAR"
              : "square-PAR",
          g_ws_extra,
          g_snes_width,
          g_new_ppu ? "new" : "legacy");
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

static void SetRenderVsync(int requested) {
  if (!g_renderer) return;
  if (!SDL_SetRenderVSync(g_renderer, requested)) {
    fprintf(stderr, "[display] SDL_SetRenderVSync(%d) rejected: %s\n",
            requested, SDL_GetError());
  }
  int actual = 0;
  Settings_SetHostVsyncActive(
      SDL_GetRenderVSync(g_renderer, &actual) && actual != 0);
}

void HostDisplay_ApplyRefreshVsync(void) {
  SetRenderVsync(g_settings.refresh_mode == kRefreshMode_Vsync ? 1 : 0);
}

void HostDisplay_DisableVsync(void) {
  SetRenderVsync(0);
}

uint64_t HostDisplay_CatchupCapNs(int maximum_catchup_frames) {
  return HostDisplayPacing_CatchupCapNs(
      CurrentPacingOptions(),
      kHostDisplayEmulationFrameIntervalNs,
      maximum_catchup_frames);
}

void HostDisplay_InvalidatePresentHistory(void) {
  memset(&s_previous_scroll, 0, sizeof(s_previous_scroll));
  memset(&s_previous_action_obj, 0, sizeof(s_previous_action_obj));
  s_retained_frame.valid = false;
}

static void ReportPresentPerformance(uint64_t render_start_ms,
                                     uint64_t vsync_start_ms) {
  const uint64_t now_ms = SDL_GetTicks();
  const uint64_t render_ms = vsync_start_ms - render_start_ms;
  const uint64_t vsync_ms = now_ms - vsync_start_ms;
  static uint64_t window_start_ms;
  static uint64_t render_sum_ms;
  static uint64_t render_max_ms;
  static uint64_t vsync_sum_ms;
  static uint64_t vsync_max_ms;
  static int window_frame_count;

  render_sum_ms += render_ms;
  if (render_ms > render_max_ms) render_max_ms = render_ms;
  vsync_sum_ms += vsync_ms;
  if (vsync_ms > vsync_max_ms) vsync_max_ms = vsync_ms;
  window_frame_count++;
  if (!window_start_ms) window_start_ms = now_ms;
  if (now_ms - window_start_ms < kPerformanceReportIntervalMs) return;

  fprintf(stderr,
          "[present-perf] frames=%d present-ms avg=%.1f max=%" PRIu64 " "
          "vsync-wait avg=%.1f max=%" PRIu64 " (no present thread)\n",
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
  if (mode == kHostDisplayPresent_None || !g_renderer || !g_texture)
    return false;

  static bool performance_initialized;
  static bool performance_enabled;
  if (!performance_initialized) {
    performance_initialized = true;
    performance_enabled = getenv("AR_PERF") != NULL;
  }

  const bool game_tick = mode == kHostDisplayPresent_GameTick ||
                         mode == kHostDisplayPresent_HeadlessVideo;
  FrameSlot slot;
  FrameSlot_Capture(&slot);
  const uint64_t render_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  PresentUpload(&slot);

  /* Preserve the previous-tick pairing before extraction advances the live
   * snapshot. Re-presenting must reuse this retained pair and must never call
   * FrameSlot_Capture, whose fresh timestamp would collapse interpolation. */
  if (game_tick) {
    s_retained_frame.previous_scroll = s_previous_scroll;
    s_retained_frame.previous_action_obj = s_previous_action_obj;
    s_retained_frame.slot = slot;
    s_retained_frame.valid = true;
  }
  PresentFrame(
      &slot,
      game_tick ? &s_previous_scroll : NULL,
      game_tick ? &s_previous_action_obj : NULL,
      game_tick ? alpha : kInterpPhaseNone,
      HostDisplay_FramesPerSecond());
  if (game_tick) {
    FrameSlot_ExtractScrollSnapshot(&slot, &s_previous_scroll);
    s_previous_action_obj = slot.action_obj_interpolation;
  }

  const uint64_t vsync_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  const bool presented = CompletePresent(mode);
  if (presented && game_tick) s_tick_present_count++;
  if (presented && performance_enabled)
    ReportPresentPerformance(render_start_ms, vsync_start_ms);
  return presented;
}

bool HostDisplay_TryRepresentFrame(float alpha,
                                   bool diorama_frame_active,
                                   bool interpolation_enabled,
                                   bool redraw_pending) {
  const bool uncapped_profile =
      g_settings.refresh_mode == kRefreshMode_Uncapped;
  const bool pair_interpolable = s_retained_frame.valid &&
      DioramaScrollPairIsInterpolable(
          &s_retained_frame.slot, &s_retained_frame.previous_scroll);
  if (!s_retained_frame.valid || !g_renderer || !g_texture ||
      !HostDisplayPacing_ShouldRepresentFrame(
          (RefreshMode)g_settings.refresh_mode, diorama_frame_active,
          interpolation_enabled, pair_interpolable, redraw_pending)) {
    return false;
  }

  if (!uncapped_profile) {
    SDL_assert(
        s_retained_frame.previous_scroll.timestamp_ns <
        s_retained_frame.slot.timestamp_ns);
    SDL_assert(s_retained_frame.slot.diorama_active);
    SDL_assert(alpha >= 0.0f && alpha < 1.0f);
  }
  if (s_retained_frame.slot.snes_width != g_snes_width ||
      s_retained_frame.slot.snes_height != g_snes_height ||
      s_retained_frame.slot.ws_extra != g_ws_extra) {
    SDL_assert(false &&
               "retained slot geometry disagrees with live geometry");
    s_retained_frame.valid = false;
    return false;
  }

  if (uncapped_profile)
    RefreshRetainedSimCamera(&s_retained_frame.slot);

  PresentFrame(
      &s_retained_frame.slot,
      uncapped_profile ? NULL : &s_retained_frame.previous_scroll,
      uncapped_profile ? NULL : &s_retained_frame.previous_action_obj,
      uncapped_profile ? kInterpPhaseNone : alpha,
      HostDisplay_FramesPerSecond());
  if (!CompletePresent(kHostDisplayPresent_GameTick)) return false;
  s_represent_count++;
  if (!uncapped_profile && alpha > s_maximum_represent_alpha)
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
