#ifndef SIM_WORLD_NAVIGATION_CAPTURE_H
#define SIM_WORLD_NAVIGATION_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "snesrecomp/runner.h"
#include "sim_render_metadata.h"
#include "sim_world_navigation_palace.h"

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
/* A separate observational BG1 winner surface for the native Palace. It has
 * no connection to the navigation marker texture or action-effect masks. */
extern uint32_t g_sim_sky_palace_mask_pixels[
    kSimWorldNavigationPalaceMaxWidth * kSimWorldNavigationPalaceMaxHeight];

/* Captures the two authentic navigation OAM compositions into host-owned
 * transparent buffers. Partial INIDISP brightness is supported and published
 * for whole-scene fading; forced blank or any structural failure changes a
 * selected navigation frame to AuthenticFallback. All-hidden action-entry OAM
 * is a successful empty capture. No emulated state is mutated. */
/* A SkyPalace frame instead validates the completed observational BG1 winner
 * capture and publishes its master brightness. Foreground pixel composition
 * consumes the slot's public surfaces during presentation upload. */
bool SimWorldNavigationCapture_Capture(SimFrameData *frame,
                                       SrRunnerHandle *runner);

#endif  /* SIM_WORLD_NAVIGATION_CAPTURE_H */
