#include "sim_world_navigation_globe.h"

#include <math.h>
#include "sim_world_map.h"

static bool ChartPoint(float radius, float tile_x, float tile_y,
                       float *x, float *y, float *inverse) {
  if (!isfinite(radius) || radius <= 0 || !isfinite(2.0f * radius) ||
      !isfinite(tile_x) || !isfinite(tile_y)) return false;
  *x = (tile_x - kSimWorldMapTiles * 0.5f) / (2.0f * radius);
  *y = (kSimWorldMapTiles * 0.5f - tile_y) / (2.0f * radius);
  const float denominator = 1.0f + *x * *x + *y * *y;
  if (!isfinite(denominator)) return false;
  *inverse = 1.0f / denominator;
  return true;
}

bool SimWorldNavigationGlobe_Sample(
    float tile_x, float tile_y, float normal[3], float *scale) {
  return SimWorldNavigationGlobe_SampleAtRadius(
      kSimWorldNavigationGlobeRadiusTiles, tile_x, tile_y, normal, scale);
}

bool SimWorldNavigationGlobe_SampleAtRadius(
    float radius_tiles, float tile_x, float tile_y, float normal[3], float *scale) {
  float x, y, inverse;
  if (!normal || !ChartPoint(radius_tiles, tile_x, tile_y, &x, &y, &inverse)) return false;
  normal[0] = 2.0f * x * inverse;
  normal[1] = 2.0f * y * inverse;
  normal[2] = 2.0f * inverse - 1.0f;
  if (scale) *scale = inverse;
  return true;
}

bool SimWorldNavigationGlobe_Source(
    const float normal[3], float *tile_x, float *tile_y) {
  return SimWorldNavigationGlobe_SourceAtRadius(
      kSimWorldNavigationGlobeRadiusTiles, normal, tile_x, tile_y);
}

bool SimWorldNavigationGlobe_SourceAtRadius(
    float radius_tiles, const float normal[3], float *tile_x, float *tile_y) {
  if (!normal || !tile_x || !tile_y || !isfinite(radius_tiles) || radius_tiles <= 0 ||
      !isfinite(2.0f * radius_tiles)) return false;
  const float length = hypotf(hypotf(normal[0], normal[1]), normal[2]);
  if (!isfinite(length) || length <= 0.0f) return false;
  const float denominator = length + normal[2];
  if (denominator <= length * 0.000001f) return false;
  const float scale = 2.0f * radius_tiles / denominator;
  const float x = kSimWorldMapTiles * 0.5f + normal[0] * scale;
  const float y = kSimWorldMapTiles * 0.5f - normal[1] * scale;
  if (!isfinite(x) || !isfinite(y)) return false;
  *tile_x = x;
  *tile_y = y;
  return true;
}

bool SimWorldNavigationGlobe_BuildFrame(
    float focus_tile_x, float focus_tile_y, float heading_radians,
    SimWorldNavigationGlobeFrame *out) {
  return SimWorldNavigationGlobe_BuildFrameAtRadius(kSimWorldNavigationGlobeRadiusTiles,
      focus_tile_x, focus_tile_y, heading_radians, out);
}

bool SimWorldNavigationGlobe_BuildFrameAtRadius(
    float radius_tiles, float focus_tile_x, float focus_tile_y, float heading_radians,
    SimWorldNavigationGlobeFrame *out) {
  float x, y, inverse;
  if (!out || !isfinite(heading_radians) ||
      !ChartPoint(radius_tiles, focus_tile_x, focus_tile_y, &x, &y, &inverse)) return false;
  /* Normalized analytic chart derivatives, not finite differences. */
  const float east[3] = {
    (1.0f - x * x + y * y) * inverse,
    -2.0f * x * y * inverse, -2.0f * x * inverse,
  };
  const float north[3] = {
    -2.0f * x * y * inverse,
    (1.0f + x * x - y * y) * inverse, -2.0f * y * inverse,
  };
  SimWorldNavigationGlobeFrame frame;
  const float c = cosf(heading_radians), s = sinf(heading_radians);
  for (int i = 0; i < 3; i++) {
    frame.right[i] = c * east[i] - s * north[i];
    frame.up[i] = s * east[i] + c * north[i];
  }
  if (!SimWorldNavigationGlobe_SampleAtRadius(
          radius_tiles, focus_tile_x, focus_tile_y, frame.outward, NULL)) return false;
  *out = frame;
  return true;
}

bool SimWorldNavigationGlobe_OrbitFrame(
    SimWorldNavigationGlobeFrame *frame, float yaw, float pitch) {
  if (!frame || !isfinite(yaw) || !isfinite(pitch)) return false;
  if (yaw == 0 && pitch == 0) return true;
  const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
  SimWorldNavigationGlobeFrame out;
  for (int i = 0; i < 3; i++) {
    const float forward = cy * frame->outward[i] + sy * frame->right[i];
    out.right[i] = cy * frame->right[i] - sy * frame->outward[i];
    out.outward[i] = cp * forward + sp * frame->up[i];
    out.up[i] = cp * frame->up[i] - sp * forward;
  }
  *frame = out;
  return true;
}

void SimWorldNavigationGlobe_TransformNormal(
    const SimWorldNavigationGlobeFrame *frame,
    const float normal[3], float out[3]) {
  const float local[3] = {
    frame->right[0] * normal[0] + frame->right[1] * normal[1] +
        frame->right[2] * normal[2],
    frame->up[0] * normal[0] + frame->up[1] * normal[1] +
        frame->up[2] * normal[2],
    frame->outward[0] * normal[0] + frame->outward[1] * normal[1] +
        frame->outward[2] * normal[2],
  };
  for (int i = 0; i < 3; i++) out[i] = local[i];
}

bool SimWorldNavigationGlobe_CapOccluded(
    const float camera[3], const float axis[3], float angular_radius,
    float maximum_radius, float occluder_radius) {
  if (!camera || !axis || !isfinite(angular_radius) || angular_radius < 0 ||
      angular_radius >= 1.57079632679f || !isfinite(maximum_radius) ||
      maximum_radius <= 0 || !isfinite(occluder_radius) || occluder_radius <= 0)
    return false;
  const float distance = hypotf(hypotf(camera[0], camera[1]), camera[2]);
  const float axis_length = hypotf(hypotf(axis[0], axis[1]), axis[2]);
  if (!isfinite(distance) || distance <= occluder_radius ||
      !isfinite(axis_length) || axis_length <= 0) return false;
  float cosine = 0;
  for (int i = 0; i < 3; i++)
    cosine += (camera[i] / distance) * (axis[i] / axis_length);
  /* The two tangent radii bound the visible arc from eye to object. Use the
   * tallest point, not its ground anchor: a tower may rise above the limb.
   * Caps narrower than a hemisphere are convex cones, so this also covers
   * the interiors of the submitted triangles, not just their vertices. */
  const float visible_arc = acosf(occluder_radius / distance) +
      acosf(occluder_radius / fmaxf(occluder_radius, maximum_radius));
  const float separation = acosf(fminf(1, fmaxf(-1, cosine)));
  return separation > visible_arc + angular_radius + 0.0001f;
}
