/* Private presentation contract for continuous-globe SIM.
 * Implementation shares the world renderer's resource owner, not its camera
 * or navigation state. Inputs are immutable captures and the final SIM camera;
 * no runner ABI, backend objects or live game state cross this boundary.
 * SIM owns active-town content, actors, picker and clouds; the world compositor
 * receives only synchronous presentation callbacks for its shared depth pass. */
#ifndef AR_PRESENT_SIM_GLOBE_H
#define AR_PRESENT_SIM_GLOBE_H
#include "present.h"
#include "scene3d_math.h"
#include "present_sim_globe_mapping.h"

/* Owned placement values for later SIM atmosphere composition. Published on
 * each draw; never a borrowed renderer cache. */
typedef struct PresentSimGlobeView {
  SimGlobeMapping map;
  float matrix[16], camera[3];
} PresentSimGlobeView;

/* Clamp a local copy before building the one matrix used by every SIM layer.
 * Retains the standard SIM low-angle pitch range while bounding zoom/yaw.
 * Does not change saved camera preferences, game coordinates or input. */
void PresentSimGlobe_ClampCamera(Scene3DCamera *camera);
typedef struct PresentSimGlobeGroundShadow {
  ArRenderTexture texture;
  float opacity;
} PresentSimGlobeGroundShadow;

/* Synchronous presentation-owned append seam, while the world's shared depth
 * pass is collecting. May append portable depth geometry only, not submit,
 * begin another pass or retain view/userdata. Failure aborts this composition.
 * Resident terrain/model sources are reused; the shared depth pass composites
 * fresh content each frame. No gameplay policy enters the world renderer. */
typedef struct PresentSimGlobeContent {
  /* Optional preparation runs BEFORE the depth pass. A returned mask is in
   * native town XY, borrowed through submission, and sampled only on tops. */
  bool (*prepare)(void *userdata, const PresentSimGlobeView *view,
      PresentSimGlobeGroundShadow *shadow);
  bool (*append)(void *userdata, const PresentSimGlobeView *view);
  void *userdata;
} PresentSimGlobeContent;
/* Complete or CoreFailure: never silently fall back to flat SIM.
 * Optional out_view is usable only on Complete.
 * Reset/lifetime belongs to PresentWorldNav_ResetResources. */
PresentationOutcome PresentSimGlobeTown(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    const PresentSimGlobeContent *content, PresentSimGlobeView *out_view);

#if AR_SIM_GLOBE_TESTING
/* Frozen globe-source contract tests, not shipping mode/settings switches.
 * Uses the navigation globe's terrain and active town models with the SIM
 * camera, no flat town backing/collar. Radius scale is relative to today's
 * navigation radius (1..4), preserving the local tile metric. Active models
 * use background_voxel_detail; neighbours stay Low. These background-only
 * checks deliberately omit the live callback, actors and UI. */
PresentationOutcome PresentSimGlobe_TestTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    float radius_scale, PresentSimGlobeView *out_view);
/* Same scene with the active town's SIM facing/shading settings. Ground and
 * bridges remain geometric; neighbours retain navigation's Low models. */
PresentationOutcome PresentSimGlobe_TestFacingTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    float radius_scale, PresentSimGlobeView *out_view);
/* Requires the matching native SIM canvas/voxel scene to have been built and
 * uploaded. Uses its exact active model identities/animation phases and
 * replaces (does not overlay) active-town globe mountains. */
PresentationOutcome PresentSimGlobe_TestDetailedTownScene(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    float radius_scale, PresentSimGlobeView *out_view);
struct SimWorldNavigationMountainFace;
/* Temporary synthetic source for capacity tests after a normal SIM draw.
 * Requires an active depth pass; restores authored sources before returning. */
bool PresentSimGlobe_TestSurfaceSource(const FrameSlot *slot,
    struct SimWorldNavigationMountainFace *faces, size_t count,
    size_t *published_mountains, size_t *chunks);
#endif
#endif
