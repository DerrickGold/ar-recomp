/* Shared environment mesh clipping and soft light patches. Phase: pure. */
#include "action/action_effect_render_internal.h"
#include "action_effect_members.h"

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
  if (writer->source) {
    const bool ok = ActionEffectSource_Triangle(writer->source, mesh, source, triangle, clip);
    writer->vertex_count = (int)writer->source->count;
    return ok;
  }
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

/* Packet construction deliberately keeps authored coordinates separate from
 * capture origin. GPU clipping follows the reference's local-space arithmetic
 * and does not round a boundary by adding/subtracting world coordinates. */
_Static_assert(sizeof(ActionEffectSourcePrimitive) == 208, "source primitive std430");

bool ActionEffectSource_ProjectPoint(void *userdata, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point) {
  (void)effect; (void)x; (void)y; (void)point;
  if (userdata) ((ActionEffectSourceBatch *)userdata)->failed = true;
  return false;
}

bool ActionEffectSource_ClipBounds(void *userdata, const ActionEffectInstance *effect,
    ActionEffectLocalRect *bounds) {
  (void)userdata;
  *bounds = effect->geometry.data.rect;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    bounds->x0 = fmaxf(bounds->x0, effect->clip_rect.x0);
    bounds->y0 = fmaxf(bounds->y0, effect->clip_rect.y0);
    bounds->x1 = fminf(bounds->x1, effect->clip_rect.x1);
    bounds->y1 = fminf(bounds->y1, effect->clip_rect.y1);
  }
  return RectIsSane(bounds) && bounds->x0 < bounds->x1 && bounds->y0 < bounds->y1;
}

static ActionEffectSourcePrimitive *SourcePrimitive(ActionEffectSourceBatch *b,
    const ActionEffectInstance *e, unsigned kind) {
  if (b->failed || b->count >= b->capacity || !b->primitives) {
    b->failed = true;
    return NULL;
  }
  const ActionEffectProjectionContext *c = &b->context;
  int cx = c->bg1_camera_x, cy = c->bg1_camera_y;
  if (e->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds) {
    cx = (int16_t)((cx + c->bg2_camera_x) / 2);
    cy = (int16_t)((cy + c->bg2_camera_y) / 2);
  } else if (e->projection_plane == kActionEffectProjectionPlane_Bg2 ||
             e->projection_plane == kActionEffectProjectionPlane_Bg2High) {
    cx = c->bg2_camera_x; cy = c->bg2_camera_y;
  }
  ActionEffectSourcePrimitive *p = &b->primitives[b->count++];
  *p = (ActionEffectSourcePrimitive){
    .meta = {kind, e->projection_plane == kActionEffectProjectionPlane_Obj ?
        (e->obj_priority ? 5u + e->obj_priority : 0u) : e->projection_plane, e->obj_priority, 0},
    .origin = {c->ws_extra + (int16_t)(uint16_t)(e->world_x - cx),
               c->ws_extra_top + (int16_t)(uint16_t)(e->world_y - cy),
               (e->render_layer == kActionEffectRenderLayer_Atmosphere ? 1 : 0) |
               (e->flags & kActionEffectFlag_StaticAnchor ? 2 : 0) |
               (e->flags & kActionEffectFlag_ClippedMesh ? 4 : 0)},
    .clip = {-1e9f, -1e9f, 1e9f, 1e9f}};
  if ((e->flags & kActionEffectFlag_ClipToRect) && !(e->flags & kActionEffectFlag_ClippedMesh)) {
    const ActionEffectLocalRect *r = &e->clip_rect;
    p->clip[0] = r->x0; p->clip[1] = r->y0; p->clip[2] = r->x1; p->clip[3] = r->y1;
  }
  return p;
}

bool ActionEffectSource_Triangle(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    const ArRenderVertex2D *v, const int *triangle, const ActionEffectLocalRect *clip) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, b->lit_triangles ? kActionSourceLitTriangle : kActionSourceTriangle);
  if (!p) return false;
  p->clip[0] = clip->x0; p->clip[1] = clip->y0; p->clip[2] = clip->x1; p->clip[3] = clip->y1;
  for (unsigned i = 0; i < 3; ++i) {
    const ArRenderVertex2D *a = &v[triangle[i]];
    p->points[i][0] = a->position.x; p->points[i][1] = a->position.y;
    memcpy(p->colors[i], &a->color, sizeof(a->color));
    if (b->lit_triangles) {
      p->points[i][2] = b->light_sample ? b->light_sample : a->tex_coord.x;
      p->points[i][3] = b->light_sample ? b->light_base : a->tex_coord.y;
    }
  }
  if (b->lit_triangles) p->extra[0] = b->light_cap;
  return true;
}

