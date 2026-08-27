#include "host_display_pacing.h"

#include <stdint.h>

#include "constants.h"

static const uint64_t kFpsSampleIntervalNs = kNanosecondsPerSecond / 2u;

uint64_t HostDisplayPacing_SourceFrameIntervalNs(
    uint64_t native_interval_ns, bool diorama_active,
    bool interpolation_enabled, InterpolationSourceRate source_rate) {
  if (diorama_active && interpolation_enabled &&
      source_rate == kInterpolationSource_Test30 &&
      native_interval_ns <= UINT64_MAX / 2u)
    return native_interval_ns * 2u;
  return native_interval_ns;
}

void HostDisplayPacing_ResetFpsCounter(HostDisplayFpsCounter *counter) {
  if (counter) *counter = (HostDisplayFpsCounter){0};
}

void HostDisplayPacing_RecordPresent(
    HostDisplayFpsCounter *counter, uint64_t completed_at_ns) {
  if (!counter) return;
  if (!counter->initialized || completed_at_ns <= counter->sample_start_ns) {
    counter->sample_start_ns = completed_at_ns;
    counter->completed_intervals = 0;
    counter->initialized = true;
    return;
  }

  counter->completed_intervals++;
  const uint64_t elapsed_ns = completed_at_ns - counter->sample_start_ns;
  if (elapsed_ns < kFpsSampleIntervalNs) return;
  counter->frames_per_second =
      (double)counter->completed_intervals * (double)kNanosecondsPerSecond /
      (double)elapsed_ns;
  counter->sample_start_ns = completed_at_ns;
  counter->completed_intervals = 0;
}

double HostDisplayPacing_FramesPerSecond(
    const HostDisplayFpsCounter *counter) {
  return counter ? counter->frames_per_second : 0.0;
}

uint32_t HostDisplayPacing_AllowedFramesInFlight(RefreshMode refresh_mode) {
  switch (refresh_mode) {
    case kRefreshMode_Uncapped:
    case kRefreshMode_Limit:
    case kRefreshMode_Unlimited:
      return 2;
    case kRefreshMode_Vsync:
    case kRefreshMode_Count:
    default:
      return 1;
  }
}

uint64_t HostDisplayPacing_FrameLimitIntervalNs(
    HostDisplayPacingOptions options) {
  if (options.refresh_mode != kRefreshMode_Limit) return 0;
  const int frames_per_second =
      options.frame_limit_fps > 0 ? options.frame_limit_fps : 1;
  return kNanosecondsPerSecond / (uint64_t)frames_per_second;
}

uint64_t HostDisplayPacing_UiIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  switch (options.refresh_mode) {
    case kRefreshMode_Vsync:
      /* A working renderer VSync is the presentation clock. Do not put a
       * nominal-refresh sleep in front of SDL_RenderPresent: it adds jitter,
       * breaks VRR, and duplicates the swapchain's responsibility. If the
       * backend rejected VSync, retain a fixed native-rate safety yield. */
      return options.vsync_active ? 0 : emulation_frame_interval_ns;
    case kRefreshMode_Uncapped:
      /* Uncapped is the renamed, display-relative policy. Nominal refresh is
       * optional presentation metadata; unknown/VRR/compositor sessions use
       * a deterministic 2x-native fallback without touching emulation time. */
      if (!options.compositor_managed && options.nominal_refresh_hz > 0)
        return kNanosecondsPerSecond /
            ((uint64_t)options.nominal_refresh_hz * 2u);
      return emulation_frame_interval_ns / 2u;
    case kRefreshMode_Limit:
      return HostDisplayPacing_FrameLimitIntervalNs(options);
    case kRefreshMode_Unlimited:
      return 0;
    case kRefreshMode_Count:
    default:
      return emulation_frame_interval_ns;
  }
}

uint64_t HostDisplayPacing_PausedIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  return HostDisplayPacing_UiIntervalNs(
      options, emulation_frame_interval_ns);
}

uint64_t HostDisplayPacing_GameIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  return HostDisplayPacing_UiIntervalNs(
      options, emulation_frame_interval_ns);
}

bool HostDisplayPacing_ShouldRepresentFrame(
    RefreshMode refresh_mode, bool redraw_pending) {
  if (redraw_pending) return false;
  switch (refresh_mode) {
    case kRefreshMode_Vsync:
    case kRefreshMode_Unlimited:
    case kRefreshMode_Limit:
    case kRefreshMode_Uncapped:
      return true;
    case kRefreshMode_Count:
    default:
      return false;
  }
}

uint64_t HostDisplayPacing_CatchupCapNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns,
    int maximum_catchup_frames) {
  const int catchup_frames =
      maximum_catchup_frames > 0 ? maximum_catchup_frames : 1;
  const uint64_t ordinary_cap_ns =
      emulation_frame_interval_ns * (uint64_t)catchup_frames;
  const uint64_t frame_limit_ns =
      HostDisplayPacing_FrameLimitIntervalNs(options);
  const uint64_t limited_present_headroom_ns = frame_limit_ns
      ? frame_limit_ns + emulation_frame_interval_ns
      : 0;
  return limited_present_headroom_ns > ordinary_cap_ns
      ? limited_present_headroom_ns
      : ordinary_cap_ns;
}

int HostDisplayPacing_WindowAxisToOutput(int window_position,
                                        int window_extent,
                                        int output_extent) {
  if (window_extent <= 0 || output_extent <= 0) return 0;
  const int64_t scaled =
      ((int64_t)window_position * output_extent + window_extent / 2) /
      window_extent;
  if (scaled < 0) return 0;
  if (scaled > output_extent - 1) return output_extent - 1;
  return (int)scaled;
}
