/* Host-side display policy and presentation cadence.
 *
 * SDL3 rendering remains synchronous on the SDL_Init/main thread. FrameSlot
 * is still the D6 boundary: this module may read live host settings to decide
 * when and how to present, while present.c receives only immutable slot data
 * and never reaches back into live game/PPU state.
 *
 * A between-ticks re-present reuses the exact captured frame and shared
 * deadline; capturing again would refresh its timestamp and break pair
 * continuity. */
#include "host_display.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "frame_slot.h"
#include "host_display_pacing.h"
#include "host_display_refresh_cache.h"
#include "host_display_status.h"
#include "present.h"
#include "presentation_frame_generation.h"
#include "platform/sdl/presentation_device_sdl.h"
#include "platform/sdl/presentation_geometry_sdl.h"
#include "platform/sdl/render_sdl.h"
#include "present_cadence_metrics.h"
#include "snesrecomp/runner.h"
#include "settings.h"
#include "constants.h"
#include "snesrecomp/host/widescreen.h"
#include "render/render_device.h"

extern SDL_Window *g_window;
extern ArRenderDevice g_render_device;
extern ArRenderTexture g_texture;
extern int g_snes_width;
extern int g_snes_height;
extern bool g_ws_active;
extern int g_ws_extra;
extern int g_ws_display_extra;
extern uint8_t g_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
extern uint8_t g_authentic_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_bg_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];
extern uint8_t g_hud_obj_pixels[
    SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight];

const uint64_t kHostDisplayEmulationFrameIntervalNs = 16639267ull;
int g_active_pixel_aspect = kPixelAspect_Crt43;

enum {
  kDeadlineResyncIntervalCount = 2,
  kPerformanceReportIntervalMs = 1000,
};

typedef struct RetainedPresentFrame {
  FrameSlot slot;
  bool valid;
} RetainedPresentFrame;

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
static HostDisplayRefreshCache s_display_refresh_cache;
static SDL_DisplayID s_active_display_id;

/* Refresh only the presentation-owned camera portion of a retained SIM frame.
 * The captured game/PPU snapshot, timestamp, interpolation pair, textures, and
 * object metadata remain untouched. This lets a retained re-present show the
 * current mouse orbit rather than either drawing a stale pose or waiting for
 * the next emulation tick. */
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

/* Action's camera is presentation-owned for the same reason as SIM's. Only
 * these host fields are refreshed: velocity lean, hit/landing impulses, and
 * every game/PPU field stay paired with the retained tick that captured them. */
