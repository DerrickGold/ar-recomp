/* Shared environment mesh clipping and soft light patches. Phase: pure. */
#include "action/action_effect_render_internal.h"

typedef struct SceneClipVertex {
  ArRenderVertex2D vertex; /* Position is still effect-local, before projection. */
  int source_index;
} SceneClipVertex;

static unsigned SceneClipCode(ArRenderPointF point, const ActionEffectLocalRect *clip) {
  return (point.x < clip->x0 ? 1u : 0) | (point.x > clip->x1 ? 2u : 0) |
      (point.y < clip->y0 ? 4u : 0) | (point.y > clip->y1 ? 8u : 0);
}

static float SceneClipDistance(
    ArRenderPointF point, const ActionEffectLocalRect *clip, int edge) {
  switch (edge) {
    case 0: return point.x - clip->x0;
    case 1: return clip->x1 - point.x;
    case 2: return point.y - clip->y0;
    default: return clip->y1 - point.y;
  }
}

static SceneClipVertex SceneClipIntersection(
    SceneClipVertex a, SceneClipVertex b, const ActionEffectLocalRect *clip, int edge) {
  /* Use the same endpoint order on shared edges, preventing cracks from
   * opposite-direction floating-point interpolation in adjacent triangles. */
  if (a.vertex.position.x > b.vertex.position.x ||
      (a.vertex.position.x == b.vertex.position.x && a.vertex.position.y > b.vertex.position.y)) {
    const SceneClipVertex swap = a;
    a = b;
    b = swap;
  }
  const float da = SceneClipDistance(a.vertex.position, clip, edge);
  const float db = SceneClipDistance(b.vertex.position, clip, edge);
  const float t = da / (da - db);
  SceneClipVertex result = {
    .vertex = {
      .position = {a.vertex.position.x + (b.vertex.position.x - a.vertex.position.x) * t,
                   a.vertex.position.y + (b.vertex.position.y - a.vertex.position.y) * t},
      .color = MixColor(a.vertex.color, b.vertex.color, t),
    },
    .source_index = t == 0 ? a.source_index : (t == 1 ? b.source_index : -1),
  };
  /* The clipped coordinate is exact; the other coordinate and color retain
   * the intersection with the original diagonal, rather than being clamped. */
  if (edge == 0) result.vertex.position.x = clip->x0;
  if (edge == 1) result.vertex.position.x = clip->x1;
  if (edge == 2) result.vertex.position.y = clip->y0;
  if (edge == 3) result.vertex.position.y = clip->y1;
  return result;
}

static bool SceneClipPush(SceneClipVertex *vertices, int *count, SceneClipVertex vertex) {
  if (*count && vertices[*count - 1].vertex.position.x == vertex.vertex.position.x &&
      vertices[*count - 1].vertex.position.y == vertex.vertex.position.y)
    return true;
  if (*count >= 8) return false;
  vertices[(*count)++] = vertex;
  return true;
}

bool AppendSceneClippedTriangle(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ArRenderVertex2D *source, int *mapped, const int *triangle,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project_point, void *userdata) {
  SceneClipVertex polygon[8], scratch[8];
  unsigned common = 15u, any = 0;
  int count = 3;
  for (int i = 0; i < 3; i++) {
    polygon[i] = (SceneClipVertex){source[triangle[i]], triangle[i]};
    const unsigned code = SceneClipCode(polygon[i].vertex.position, clip);
    common &= code;
    any |= code;
  }
  if (common) return true;
  /* Interior triangles keep their original vertices and shared indices.
   * Only boundary triangles need polygon clipping and color interpolation. */
  for (int edge = 0; any && edge < 4 && count; edge++) {
    int next_count = 0;
    for (int i = 0; i < count; i++) {
      const SceneClipVertex a = polygon[i], b = polygon[(i + 1) % count];
      const bool a_inside = SceneClipDistance(a.vertex.position, clip, edge) >= 0;
      const bool b_inside = SceneClipDistance(b.vertex.position, clip, edge) >= 0;
      if (a_inside != b_inside && !SceneClipPush(scratch, &next_count,
              SceneClipIntersection(a, b, clip, edge)))
        return false;
      if (b_inside && !SceneClipPush(scratch, &next_count, b)) return false;
    }
    if (next_count > 1 &&
        scratch[0].vertex.position.x == scratch[next_count - 1].vertex.position.x &&
        scratch[0].vertex.position.y == scratch[next_count - 1].vertex.position.y)
      next_count--;
    memcpy(polygon, scratch, (size_t)next_count * sizeof(polygon[0]));
    count = next_count;
  }
  if (count < 3) return true;
  /* A triangle intersected with a rectangle has at most seven vertices. */
  if (count > 7 || !Reserve(writer, count, (count - 2) * 3)) return false;
  for (int i = 0; i < count; i++) {
    const int source_index = polygon[i].source_index;
    if (source_index >= 0 && mapped[source_index] >= 0) continue;
    ArRenderPointF projected;
    if (!project_point(userdata, mesh, polygon[i].vertex.position.x,
            polygon[i].vertex.position.y, &projected))
      return true;
    polygon[i].vertex.position = projected;
  }
  int indices[7];
  for (int i = 0; i < count; i++) {
    const int source_index = polygon[i].source_index;
    if (source_index >= 0 && mapped[source_index] >= 0) {
      indices[i] = mapped[source_index];
    } else {
      indices[i] = writer->vertex_count;
      if (source_index >= 0) mapped[source_index] = writer->vertex_count;
      writer->vertices[writer->vertex_count++] = polygon[i].vertex;
    }
  }
  for (int i = 1; i < count - 1; i++) {
    writer->indices[writer->index_count++] = indices[0];
    writer->indices[writer->index_count++] = indices[i];
    writer->indices[writer->index_count++] = indices[i + 1];
  }
  return true;
}

bool AppendSceneSoftPatch(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry,
    ArRenderColorF color, float lean, ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderVertex2D vertices[35];
  int mapped[35];
  for (int row = 0; row < 5; row++) {
    const float v = (row-2)*.5f;
    for (int col = 0; col < 7; col++) {
      const float u = (col-3)/3.0f;
      const int at = row*7+col;
      vertices[at] = (ArRenderVertex2D){
        {x + u*rx + v*lean, y + v*ry},
        {color.r,color.g,color.b,color.a*SceneSoftFalloff(u)*SceneSoftFalloff(v)}, {0,0},
      };
      mapped[at] = -1;
    }
  }
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 6; col++) {
      const int a = row*7+col, b = a+7;
      const int triangles[] = {a,a+1,b,a+1,b+1,b};
      for (int t = 0; t < 6; t += 3)
        if (!AppendSceneClippedTriangle(writer,effect,vertices,mapped,&triangles[t],
                clip,project_point,userdata)) return false;
    }
  }
  return true;
}
