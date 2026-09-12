/* Private globe/Palace projection and compatibility clipping vocabulary.
 * No live game state or backend storage crosses this presentation-only seam. */
#ifndef PRESENT_WORLD_NAV_GEOMETRY_H
#define PRESENT_WORLD_NAV_GEOMETRY_H
#include "scene3d_math.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_world_navigation_globe.h"

typedef struct WorldNavigationProjection {
  float matrix[16];
  SimWorldNavigationGlobeFrame globe_frame;
  float camera_world[3];
  float tile_world;
  float chart_radius_tiles;
  float height_world_per_unit;
  float reference_height_units;
  float globe_radius_world;
  float cloud_height_world;
  float atmosphere_height_world;
  bool clip_frustum;
} WorldNavigationProjection;

enum { kWorldNavigationClippedQuads = 2 * (kScene3DClippedPolygonCapacity - 2) };

/* Conservative bounds of source vertex directions/base elevations (no extra
 * displacement, as used by the land grid), not a sampled
 * silhouette. Reject only when an entire radial bound is outside a clip plane;
 * uncertainty (including nonfinite input) retains the geometry. */
typedef struct WorldNavigationRadialBounds {
  float normal_min[3], normal_max[3], height_min, height_max;
} WorldNavigationRadialBounds;
bool WorldNavigationRadialBoundsOutside(const WorldNavigationRadialBounds *bounds,
    const Sim3DDepthRadialTransform *transform);

/* Reusable receiver geometry. A clipped quad is a triangle fan represented
 * in the existing four-vertex submission format; triangle selects the two
 * original corners used with corner zero for difference-form UV blending. */
typedef struct WorldNavigationClipPlan {
  struct { float x, y, depth, weights[2]; } points[4];
  uint8_t triangle; /* zero: unchanged quad; one/two: original triangle */
} WorldNavigationClipPlan;

bool WorldNavigationPrepareClipPlan(const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *clip, ArRenderRectI viewport,
    WorldNavigationClipPlan plans[kWorldNavigationClippedQuads], size_t *count);
void WorldNavigationApplyShadowPlan(const WorldNavigationClipPlan *plan,
    const ArRenderPointF uv[4], ArRenderColorF color, Sim3DDepthVertex output[4]);
void WorldNavigationApplyShadowUV(const WorldNavigationClipPlan *plan,
    const ArRenderPointF uv[4], ArRenderPointF output[4]);

bool WorldNavigationProjectClippedPoint(
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    const float world[3], Scene3DPoint *screen, float *depth, Scene3DClipPoint *clip);

bool WorldNavigationClipQuad(
    const Sim3DDepthVertex input[4], const Scene3DClipPoint clip[4],
    ArRenderRectI viewport, Sim3DDepthVertex output[kWorldNavigationClippedQuads * 4],
    size_t *count);

bool WorldNavigationAppendClippedQuad(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *clip, ArRenderRectI viewport);

bool WorldNavigationAppendClippedQuads(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex *input,
    const Scene3DClipPoint *clip, size_t count, ArRenderRectI viewport);

static inline uint8_t WorldNavigationViewportOutside(float x, float y, int width, int height) {
  return (uint8_t)((x < -1.0f ? 1 : 0) | (x > width + 1.0f ? 2 : 0) |
                   (y < -1.0f ? 4 : 0) | (y > height + 1.0f ? 8 : 0));
}

static inline uint8_t WorldNavigationClipOutside(Scene3DClipPoint point) {
  return (uint8_t)((point.x < -point.w ? 1 : 0) | (point.x > point.w ? 2 : 0) |
      (point.y < -point.w ? 4 : 0) | (point.y > point.w ? 8 : 0) |
      (point.z < -point.w ? 16 : 0) | (point.z > point.w ? 32 : 0));
}

static inline bool WorldNavigationProjectPoint(
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    const float world[3], Scene3DPoint *screen, float *depth, Scene3DClipPoint *clip) {
  if (projection->clip_frustum)
    return WorldNavigationProjectClippedPoint(projection, viewport, world, screen, depth, clip);
  return Scene3D_ProjectWorldPointWithDepth(projection->matrix,
      world[0], world[1], world[2], viewport.w, viewport.h, screen, depth);
}

static inline bool WorldNavigationAppendProjectedQuad(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *clip, ArRenderRectI viewport) {
  return clip ? WorldNavigationAppendClippedQuad(layer, input, clip, viewport)
      : Sim3DDepthPass_AppendQuad(layer, input);
}

static inline bool WorldNavigationAppendProjectedQuads(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex *input,
    const Scene3DClipPoint *clip, size_t count, ArRenderRectI viewport) {
  return clip ? WorldNavigationAppendClippedQuads(layer, input, clip, count, viewport)
      : Sim3DDepthPass_AppendQuads(layer, input, count);
}

#endif