static void RefreshRetainedDioramaCamera(FrameSlot *slot) {
  if (!slot || !slot->diorama_active) return;
  DioramaCameraPresentationState camera;
  Diorama_CaptureCameraPresentationState(&camera);
  slot->diorama_camera_mode = camera.mode;
  slot->diorama_free_pose = camera.free_pose;
  slot->diorama_dyncam_baseline = camera.dynamic_baseline;
  slot->diorama_manual_orbit_yaw = camera.orbit_yaw;
  slot->diorama_manual_orbit_pitch = camera.orbit_pitch;
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
  if (!g_window || !ArRenderDevice_IsReady(&g_render_device)) return false;

  int window_width = 0;
  int window_height = 0;
  int output_width = 0;
  int output_height = 0;
  SDL_GetWindowSize(g_window, &window_width, &window_height);
  if (!ArRenderDevice_GetOutputSize(
          &g_render_device, &output_width, &output_height) ||
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
      .nominal_refresh_hz = HostDisplayStatus_NominalRefreshHz(),
      .compositor_managed = RunningUnderGamescope(),
      .vsync_active = HostDisplayStatus_VsyncActive(),
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
 * In particular, a rejected backend present is neither an FPS sample nor a
 * reason for the outer loop to skip its anti-spin yield. Sampling is dormant
 * while the overlay is hidden, and restarts across cadence changes so the
 * first visible result cannot mix menu/paused/old-refresh timing. */
static bool CompletePresent(HostDisplayPresentMode mode) {
  ThrottlePresent(PresentIntervalNs(mode));
  if (!ArRenderDevice_Present(&g_render_device)) {
    if (!s_present_failure_reported) {
      fprintf(stderr, "[display] frame present failed: %s\n",
              ArRenderDevice_LastError(&g_render_device));
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
  if (!g_window || !ArRenderDevice_IsReady(&g_render_device)) return;
  ArSdlPresentationDevice_ApplyLogical(
      &g_render_device, Settings_IgnoreAspectRatio(),
      g_active_pixel_aspect == kPixelAspect_Crt43,
      Settings_VisibleWidth(), g_snes_height);
}

void HostDisplay_ApplyWindowScale(void) {
  if (!g_window || !ArRenderDevice_IsReady(&g_render_device)) return;
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

  if (apply_runtime_changes) {
    /* Aspect/PAR changes alter the framebuffer budget, not the selected HLE
     * correction profile. */
    Settings_ReconcileDisplayModeAfterGeometryChange(previous_display_mode);
    memset(g_pixels, 0, sizeof(g_pixels));
    memset(g_authentic_pixels, 0, sizeof(g_authentic_pixels));
    memset(g_hud_bg_pixels, 0, sizeof(g_hud_bg_pixels));
    memset(g_hud_obj_pixels, 0, sizeof(g_hud_obj_pixels));
    ActRaiser_RebindPpuOutputSurfaces();
    HostDisplay_ApplyWindowScale();
    HostDisplay_InvalidatePresentHistory();
  }

  fprintf(stderr,
          "[video-geometry] %s %s -> %d extra columns/side "
          "(render width %d)\n",
          s_active_aspect_x
              ? (s_active_aspect_y == 9 ? "16:9" : "16:10")
              : "4:3",
          g_active_pixel_aspect == kPixelAspect_Crt43
              ? "4:3-PAR"
              : "square-PAR",
          g_ws_extra,
          g_snes_width);
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

static int DisplayModeRefreshHz(const SDL_DisplayMode *mode) {
  if (!mode) return 0;
  if (mode->refresh_rate_numerator > 0 &&
      mode->refresh_rate_denominator > 0) {
    const uint64_t numerator = (uint64_t)mode->refresh_rate_numerator;
    const uint64_t denominator =
        (uint64_t)mode->refresh_rate_denominator;
    return (int)((numerator + denominator / 2u) / denominator);
  }
  return mode->refresh_rate > 0.0f
      ? (int)(mode->refresh_rate + 0.5f)
      : 0;
}

static int QueryDisplayRefreshHz(SDL_DisplayID display_id) {
  if (!display_id) return 0;
  const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display_id);
  int refresh_hz = DisplayModeRefreshHz(mode);
  if (refresh_hz <= 0)
    refresh_hz = DisplayModeRefreshHz(
        SDL_GetDesktopDisplayMode(display_id));
  return refresh_hz;
}

/* Select the window's session-stable display ID and publish its last valid
 * nominal rate. A failed/unspecified query never destroys a cached value.
 * `force_query` is reserved for boot and actual display-mode events; ordinary
 * mode transitions reuse the per-display cache. */
static void UpdateRefreshRate(bool force_query) {
  if (!g_window) {
    s_active_display_id = 0;
    HostDisplayStatus_SetNominalRefreshHz(0);
    return;
  }
  const SDL_DisplayID display_id = SDL_GetDisplayForWindow(g_window);
  if (!display_id) return;  /* Transient SDL failure: preserve known state. */

  s_active_display_id = display_id;
  int refresh_hz = HostDisplayRefreshCache_Get(
      &s_display_refresh_cache, display_id);
  if (force_query || refresh_hz <= 0) {
    const int queried_refresh_hz = QueryDisplayRefreshHz(display_id);
    HostDisplayRefreshCache_Remember(
        &s_display_refresh_cache, display_id, queried_refresh_hz);
  }
  refresh_hz = HostDisplayRefreshCache_Get(
      &s_display_refresh_cache, display_id);
  HostDisplayStatus_SetNominalRefreshHz(refresh_hz);
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
  UpdateRefreshRate(true);
  UpdatePixelDensity();
}

void HostDisplay_WindowDisplayChanged(void) {
  UpdateRefreshRate(false);
  UpdatePixelDensity();
}

void HostDisplay_WindowDisplayScaleChanged(void) {
  UpdatePixelDensity();
}

void HostDisplay_DisplayModeChanged(uint32_t display_id) {
  const SDL_DisplayID id = (SDL_DisplayID)display_id;
  const int refresh_hz = QueryDisplayRefreshHz(id);
  HostDisplayRefreshCache_Remember(&s_display_refresh_cache, id, refresh_hz);
  if (id == s_active_display_id) {
    HostDisplayStatus_SetNominalRefreshHz(
        HostDisplayRefreshCache_Get(&s_display_refresh_cache, id));
    UpdatePixelDensity();
  }
}

void HostDisplay_DisplayRemoved(uint32_t display_id) {
  const SDL_DisplayID id = (SDL_DisplayID)display_id;
  HostDisplayRefreshCache_Forget(&s_display_refresh_cache, id);
  if (id != s_active_display_id) return;
  s_active_display_id = 0;
  HostDisplayStatus_SetNominalRefreshHz(0);
  /* SDL may already have reassigned the window to another connected display.
   * Select its cached value immediately if so; its window-change event remains
   * the authoritative follow-up. */
  UpdateRefreshRate(false);
  UpdatePixelDensity();
}

static void SetRenderVsync(int requested) {
  if (!ArRenderDevice_IsReady(&g_render_device)) return;
  bool active = false;
  if (!ArSdlRenderBackend_SetVSync(
          &g_render_device, requested, &active)) {
    fprintf(stderr, "[display] vsync request %d rejected: %s\n",
            requested, SDL_GetError());
  }
  HostDisplayStatus_SetVsyncActive(active);
}

static void SetAllowedFramesInFlight(uint32_t requested) {
  if (!ArRenderDevice_IsReady(&g_render_device)) return;
  bool changed = false;
  if (!ArSdlRenderBackend_SetAllowedFramesInFlight(
          &g_render_device, requested, &changed)) {
    fprintf(stderr,
            "[display] SDL_SetGPUAllowedFramesInFlight(%" PRIu32
            ") rejected: %s\n",
            requested, SDL_GetError());
    return;
  }
  if (!changed) return;
  fprintf(stderr, "[display] GPU frames in flight: %" PRIu32 "\n",
          requested);
}

void HostDisplay_ApplyRefreshVsync(void) {
  const RefreshMode mode = (RefreshMode)g_settings.refresh_mode;
  SetRenderVsync(mode == kRefreshMode_Vsync ? 1 : 0);
  SetAllowedFramesInFlight(
      HostDisplayPacing_AllowedFramesInFlight(mode));
}

void HostDisplay_DisableVsync(void) {
  SetRenderVsync(0);
  SetAllowedFramesInFlight(
      HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Unlimited));
}

uint64_t HostDisplay_CatchupCapNs(uint64_t emulation_frame_interval_ns,
                                  int maximum_catchup_frames) {
  return HostDisplayPacing_CatchupCapNs(
      CurrentPacingOptions(),
      emulation_frame_interval_ns,
      maximum_catchup_frames);
}

void HostDisplay_InvalidatePresentHistory(void) {
  s_retained_frame.valid = false;
  DioramaFrameGeneration_Reset();
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

static bool PresentPerformanceEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("AR_PERF") ? 1 : 0;
  return enabled != 0;
}

bool HostDisplay_SubmitFrame(HostDisplayPresentMode mode, float alpha) {
  if (mode == kHostDisplayPresent_None ||
      !ArRenderDevice_IsReady(&g_render_device) ||
      !ArRenderTexture_IsValid(g_texture))
    return false;

  const bool performance_enabled = PresentPerformanceEnabled();

  const bool game_tick = mode == kHostDisplayPresent_GameTick ||
                         mode == kHostDisplayPresent_HeadlessVideo;
  FrameSlot slot;
  FrameSlot_Capture(&slot);
  const uint64_t render_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  PresentUpload(&slot);

  if (game_tick) {
    s_retained_frame.slot = slot;
    s_retained_frame.valid = true;
  }
  PresentFrame(&slot,
               game_tick ? alpha : kPresentationFrameGenerationPhaseNone,
               HostDisplay_FramesPerSecond());

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
  const bool use_interpolation =
      diorama_frame_active && interpolation_enabled;
  if (!s_retained_frame.valid ||
      !ArRenderDevice_IsReady(&g_render_device) ||
      !ArRenderTexture_IsValid(g_texture) ||
      !HostDisplayPacing_ShouldRepresentFrame(
          (RefreshMode)g_settings.refresh_mode, redraw_pending)) {
    return false;
  }

  if (use_interpolation) {
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

  const bool performance_enabled = PresentPerformanceEnabled();
  const uint64_t render_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  RefreshRetainedDioramaCamera(&s_retained_frame.slot);
  RefreshRetainedSimCamera(&s_retained_frame.slot);

  PresentFrame(&s_retained_frame.slot,
               use_interpolation
                   ? alpha : kPresentationFrameGenerationPhaseNone,
               HostDisplay_FramesPerSecond());
  const uint64_t vsync_start_ms =
      performance_enabled ? SDL_GetTicks() : 0;
  if (!CompletePresent(kHostDisplayPresent_GameTick)) return false;
  s_represent_count++;
  if (use_interpolation && alpha > s_maximum_represent_alpha)
    s_maximum_represent_alpha = alpha;
  if (performance_enabled)
    ReportPresentPerformance(render_start_ms, vsync_start_ms);
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
