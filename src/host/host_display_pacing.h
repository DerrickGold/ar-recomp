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
