#ifndef HOST_DISPLAY_PACING_H
#define HOST_DISPLAY_PACING_H

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

typedef struct HostDisplayPacingOptions {
  RefreshMode refresh_mode;
  int frame_limit_fps;
  int nominal_refresh_hz;
  bool compositor_managed;
  bool vsync_active;
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

/* Select the emulation/source cadence independently from host presentation.
 * Test30 is intentionally scoped to an active interpolated Diorama frame. */
uint64_t HostDisplayPacing_SourceFrameIntervalNs(
    uint64_t native_interval_ns, bool diorama_active,
    bool interpolation_enabled, InterpolationSourceRate source_rate);

void HostDisplayPacing_ResetFpsCounter(HostDisplayFpsCounter *counter);
void HostDisplayPacing_RecordPresent(
    HostDisplayFpsCounter *counter, uint64_t completed_at_ns);
double HostDisplayPacing_FramesPerSecond(
    const HostDisplayFpsCounter *counter);

/* Keep VSync latency to one queued frame. Software-paced and unlimited modes
 * need two so the GPU renderer can overlap submission with the preceding
 * frame instead of serializing each SDL_RenderPresent. */
uint32_t HostDisplayPacing_AllowedFramesInFlight(RefreshMode refresh_mode);

uint64_t HostDisplayPacing_FrameLimitIntervalNs(
    HostDisplayPacingOptions options);
/* Software presentation deadline. VSync returns zero when the renderer
 * accepted it; Uncapped follows nominal display metadata; Limit follows the
 * user target; Unlimited returns zero deliberately. */
uint64_t HostDisplayPacing_UiIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);
uint64_t HostDisplayPacing_PausedIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);
uint64_t HostDisplayPacing_GameIntervalNs(
    HostDisplayPacingOptions options,
    uint64_t emulation_frame_interval_ns);

/* A valid refresh mode owns host presentation cadence independently of whether
 * the retained frame can be visually interpolated. The caller separately
 * decides whether to apply interpolation or present the retained tick exactly.
 * A pending content redraw is the only ordinary suppression: presenting the
 * older retained frame then would visibly move backward for one host frame. */
bool HostDisplayPacing_ShouldRepresentFrame(
    RefreshMode refresh_mode, bool redraw_pending);

/* Retain enough accumulated time for the normal spiral-of-death window and
 * for one deliberately slow explicit-Limit present plus the next emulation
 * tick. No nominal display property may affect this emulation-side bound. */
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
