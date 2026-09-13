/* Private presentation contract for the connected SIM background.
 * Implementation shares the world renderer's resource owner, not its camera
 * or navigation state. Inputs are immutable captures and the final SIM camera;
 * no runner ABI, backend objects or live game state cross this boundary.
 * The active town, actors, picker and clouds remain owned by present_sim3d. */
#ifndef AR_PRESENT_SIM_GLOBE_H
#define AR_PRESENT_SIM_GLOBE_H
#include "present.h"
#include "scene3d_math.h"
#include "present_sim_globe_mapping.h"

/* Owned placement values for later SIM atmosphere composition. Published on
 * both direct draws and retained-image hits; never a borrowed renderer cache. */
typedef struct PresentSimGlobeView {
  SimGlobeMapping map;
  float matrix[16], camera[3];
} PresentSimGlobeView;

/* Clamp a local copy before building the one matrix used by every SIM layer.
 * Retains the standard SIM low-angle pitch range while bounding zoom/yaw.
 * Does not change saved camera preferences, game coordinates or input. */
void PresentSimGlobe_ClampCamera(Scene3DCamera *camera);
/* A selected globe is complete or CoreFailure; failure must not silently
 * replace the connected world with the flat underlay.
 * Optional out_view is usable only on Complete, including cached frames.
 * Reset/lifetime belongs to PresentWorldNav_ResetResources. */
PresentationOutcome PresentSimGlobeUnderlay(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    PresentSimGlobeView *out_view);

#if AR_SIM_GLOBE_TESTING
/* Direct-render oracle only; not a shipping setting or environment switch. */
void PresentSimGlobe_TestRetainImages(bool enabled);
#endif
#endif
