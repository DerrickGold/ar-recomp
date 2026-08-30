#include "diorama_edge_aa.h"

#include <math.h>

static int BoundaryPointCount(int subdiv_x, int subdiv_y) {
  if (subdiv_x <= 0 || subdiv_y <= 0) return 0;
  return 2 * (subdiv_x + subdiv_y);
}

int DioramaEdgeAa_FringeVertexCapacity(int subdiv_x, int subdiv_y) {
  return 2 * BoundaryPointCount(subdiv_x, subdiv_y);
}

int DioramaEdgeAa_FringeIndexCapacity(int subdiv_x, int subdiv_y) {
  return 6 * BoundaryPointCount(subdiv_x, subdiv_y);
}

static int BoundaryGridIndex(int point, int subdiv_x, int subdiv_y) {
  const int columns = subdiv_x + 1;
  if (point <= subdiv_x) return point;
  if (point <= subdiv_x + subdiv_y)
    return (point - subdiv_x) * columns + subdiv_x;
  if (point <= 2 * subdiv_x + subdiv_y) {
    const int column = 2 * subdiv_x + subdiv_y - point;
    return subdiv_y * columns + column;
  }
  const int row = 2 * (subdiv_x + subdiv_y) - point;
  return row * columns;
}

static DioramaEdgeAaMask BoundarySegmentEdge(
    int segment, int subdiv_x, int subdiv_y) {
  if (segment < subdiv_x) return kDioramaEdgeAa_Top;
  if (segment < subdiv_x + subdiv_y) return kDioramaEdgeAa_Right;
  if (segment < 2 * subdiv_x + subdiv_y)
    return kDioramaEdgeAa_Bottom;
  return kDioramaEdgeAa_Left;
}

static ArRenderPointF OutwardNormal(
    ArRenderPointF from, ArRenderPointF to) {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float length = sqrtf(dx * dx + dy * dy);
  if (length <= 0.00001f) return (ArRenderPointF){0.0f, 0.0f};
  /* The perimeter is clockwise in screen coordinates, where Y grows down.
   * Rotating its tangent this way points away from the grid interior. */
  return (ArRenderPointF){dy / length, -dx / length};
}

static float Clampf(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

static ArRenderPointF FringeOffset(
    ArRenderPointF previous, ArRenderPointF current, ArRenderPointF next,
    bool previous_active, bool next_active, float width) {
  const ArRenderPointF previous_normal =
      OutwardNormal(previous, current);
  const ArRenderPointF next_normal = OutwardNormal(current, next);
  if (!previous_active && !next_active)
    return (ArRenderPointF){0.0f, 0.0f};
  if (!previous_active)
    return (ArRenderPointF){next_normal.x * width, next_normal.y * width};
  if (!next_active)
    return (ArRenderPointF){previous_normal.x * width,
                            previous_normal.y * width};

  float mx = previous_normal.x + next_normal.x;
  float my = previous_normal.y + next_normal.y;
  const float miter_length = sqrtf(mx * mx + my * my);
  if (miter_length <= 0.00001f)
    return (ArRenderPointF){next_normal.x * width, next_normal.y * width};
  mx /= miter_length;
  my /= miter_length;
  float denominator = mx * next_normal.x + my * next_normal.y;
  if (denominator < 0.25f) denominator = 0.25f;
  float distance = width / denominator;
  if (distance > width * 4.0f) distance = width * 4.0f;
  return (ArRenderPointF){mx * distance, my * distance};
}

bool DioramaEdgeAa_BuildFringe(
    const ArRenderVertex2D *grid, int subdiv_x, int subdiv_y,
    float width_pixels, DioramaEdgeAaMask edge_mask,
    float u_min, float u_max, float v_min, float v_max,
    float texel_width, float texel_height,
    ArRenderVertex2D *out_vertices, int vertex_capacity,
    int32_t *out_indices, int index_capacity,
    int *out_vertex_count, int *out_index_count) {
  if (out_vertex_count) *out_vertex_count = 0;
  if (out_index_count) *out_index_count = 0;
  const int boundary_count = BoundaryPointCount(subdiv_x, subdiv_y);
  const int needed_vertices = 2 * boundary_count;
  int enabled_segments = 0;
  for (int segment = 0; segment < boundary_count; segment++) {
    if (edge_mask & BoundarySegmentEdge(segment, subdiv_x, subdiv_y))
      enabled_segments++;
  }
  const int needed_indices = 6 * enabled_segments;
  if (!grid || !out_vertices || !out_indices || !out_vertex_count ||
      !out_index_count || boundary_count <= 0 || width_pixels <= 0.0f ||
      texel_width <= 0.0f || texel_height <= 0.0f ||
      u_max - u_min <= texel_width || v_max - v_min <= texel_height ||
      vertex_capacity < needed_vertices || index_capacity < needed_indices)
    return false;

  const float sample_u_min = u_min + 0.5f * texel_width;
  const float sample_u_max = u_max - 0.5f * texel_width;
  const float sample_v_min = v_min + 0.5f * texel_height;
  const float sample_v_max = v_max - 0.5f * texel_height;
  for (int point = 0; point < boundary_count; point++) {
    const int previous_point =
        point > 0 ? point - 1 : boundary_count - 1;
    const int next_point =
        point + 1 < boundary_count ? point + 1 : 0;
    const int previous_segment = previous_point;
    const int next_segment = point;
    const ArRenderVertex2D source =
        grid[BoundaryGridIndex(point, subdiv_x, subdiv_y)];
    const ArRenderPointF previous =
        grid[BoundaryGridIndex(previous_point, subdiv_x, subdiv_y)].position;
    const ArRenderPointF next =
        grid[BoundaryGridIndex(next_point, subdiv_x, subdiv_y)].position;
    const ArRenderPointF offset = FringeOffset(
        previous, source.position, next,
        (edge_mask & BoundarySegmentEdge(
             previous_segment, subdiv_x, subdiv_y)) != 0,
        (edge_mask & BoundarySegmentEdge(
             next_segment, subdiv_x, subdiv_y)) != 0,
        width_pixels);

    ArRenderVertex2D inner = source;
    inner.tex_coord.x = Clampf(
        inner.tex_coord.x, sample_u_min, sample_u_max);
    inner.tex_coord.y = Clampf(
        inner.tex_coord.y, sample_v_min, sample_v_max);
    ArRenderVertex2D outer = inner;
    outer.position.x += offset.x;
    outer.position.y += offset.y;
    outer.color.a = 0.0f;
    out_vertices[2 * point] = inner;
    out_vertices[2 * point + 1] = outer;
  }

  int index_count = 0;
  for (int segment = 0; segment < boundary_count; segment++) {
    if (!(edge_mask & BoundarySegmentEdge(
            segment, subdiv_x, subdiv_y)))
      continue;
    const int next = segment + 1 < boundary_count ? segment + 1 : 0;
    const int inner0 = 2 * segment;
    const int outer0 = inner0 + 1;
    const int inner1 = 2 * next;
    const int outer1 = inner1 + 1;
    out_indices[index_count++] = inner0;
    out_indices[index_count++] = inner1;
    out_indices[index_count++] = outer0;
    out_indices[index_count++] = inner1;
    out_indices[index_count++] = outer1;
    out_indices[index_count++] = outer0;
  }
  *out_vertex_count = needed_vertices;
  *out_index_count = index_count;
  return true;
}
