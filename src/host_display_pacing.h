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

#endif /* HOST_DISPLAY_PACING_H */
