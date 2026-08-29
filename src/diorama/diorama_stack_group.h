#ifndef AR_DIORAMA_STACK_GROUP_H
#define AR_DIORAMA_STACK_GROUP_H

#include <stdbool.h>

#include "render/render_types.h"

typedef struct DioramaStackGroupBounds {
  float x0, y0, x1, y1;
} DioramaStackGroupBounds;

typedef struct DioramaStackGroupPlan {
  bool use_intermediate;
  int target_width;
  int target_height;
  ArRenderRectI output_bounds;
  float scale_x;
  float scale_y;
  double direct_pixels;
  double grouped_pixels;
} DioramaStackGroupPlan;

/* Bounds only vertices referenced by the compacted index stream. This keeps
 * sparse object-stack cells from inflating the group back to the complete
 * projected plane. */
bool DioramaStackGroupBounds_FromGeometry(
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count,
    DioramaStackGroupBounds *bounds);

/* Decide whether repeated full-resolution blending is expensive enough to
 * justify a half-resolution intermediate. The target must still be at least
 * as large as the captured source, so grouping never downsamples an already
 * minified layer. `copies` contains one projected bound per non-redundant
 * stack/voxel slice; the ordinary front plane is deliberately excluded. */
DioramaStackGroupPlan DioramaStackGroupPlan_Build(
    int output_width, int output_height,
    int source_width, int source_height,
    const DioramaStackGroupBounds *copies, int copy_count);

#endif /* AR_DIORAMA_STACK_GROUP_H */
