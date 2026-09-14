/* Presentation-only projection for the continuous SIM experiment. Native
 * movement/target coordinates remain unchanged. No renderer, capture, live
 * state, texture handle or retained pointer is owned by this value. */
#ifndef AR_PRESENT_SIM_GLOBE_PROJECT_H
#define AR_PRESENT_SIM_GLOBE_PROJECT_H
#include "present_sim_globe_mapping.h"
#include "render/render_types.h"
#include "scene3d_math.h"

typedef struct PresentSimGlobeProjection {
  SimGlobeMapping map;
  float matrix[16];
  ArRenderRectI viewport;
  float pixel_scale[2], reference_depth;
  bool ready;
} PresentSimGlobeProjection;

typedef struct PresentSimGlobeProjectedPoint {
  float world[3];
  Scene3DClipPoint clip;
  Scene3DPoint screen;
  float depth, pixel_scale[2];
} PresentSimGlobeProjectedPoint;

typedef struct PresentSimGlobeBillboardAxes {
  /* Screen displacement per authored sprite pixel, about the shared foot
   * anchor. Positive down follows the sprite's native image coordinates. */
  Scene3DPoint right, down;
} PresentSimGlobeBillboardAxes;

/* matrix is the final town-cell-to-clip transform published by the globe,
 * not the uncomposed SIM camera matrix. reference_depth uses the existing
 * SIM billboard auto-fit depth. All inputs are copied. Failure is atomic. */
bool PresentSimGlobeProject_Build(const SimGlobeMapping *map,
    const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
    float reference_depth, PresentSimGlobeProjection *out);

/* Interpolate native XY FIRST, then sample support and call this function.
 * XY is in native town pixels, not captured screen pixels, and may extend
 * beyond the town for incoming flyers/effects. No coordinate clamping.
 *
 * support is explicitly in registered world-floor units. Grounded actors and
 * target graphics use the same owned terrain registration as the drawn town;
 * aerial trajectories supply one stable datum, NOT the terrain below them.
 * altitude is the already-scaled native-pixel rise above that datum, measured
 * radially in local SIM units (16 pixels/cell). Support selection, height-pop,
 * sprite priority and multipart screen offsets belong to the caller.
 *
 * Returns screen, unclamped GPU depth and homogeneous clip position from ONE
 * transform. Offscreen points are allowed: a sprite may straddle the viewport.
 * Camera-plane/nonfinite failures leave the entire result untouched. Fixed
 * menus and screen-space effects must bypass this world projection. */
bool PresentSimGlobeProject_Point(const PresentSimGlobeProjection *projection,
    float native_x, float native_y, float support, float altitude,
    PresentSimGlobeProjectedPoint *out);

/* Pitch-only facing about the local globe's east tangent. Unlike a spherical
 * billboard, this preserves the ground's projected roll and yaw foreshortening.
 * No anchor/height changes, terrain re-sampling or camera-angle reconstruction.
 * The caller selects grounded art and applies height-pop to both axes once.
 * Flyers/HUD retain their screen-facing axes; map decals use curved corners.
 * The small sprite uses the same anchor-local affine scale as existing SIM.
 * Nonfinite/degenerate inputs leave out unchanged. */
bool PresentSimGlobeProject_GroundBillboardAxes(
    const PresentSimGlobeProjection *projection,
    float native_x, float native_y, const PresentSimGlobeProjectedPoint *anchor,
    PresentSimGlobeBillboardAxes *out);
#endif
