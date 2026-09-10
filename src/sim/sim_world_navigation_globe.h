#ifndef SIM_WORLD_NAVIGATION_GLOBE_H
#define SIM_WORLD_NAVIGATION_GLOBE_H

#include <stdbool.h>

enum { kSimWorldNavigationGlobeRadiusTiles = 48 };

/* A rigid, orthonormal world-to-focus orientation. The original square map
 * is a fixed stereographic chart on the sphere, not a camera-relative warp.
 * This conformal chart preserves local town angles without a polar pinch.
 * Uncharted sphere surface is ocean; native destination coordinates do not
 * wrap or change when presentation rotates the planet. */
typedef struct SimWorldNavigationGlobeFrame {
  float right[3], up[3], outward[3];
} SimWorldNavigationGlobeFrame;

/* Map tile coordinates to a persistent unit-sphere direction. Optional scale
 * is the local arc length per map tile, relative to the chart's center. */
bool SimWorldNavigationGlobe_Sample(
    float tile_x, float tile_y, float normal[3], float *scale);
/* Inverse chart, also defined over ocean outside the original map bounds.
 * The antipodal chart singularity has no finite source coordinate. */
bool SimWorldNavigationGlobe_Source(
    const float normal[3], float *tile_x, float *tile_y);
bool SimWorldNavigationGlobe_BuildFrame(
    float focus_tile_x, float focus_tile_y, float heading_radians,
    SimWorldNavigationGlobeFrame *out);
/* Explicit chart radius, in center-metric map tiles. The default wrappers
 * above retain navigation's radius. These pure variants let a presentation
 * choose another curvature without changing native coordinates or global
 * state. Radius must be finite and positive; failure leaves outputs intact. */
bool SimWorldNavigationGlobe_SampleAtRadius(
    float radius_tiles, float tile_x, float tile_y, float normal[3], float *scale);
bool SimWorldNavigationGlobe_SourceAtRadius(
    float radius_tiles, const float normal[3], float *tile_x, float *tile_y);
bool SimWorldNavigationGlobe_BuildFrameAtRadius(
    float radius_tiles, float focus_tile_x, float focus_tile_y, float heading_radians,
    SimWorldNavigationGlobeFrame *out);
/* Rotate an already built navigation frame about the planet centre. Zero is
 * an exact no-op; invalid angles leave the frame untouched. Supports full
 * yaw turns and either pole without changing the native travel coordinates. */
bool SimWorldNavigationGlobe_OrbitFrame(
    SimWorldNavigationGlobeFrame *frame, float yaw, float pitch);
void SimWorldNavigationGlobe_TransformNormal(
    const SimWorldNavigationGlobeFrame *frame,
    const float normal[3], float out[3]);

/* Conservative horizon test for a radial cap. Camera is relative to the
 * planet centre; axis is its outward direction. Every object point must be
 * within angular_radius of axis and no farther than maximum_radius from the
 * centre. occluder_radius must fit INSIDE the rendered opaque planet mesh.
 * Uncertain/invalid bounds remain visible; true means the entire cap is hidden. */
bool SimWorldNavigationGlobe_CapOccluded(
    const float camera[3], const float axis[3], float angular_radius,
    float maximum_radius, float occluder_radius);

#endif /* SIM_WORLD_NAVIGATION_GLOBE_H */