bool ActionEffectSource_Particle(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    float x, float y, float px, float py, float width, float reach, ArRenderColorF color) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, kActionSourceParticle);
  if (!p) return false;
  p->points[0][0] = x; p->points[0][1] = y;
  p->points[1][0] = px; p->points[1][1] = py;
  p->extra[0] = width; p->extra[1] = reach;
  memcpy(p->colors[0], &color, sizeof(color));
  return true;
}

bool ActionEffectSource_Leaf(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    const ArRenderPointF points[6], ArRenderColorF color, ArRenderColorF rim) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, kActionSourceLeaf);
  if (!p) return false;
  for (unsigned i = 0; i < 6; ++i) {
    p->points[i][0] = points[i].x; p->points[i][1] = points[i].y;
  }
  memcpy(p->colors[0], &color, sizeof(color));
  memcpy(p->colors[1], &rim, sizeof(rim));
  return true;
}

void ActionEffectSource_Shadow(ActionEffectSourceBatch *b, unsigned begin, float x, float y, bool multiply) {
  for (unsigned i = begin; i < b->count; ++i) {
    b->primitives[i].meta[3] = multiply ? 1 : 2;
    b->primitives[i].extra[0] = x; b->primitives[i].extra[1] = y;
  }
}

bool ActionEffectSource_Quad(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    const ArRenderPointF points[4], const ArRenderColorF colors[4]) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, kActionSourceQuad);
  if (!p) return false;
  for (unsigned i = 0; i < 4; ++i) {
    p->points[i][0] = points[i].x; p->points[i][1] = points[i].y;
    memcpy(i < 3 ? p->colors[i] : p->extra, &colors[i], sizeof(colors[i]));
  }
  return true;
}

bool ActionEffectSource_Star(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    float x, float y, float size, ArRenderColorF color) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, kActionSourceStar);
  if (!p) return false;
  p->points[0][0] = x; p->points[0][1] = y;
  p->extra[0] = size;
  memcpy(p->colors[0], &color, sizeof(color));
  return true;
}

bool ActionEffectSource_GlowTriangle(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    float x, float y, float rx, float ry, const float coefficients[3][4],
    const ArRenderColorF colors[3]) {
  ActionEffectSourcePrimitive *p = SourcePrimitive(b, e, kActionSourceGlowTriangle);
  if (!p) return false;
  p->points[0][0] = x; p->points[0][1] = y;
  p->points[1][0] = rx; p->points[1][1] = ry;
  memcpy(p->points[2], coefficients, 3 * sizeof(p->points[0]));
  memcpy(p->colors, colors, sizeof(p->colors));
  return true;
}

void ActionEffectSource_TintRange(ActionEffectSourceBatch *b, unsigned begin, unsigned end, uint32_t color,
    float intensity, bool multiply) {
  if (end > b->count) { b->failed = true; return; }
  for (unsigned i = begin; i < end; ++i) for (unsigned j = 0;
      j < (b->primitives[i].meta[0] == kActionSourceQuad ? 4u : 3u); ++j) {
    float *c = j < 3 ? b->primitives[i].colors[j] : b->primitives[i].extra;
    c[0] *= ((color >> 16) & 255) / 255.f;
    c[1] *= ((color >> 8) & 255) / 255.f;
    c[2] *= (color & 255) / 255.f;
    if (multiply) { c[0] *= intensity; c[1] *= intensity; c[2] *= intensity; }
    else if (b->primitives[i].meta[0] == kActionSourceLitTriangle) {
      c[3] *= intensity;
      b->primitives[i].points[j][3] *= intensity;
      if (j == 0) b->primitives[i].extra[0] = fminf(1, b->primitives[i].extra[0] * intensity);
    } else c[3] = fminf(1, c[3] * intensity);
  }
}

void ActionEffectSource_Tint(ActionEffectSourceBatch *b, unsigned begin, uint32_t color,
    float intensity, bool multiply) {
  ActionEffectSource_TintRange(b, begin, b->count, color, intensity, multiply);
}

void TintEffectMember(ActionEffectGeometryWriter *writer, const ActionNativeMember *member,
    int begin, int end, bool multiply) {
  if (!member) return;
  if (writer->source)
    ActionEffectSource_TintRange(writer->source, begin, end, member->color, member->intensity, multiply);
  else ActionEffectMembers_Tint(member, writer->vertices, begin, end, multiply);
}

/* Ring geometry and colors come from the same recipe as the reference path.
 * The GPU supplies only the anchor and projected radii, including screen minima. */
