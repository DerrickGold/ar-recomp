#include "voxel_pick.h"
#include "diorama/diorama_render_options.h"

#include <float.h>
#include <math.h>

static double Cross(double ax, double ay, double bx, double by) {
  return ax * by - ay * bx;
}

static bool Triangle(const DioramaVoxelPickFace *face, int b, int c,
    double sx, double sy, EditorVoxelPickHit *out) {
  const double ax = face->screen_xy[0], ay = face->screen_xy[1];
  const double bx = face->screen_xy[b * 2] - ax;
  const double by = face->screen_xy[b * 2 + 1] - ay;
  const double cx = face->screen_xy[c * 2] - ax;
  const double cy = face->screen_xy[c * 2 + 1] - ay;
  const double area = Cross(bx, by, cx, cy);
  const double scale = fmax(bx * bx + by * by, cx * cx + cy * cy);
  /* Reject collapsed and effectively edge-on triangles without imposing a
   * minimum pixel size on ordinary, narrow caps. Use double intermediates so
   * large capture coordinates cannot overflow the area calculation. */
  if (scale == 0 || fabs(area) <= scale * 1e-12) return false;
  double weights[3];
  weights[1] = Cross(sx - ax, sy - ay, cx, cy) / area;
  weights[2] = Cross(bx, by, sx - ax, sy - ay) / area;
  weights[0] = 1 - weights[1] - weights[2];
  for (int i = 0; i < 3; ++i) {
    if (weights[i] < -1e-7 || weights[i] > 1 + 1e-7) return false;
    weights[i] = fmax(0, fmin(1, weights[i]));
  }
  const double sum = weights[0] + weights[1] + weights[2];
  const int vertices[3] = {0, b, c};
  double reciprocal_w = 0, world_x = 0, world_y = 0, depth = 0;
  for (int i = 0; i < 3; ++i) {
    const int v = vertices[i];
    const double weight = weights[i] / sum;
    const double perspective_weight = weight / face->clip_w[v];
    reciprocal_w += perspective_weight;
    world_x += perspective_weight * face->world_xy[v * 2];
    world_y += perspective_weight * face->world_xy[v * 2 + 1];
    depth += weight * face->depth[v];
  }
  world_x /= reciprocal_w;
  world_y /= reciprocal_w;
  if (!isfinite(world_x) || !isfinite(world_y) || !isfinite(depth) ||
      fabs(world_x) > FLT_MAX || fabs(world_y) > FLT_MAX || fabs(depth) > FLT_MAX)
    return false;
  *out = (EditorVoxelPickHit){(float)world_x, (float)world_y, (float)depth};
  return true;
}

bool EditorVoxelPick_Hit(const DioramaVoxelPickFace *face,
    float screen_x, float screen_y, EditorVoxelPickHit *out_hit) {
  if (!face || !out_hit || !isfinite(screen_x) || !isfinite(screen_y)) return false;
  for (int i = 0; i < 4; ++i) {
    /* Unclipped surfaces crossing or behind the eye are not valid pick faces.
     * A malformed fourth vertex invalidates the whole observation, even when
     * the pointer happens to land inside its other triangle. */
    if (!isfinite(face->screen_xy[i * 2]) || !isfinite(face->screen_xy[i * 2 + 1]) ||
        !isfinite(face->world_xy[i * 2]) || !isfinite(face->world_xy[i * 2 + 1]) ||
        !isfinite(face->depth[i]) || !isfinite(face->clip_w[i]) || face->clip_w[i] <= 0)
      return false;
  }
  EditorVoxelPickHit first, second;
  const bool hit_first = Triangle(face, 1, 2, screen_x, screen_y, &first);
  const bool hit_second = Triangle(face, 2, 3, screen_x, screen_y, &second);
  if (!hit_first && !hit_second) return false;
  *out_hit = hit_first && (!hit_second || first.depth <= second.depth) ? first : second;
  return true;
}
