#include "diorama_stack_group.h"

#include <math.h>
#include <stddef.h>

enum {
  kStackGroupPaddingPixels = 2,
};

/* Require a meaningful win because target switching and the final texture
 * sample have fixed costs that a pure fragment-area estimate cannot see. */
static const double kStackGroupMaximumCostRatio = 0.85;

bool DioramaStackGroupBounds_FromGeometry(
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count,
    DioramaStackGroupBounds *bounds) {
  if (bounds) *bounds = (DioramaStackGroupBounds){0};
  if (!vertices || vertex_count <= 0 || !indices || index_count <= 0 ||
      !bounds)
    return false;
  bool found = false;
  for (int i = 0; i < index_count; i++) {
    const int32_t index = indices[i];
    if (index < 0 || index >= vertex_count) continue;
    const float x = vertices[index].position.x;
    const float y = vertices[index].position.y;
    if (!isfinite(x) || !isfinite(y)) continue;
    if (!found) {
      bounds->x0 = bounds->x1 = x;
      bounds->y0 = bounds->y1 = y;
      found = true;
    } else {
      if (x < bounds->x0) bounds->x0 = x;
      if (x > bounds->x1) bounds->x1 = x;
      if (y < bounds->y0) bounds->y0 = y;
      if (y > bounds->y1) bounds->y1 = y;
    }
  }
  return found && bounds->x1 > bounds->x0 && bounds->y1 > bounds->y0;
}

static float ClampFloat(float value, float minimum, float maximum) {
  return value < minimum ? minimum : value > maximum ? maximum : value;
}

DioramaStackGroupPlan DioramaStackGroupPlan_Build(
    int output_width, int output_height,
    int source_width, int source_height,
    const DioramaStackGroupBounds *copies, int copy_count) {
  DioramaStackGroupPlan plan = {0};
  if (output_width <= 0 || output_height <= 0 ||
      source_width <= 0 || source_height <= 0 || !copies || copy_count < 2)
    return plan;

  plan.target_width = (output_width + 1) / 2;
  plan.target_height = (output_height + 1) / 2;
  plan.scale_x = (float)plan.target_width / (float)output_width;
  plan.scale_y = (float)plan.target_height / (float)output_height;
  /* At a small output the direct path is both cheaper and sharper. Group only
   * where the half-output target still preserves every captured source texel. */
  if (plan.target_width < source_width || plan.target_height < source_height)
    return plan;

  float union_x0 = (float)output_width;
  float union_y0 = (float)output_height;
  float union_x1 = 0.0f;
  float union_y1 = 0.0f;
  int visible_copies = 0;
  for (int i = 0; i < copy_count; i++) {
    const float x0 = ClampFloat(copies[i].x0, 0.0f, (float)output_width);
    const float y0 = ClampFloat(copies[i].y0, 0.0f, (float)output_height);
    const float x1 = ClampFloat(copies[i].x1, 0.0f, (float)output_width);
    const float y1 = ClampFloat(copies[i].y1, 0.0f, (float)output_height);
    if (x1 <= x0 || y1 <= y0) continue;
    plan.direct_pixels += (double)(x1 - x0) * (double)(y1 - y0);
    if (x0 < union_x0) union_x0 = x0;
    if (y0 < union_y0) union_y0 = y0;
    if (x1 > union_x1) union_x1 = x1;
    if (y1 > union_y1) union_y1 = y1;
    visible_copies++;
  }
  if (visible_copies < 2 || plan.direct_pixels <= 0.0) return plan;

  int bounds_x0 = (int)floorf(union_x0) - kStackGroupPaddingPixels;
  int bounds_y0 = (int)floorf(union_y0) - kStackGroupPaddingPixels;
  int bounds_x1 = (int)ceilf(union_x1) + kStackGroupPaddingPixels;
  int bounds_y1 = (int)ceilf(union_y1) + kStackGroupPaddingPixels;
  if (bounds_x0 < 0) bounds_x0 = 0;
  if (bounds_y0 < 0) bounds_y0 = 0;
  if (bounds_x1 > output_width) bounds_x1 = output_width;
  if (bounds_y1 > output_height) bounds_y1 = output_height;
  plan.output_bounds = (ArRenderRectI){
    bounds_x0, bounds_y0, bounds_x1 - bounds_x0, bounds_y1 - bounds_y0,
  };
  if (plan.output_bounds.w <= 0 || plan.output_bounds.h <= 0) return plan;

  const double union_pixels =
      (double)plan.output_bounds.w * (double)plan.output_bounds.h;
  const double target_clear_pixels =
      (double)plan.target_width * (double)plan.target_height;
  plan.grouped_pixels =
      plan.direct_pixels * (double)plan.scale_x * (double)plan.scale_y +
      union_pixels + target_clear_pixels;
  plan.use_intermediate =
      plan.grouped_pixels <= plan.direct_pixels * kStackGroupMaximumCostRatio;
  return plan;
}
