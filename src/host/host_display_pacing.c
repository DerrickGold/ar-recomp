#include "host_display_pacing.h"

#include <stdint.h>

#include "constants.h"

static const uint64_t kFpsSampleIntervalNs = kNanosecondsPerSecond / 2u;

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

uint64_t HostDisplayPacing_FrameLimitIntervalNs(
    HostDisplayPacingOptions options) {
  if (options.refresh_mode == kRefreshMode_Limit) {
    const int frames_per_second =
        options.frame_limit_fps > 0 ? options.frame_limit_fps : 1;
    return kNanosecondsPerSecond / (uint64_t)frames_per_second;
  }

  /* Unlimited is softly capped at twice the trustworthy host refresh. A
   * compositor-managed session owns its pacing and may advertise a phantom
   * refresh rate, so only an explicit Limit applies there. */
  if (options.refresh_mode == kRefreshMode_Unlimited &&
      !options.compositor_managed &&
      options.host_refresh_hz > 0) {
    return kNanosecondsPerSecond /
           ((uint64_t)options.host_refresh_hz * 2u);
  }
  return 0;
}

uint64_t HostDisplayPacing_UiIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  if (options.compositor_managed)
    return emulation_frame_interval_ns / 2u;

  const int refresh_hz =
      options.host_refresh_hz > 0 ? options.host_refresh_hz : 60;
  const uint64_t refresh_interval_ns =
      kNanosecondsPerSecond / (uint64_t)refresh_hz;
  return options.refresh_mode == kRefreshMode_Vsync
      ? refresh_interval_ns / 2u
      : refresh_interval_ns;
}

uint64_t HostDisplayPacing_PausedIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  const uint64_t ui_interval_ns =
      HostDisplayPacing_UiIntervalNs(options, emulation_frame_interval_ns);
  return ui_interval_ns > emulation_frame_interval_ns
      ? ui_interval_ns
      : emulation_frame_interval_ns;
}

uint64_t HostDisplayPacing_GameIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns) {
  if (options.refresh_mode == kRefreshMode_Uncapped) return 0;
  const uint64_t limited_interval_ns =
      HostDisplayPacing_FrameLimitIntervalNs(options);
  if (limited_interval_ns) return limited_interval_ns;

  const uint64_t anti_spin_interval_ns =
      HostDisplayPacing_UiIntervalNs(options, emulation_frame_interval_ns);
  return anti_spin_interval_ns
      ? anti_spin_interval_ns
      : emulation_frame_interval_ns / 2u;
}

bool HostDisplayPacing_ShouldRepresentFrame(
    RefreshMode refresh_mode, bool diorama_frame_active,
    bool interpolation_enabled, bool pair_interpolable,
    bool redraw_pending) {
  if (redraw_pending) return false;
  if (refresh_mode == kRefreshMode_Uncapped) return true;
  return diorama_frame_active && interpolation_enabled && pair_interpolable;
}

uint64_t HostDisplayPacing_CatchupCapNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns,
    int maximum_catchup_frames) {
  const int catchup_frames =
      maximum_catchup_frames > 0 ? maximum_catchup_frames : 1;
  const uint64_t ordinary_cap_ns =
      emulation_frame_interval_ns * (uint64_t)catchup_frames;
  const uint64_t limited_present_headroom_ns =
      HostDisplayPacing_FrameLimitIntervalNs(options) +
      emulation_frame_interval_ns;
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
