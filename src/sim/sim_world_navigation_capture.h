#ifndef SIM_WORLD_NAVIGATION_CAPTURE_H
#define SIM_WORLD_NAVIGATION_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "snesrecomp/runner.h"
#include "sim_render_metadata.h"

enum {
  kSimWorldNavigationCompositionWidth = kActRaiserAuthenticWidth,
  kSimWorldNavigationCompositionHeight = kActRaiserAuthenticHeight,
  kSimWorldNavigationCompositionPitch =
      kSimWorldNavigationCompositionWidth * (int)sizeof(uint32_t),
};

extern uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
extern uint32_t g_sim_world_navigation_ui_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];

/* Captures the two authentic navigation OAM compositions into host-owned
 * transparent buffers. Partial INIDISP brightness is supported and published
 * for whole-scene fading; forced blank or any structural failure changes a
 * selected navigation frame to AuthenticFallback. All-hidden action-entry OAM
 * is a successful empty capture. No emulated state is mutated. */
bool SimWorldNavigationCapture_Capture(SimFrameData *frame,
                                       SrRunnerHandle *runner);

#endif  /* SIM_WORLD_NAVIGATION_CAPTURE_H */