bool AppendSourceBillboard(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float x, float y, float rx, float ry, float min_x, float min_y, unsigned segments,
    const float (*corners)[4], const ArRenderColorF *colors) {
  for (unsigned r = 0; r < kActionEffectGlowRings; ++r) for (unsigned s = 0; s < segments; ++s) {
    const unsigned next = (s + 1) % segments;
    const unsigned outer0 = 1 + r * segments + s, outer1 = 1 + r * segments + next;
    const unsigned inner0 = r ? outer0 - segments : 0, inner1 = r ? outer1 - segments : 0;
    const unsigned triangles[6] = {inner0, outer0, outer1, inner0, outer1, inner1};
    for (unsigned t = 0; t < (r ? 2u : 1u); ++t) {
      float coefficients[3][4];
      ArRenderColorF palette[3];
      for (unsigned j = 0; j < 3; ++j) {
        const unsigned at = triangles[t * 3 + j];
        memcpy(coefficients[j], corners[at], sizeof(coefficients[j]));
        palette[j] = colors[at ? 1 + (at - 1) / segments : 0];
      }
      if (!ActionEffectSource_GlowTriangle(writer->source, effect, x, y, rx, ry,
              coefficients, palette)) return false;
      ActionEffectSourcePrimitive *p = &writer->source->primitives[writer->source->count - 1];
      p->points[1][2] = min_x; p->points[1][3] = min_y;
    }
  }
  writer->vertex_count = (int)writer->source->count;
  return true;
}

bool ActionEffectSource_Ribbon(ActionEffectSourceBatch *batch, const ActionEffectInstance *effect,
    const ArRenderPointF *points, unsigned count, float width, float taper, ArRenderColorF color) {
  if (count < 2 || count > 64) { batch->failed = true; return false; }
  const unsigned first = batch->count;
  for (unsigned i = 0; i + 1 < count; ++i) {
    ActionEffectSourcePrimitive *p = SourcePrimitive(batch, effect, kActionSourceRibbon);
    if (!p) return false;
    p->points[0][0] = points[i].x; p->points[0][1] = points[i].y;
    p->points[1][0] = points[i + 1].x; p->points[1][1] = points[i + 1].y;
    p->extra[0] = width * (i ? 1 : taper);
    p->extra[1] = width * (i + 2 == count ? taper : 1);
    p->extra[2] = first; p->extra[3] = count - 1;
    memcpy(p->colors[0], &color, sizeof(color));
  }
  return true;
}

ActionEffectSourceLightJob *ActionEffectSource_BeginLight(ActionEffectSourceBatch *b,
    const ActionEffectInstance *light, const ActionEffectInstance *receiver, const ActionMoonField *f,
    const ActionMoonlightOcclusion *occlusion, unsigned kind, const ActionEffectLocalRect *clip) {
  if (!b->lights || b->light_count >= b->light_capacity || kind > 3) {
    b->failed = true; return NULL;
  }
  /* Reuse the canonical capture-origin mapping without emitting geometry. */
  ActionEffectSourcePrimitive mapping[2];
  ActionEffectSourceBatch temporary = {.context=b->context,.primitives=mapping,.capacity=2};
  SourcePrimitive(&temporary,light,kActionSourceTriangle);
  SourcePrimitive(&temporary,receiver,kActionSourceTriangle);
  ActionEffectSourceLightJob *job=&b->lights[b->light_count++];
  *job=(ActionEffectSourceLightJob){.occlusion=occlusion};
  ActionEffectSourceLightData *d=&job->data;
  d->meta[1]=kind; d->meta[2]=f->RayShape[1];
  for(unsigned i=0;i<2;++i) {
    float *out=i?d->receiver:d->light;
    out[0]=mapping[i].origin[0];out[1]=mapping[i].origin[1];
    out[2]=mapping[i].meta[1];out[3]=mapping[i].origin[2];
  }
  memcpy(d->clip,clip,sizeof(*clip));
  d->filter[0]=receiver->world_x;
  memcpy(d->transport,f->Shadow+1,3*sizeof(float));
  memcpy(d->surface,f->Shadow+4,2*sizeof(float));
  for(unsigned i=0;i<6;++i)memcpy(d->rays[i],f->low[i],3*sizeof(float));
  for(unsigned i=0;i<9;++i) {
    const float radius=f->Shadow[0]*sqrtf((i+.5f)/9),angle=i*2.39996323f;
    d->sources[i][0]=radius*cosf(angle);d->sources[i][1]=radius*sinf(angle);
  }
  return job;
}

bool ActionEffectSource_FloorTriangle(ActionEffectSourceBatch *b, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, const ArRenderPointF corners[3], float floor, float height,
    float phase, float seed, unsigned slice, ArRenderColorF tint) {
  ActionEffectSourcePrimitive *p=SourcePrimitive(b,e,kActionSourceFloorTriangle);
  if(!p)return false;
  memcpy(p->clip,clip,sizeof(*clip));
  for(unsigned i=0;i<3;++i) {p->points[i][0]=corners[i].x;p->points[i][1]=corners[i].y;}
  p->points[3][0]=floor;p->points[3][1]=height;p->points[3][2]=phase;p->points[3][3]=seed;
  p->points[4][0]=slice;p->points[4][1]=e->world_x;p->points[4][2]=e->tuning.intensity;
  p->points[5][0]=e->geometry.data.rect.x0;p->points[5][1]=e->geometry.data.rect.x1;
  memcpy(p->colors[0],&tint,sizeof(tint));
  return true;
}
