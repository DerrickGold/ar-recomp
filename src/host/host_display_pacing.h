#ifndef HOST_DISPLAY_PACING_H
#define HOST_DISPLAY_PACING_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

typedef struct HostDisplayPacingOptions {
  RefreshMode refresh_mode;
  int frame_limit_fps;
  int host_refresh_hz;
  bool compositor_managed;
} HostDisplayPacingOptions;

/* Rolling completed-present rate. Count intervals between present-completion
 * timestamps rather than emulation ticks, so retained-frame redraws and a
 * blocking SDL_RenderPresent are both reflected in the number the user sees. */
typedef struct HostDisplayFpsCounter {
  uint64_t sample_start_ns;
  uint32_t completed_intervals;
  double frames_per_second;
  bool initialized;
} HostDisplayFpsCounter;

void HostDisplayPacing_ResetFpsCounter(HostDisplayFpsCounter *counter);
void HostDisplayPacing_RecordPresent(
    HostDisplayFpsCounter *counter, uint64_t completed_at_ns);
double HostDisplayPacing_FramesPerSecond(
    const HostDisplayFpsCounter *counter);

uint64_t HostDisplayPacing_FrameLimitIntervalNs(
    HostDisplayPacingOptions options);
uint64_t HostDisplayPacing_UiIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);
uint64_t HostDisplayPacing_PausedIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);
uint64_t HostDisplayPacing_GameIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);

/* Between emulation ticks, ordinary redraws are useful only for interpolable
 * diorama frames. Uncapped is the explicit profiling exception: recomposite a
 * retained frame even when visually identical so the measured present rate is
 * actual renderer throughput rather than the emulation's ~60 Hz tick rate. */
bool HostDisplayPacing_ShouldRepresentFrame(
    RefreshMode refresh_mode, bool diorama_frame_active,
    bool interpolation_enabled, bool pair_interpolable,
    bool redraw_pending);

/* Retain enough accumulated time for the normal spiral-of-death window and
 * for one deliberately slow limited present plus the next emulation tick. */
uint64_t HostDisplayPacing_CatchupCapNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns,
    int maximum_catchup_frames);

/* Maps one axis of a window-client coordinate onto renderer-output pixels.
 *
 * Pure so it can be unit-tested: the interesting behaviour is at the edges and
 * the two sizes are rarely equal (a high-DPI backing store makes output larger;
 * a reduced render resolution would make it smaller).
 *
 * Rounds to nearest and CLAMPS to the last valid pixel. The clamp is the point:
 * round-to-nearest alone returns `extent` — one past the end — for the final
 * window pixel whenever output <= window/2, which is exactly the ratio a 2x
 * downscale produces. Every current caller happens to bounds-check, so the
 * visible symptom can be only a one-pixel dead edge, but SDL permits mouse
 * coordinates outside the window, so no caller may assume the
 * result is in range unless this guarantees it.
 *
 * Returns 0 for a non-positive extent rather than dividing by zero. */
int HostDisplayPacing_WindowAxisToOutput(int window_position,
                                         int window_extent,
                                         int output_extent);

#endif /* HOST_DISPLAY_PACING_H */
