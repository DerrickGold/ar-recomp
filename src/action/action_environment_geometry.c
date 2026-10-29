#include "render/cloud_mesh.h"
/* Shared environment mesh clipping and soft light patches. Phase: pure. */
#include "action/action_effect_render_internal.h"
#include "action_effect_members.h"
#include "action_light_kinds.h"
#include "action_storm.h"

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
      .tex_coord = {a.vertex.tex_coord.x+(b.vertex.tex_coord.x-a.vertex.tex_coord.x)*t,
                    a.vertex.tex_coord.y+(b.vertex.tex_coord.y-a.vertex.tex_coord.y)*t},
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

/* Keep the same diagonal on CPU and GPU. Re-triangulating the clipped quad
 * changes interpolated texture coordinates when the scene is tilted. */
bool AppendSceneClippedQuad(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *mesh,const ArRenderVertex2D source[4],
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  int mapped[4]={-1,-1,-1,-1};
  const int triangles[2][3]={{0,1,2},{0,2,3}};
  return AppendSceneClippedTriangle(writer,mesh,source,mapped,triangles[0],clip,project,context)&&
      AppendSceneClippedTriangle(writer,mesh,source,mapped,triangles[1],clip,project,context);
}

bool AppendSceneSoftPatch(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry,
    ArRenderColorF color, float lean, ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderVertex2D vertices[kCloudPatchVertices];
  int mapped[kCloudPatchVertices];
  CloudMesh_Patch(vertices,x,y,rx,ry,color,lean);
  for(unsigned i=0;i<kCloudPatchVertices;++i)mapped[i]=-1;
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

bool ActionEffectSource_ParticleViewBounds(const ActionEffectProjectionContext *input,
    const ActionEffectInstance *effect, ActionEffectLocalRect *bounds) {
  if (!input) return false;
  ActionEffectProjectionContext context = *input;
  DioramaProjection projection;
  if (context.diorama_projection) {
    projection = *context.diorama_projection;
    /* One retained packet serves all skybox bands in the same capture. */
    projection.bg2_skybox.active_band = -1;
    context.diorama_projection = &projection;
  }
  const DioramaSkyboxSourceMapping *sky=context.diorama_projection?
      &context.diorama_projection->bg2_skybox.resident:NULL;
  if(sky&&sky->meta[0]>0&&!context.diorama_projection->bg2_plane.valid&&
      effect->projection_plane==kActionEffectProjectionPlane_Bg2) {
    /* Resident bands are fitted on the GPU and have no CPU band rectangles.
     * Their fitted window stays inside the raw U ranges and vertical follow
     * interval. Cull against that conservative envelope, never guessed bands
     * or a GPU readback. This also covers a static anchor spanning many rows. */
    if(sky->meta[0]>kDioramaSkyboxSourceBands||sky->meta[1]<=0||sky->meta[2]<=0)return false;
    float left=INFINITY,right=-INFINITY;
    for(unsigned i=0;i<(unsigned)sky->meta[0];++i)if(sky->bands[i][1]>sky->bands[i][0]) {
      left=fminf(left,sky->bands[i][0]*sky->meta[1]);
      right=fmaxf(right,sky->bands[i][1]*sky->meta[1]);
    }
    const float top=sky->fit[0]>0?sky->follow[0]:sky->vertical[2]*sky->meta[2];
    const float bottom=sky->fit[0]>0?sky->follow[1]+sky->fit[0]:sky->vertical[3]*sky->meta[2];
    const ArRenderPointF origin=ActionEffectProjection_CaptureOrigin(&context,effect);
    *bounds=(ActionEffectLocalRect){left-sky->output[2]-origin.x,top-sky->output[3]-origin.y,
        right-sky->output[2]-origin.x,bottom-sky->output[3]-origin.y};
    if(!RectIsSane(bounds)||bounds->x0>=bounds->x1||bounds->y0>=bounds->y1)return false;
  } else if (!ActionEffectProjection_ViewBounds(&context, effect, bounds)) return false;
  /* Only bound particle birth-cell work here. This apron covers the resident
   * global-motion endpoints (+/-7 pixels) and their interpolated displacement
   * (up to 10.5). GPU clipping still decides visibility on every repaint.
   * ParticleArea separately backtracks travel/wander; no motion download. */
  const float apron = 16;
  bounds->x0 -= apron;
  bounds->y0 -= apron;
  bounds->x1 += apron;
  bounds->y1 += apron;
  return true;
}

bool ActionEffectSource_UsesViewBounds(const ActionEffectInstance *effect) {
  return effect&&(effect->kind==kActionEffect_AuthoredParticleArea||
      effect->kind==kActionEffect_AuthoredStorm||
      (effect->kind==kActionEffect_AuthoredFan&&effect->projection_plane==kActionEffectProjectionPlane_Bg2&&
       (effect->flags&kActionEffectFlag_StaticAnchor)&&!effect->field_style.shadows)||
      (effect->kind==kActionEffect_AuthoredCloud&&effect->field_style.rolling>0));
}

bool ActionEffectSource_ClipBounds(void *userdata, const ActionEffectInstance *effect,
    ActionEffectLocalRect *bounds) {
  if (!effect || !bounds || effect->geometry.kind != kActionEffectGeometry_Rect) return false;
  *bounds = effect->geometry.data.rect;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    bounds->x0 = fmaxf(bounds->x0, effect->clip_rect.x0);
    bounds->y0 = fmaxf(bounds->y0, effect->clip_rect.y0);
    bounds->x1 = fminf(bounds->x1, effect->clip_rect.x1);
    bounds->y1 = fminf(bounds->y1, effect->clip_rect.y1);
  }
  if (ActionEffectSource_UsesViewBounds(effect)) {
    /* Long weather banks otherwise resubmit the entire level's masks for
     * every skybox raster band. Admit only patches intersecting the union of
     * visible bands plus the same motion apron used by particle areas. Their
     * seeded positions remain world-relative; GPU clipping still handles
     * each band and repaint. */
    const ActionEffectSourceBatch *source = userdata;
    if (!source) return false;
    ActionEffectLocalRect view;
    if (!ActionEffectSource_ParticleViewBounds(&source->context, effect, &view)) return false;
    bounds->x0 = fmaxf(bounds->x0, view.x0);
    bounds->y0 = fmaxf(bounds->y0, view.y0);
    bounds->x1 = fminf(bounds->x1, view.x1);
    bounds->y1 = fminf(bounds->y1, view.y1);
  }
  return RectIsSane(bounds) && bounds->x0 < bounds->x1 && bounds->y0 < bounds->y1;
}

static ActionEffectSourcePrimitive *SourcePrimitive(ActionEffectSourceBatch *b,
    const ActionEffectInstance *e, unsigned kind) {
  if (b->failed || b->count >= b->capacity || !b->primitives) {
    b->failed = true;
    return NULL;
  }
  const ArRenderPointF origin = ActionEffectProjection_CaptureOrigin(&b->context, e);
  ActionEffectSourcePrimitive *p = &b->primitives[b->count++];
  const bool weather=(e->kind==kActionEffect_AuthoredCloud&&e->field_style.textured)||
      e->kind==kActionEffect_AuthoredStorm||
      (e->kind==kActionEffect_AuthoredFan&&!e->field_style.shadows);
  b->weather_only=weather&&(b->count==1||b->weather_only);
  *p = (ActionEffectSourcePrimitive){
    .meta = {kind, e->projection_plane == kActionEffectProjectionPlane_Obj ?
        (e->obj_priority ? 5u + e->obj_priority : 0u) : e->projection_plane, e->obj_priority, 0},
    .origin = {origin.x, origin.y,
               (e->render_layer == kActionEffectRenderLayer_Atmosphere ? 1 : 0) |
               (e->flags & kActionEffectFlag_StaticAnchor ? 2 : 0) |
               (e->flags & kActionEffectFlag_ClippedMesh ? 4 : 0) |
               (ActionStorm_CustomParallax(e) ? 16 : 0), e->field_style.storm.parallax_x},
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
    p->points[i][2] = a->tex_coord.x; p->points[i][3] = a->tex_coord.y;
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

bool ActionEffectSource_RainbowTriangle(ActionEffectSourceBatch *b,const ActionEffectInstance *e,
    const ArRenderVertex2D *v,const int *triangle,const ActionEffectLocalRect *clip,
    const float directions[3][4]) {
  if(!ActionEffectSource_Triangle(b,e,v,triangle,clip))return false;
  ActionEffectSourcePrimitive *p=&b->primitives[b->count-1];
  p->meta[0]=kActionSourceRainbowTriangle;
  memcpy(p->points+3,directions,3*sizeof(p->points[0]));
  p->extra[0]=1; /* Presentation fade, applied after optical response. */
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

void ActionEffectSource_Shadow(ActionEffectSourceBatch *b, unsigned begin, float x, float y, bool multiply, float strength) {
  for (unsigned i = begin; i < b->count; ++i) {
    b->primitives[i].meta[3] = multiply ? 1 : 2;
    b->primitives[i].extra[0] = x; b->primitives[i].extra[1] = y;
    b->primitives[i].extra[2] = strength;
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
  const float gain = multiply ? intensity : ActionLightIntensity_Gain(intensity);
  const float opacity = ActionLightIntensity_Opacity(intensity);
  for (unsigned i = begin; i < end; ++i) for (unsigned j = 0;
      j < (b->primitives[i].meta[0] == kActionSourceQuad ? 4u : 3u); ++j) {
    float *c = j < 3 ? b->primitives[i].colors[j] : b->primitives[i].extra;
    c[0] *= ((color >> 16) & 255) / 255.f;
    c[1] *= ((color >> 8) & 255) / 255.f;
    c[2] *= (color & 255) / 255.f;
    if (gain != 1) { c[0] *= gain; c[1] *= gain; c[2] *= gain; }
    if (!multiply) {
      if (b->primitives[i].meta[0] == kActionSourceLitTriangle) {
        c[3] *= opacity;
        b->primitives[i].points[j][3] *= opacity;
        if (j == 0) b->primitives[i].extra[0] = fminf(1, b->primitives[i].extra[0] * opacity);
      } else c[3] = fminf(1, c[3] * opacity);
    }
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

/* Glows and cloud puffs are finite areas, not points. Keep their complete
 * animated extent when testing a capture edge; the anchor and unit sizing
 * probes may lie outside it. The shared coefficients retain the same shape,
 * screen minima and vertex budget on the CPU and resident GPU paths. */
bool AppendBillboard(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    float x, float y, float rx, float ry, float min_x, float min_y, unsigned segments,
    const float (*corners)[4], const ArRenderColorF *colors,
    ActionEffectProjectPointFn project, void *context) {
  const unsigned count = 1+kActionEffectGlowRings*segments;
  float extent[4] = {0};
  for (unsigned i=0;i<count;++i) for (unsigned j=0;j<4;++j)
    extent[j]=fmaxf(extent[j],fabsf(corners[i][j]));
  const int base=writer->vertex_count;
  if (!writer->source) {
    ArRenderPointF anchor;
    float sx,sy;
    const bool native=project==ActionEffectProjection_ProjectPoint;
    if (!ProjectWithScale(effect,native?ActionEffectProjection_ProjectBillboardPoint:project,
            context,x,y,&anchor,&sx,&sy)) return true;
    const float radius_x=fmaxf(min_x,rx*sx),radius_y=fmaxf(min_y,ry*sy);
    if (native) {
      ActionEffectLocalRect bounds;
      if (!ActionEffectProjection_BillboardBounds(context,effect,&bounds)) return true;
      const float ex=(extent[0]*radius_x+extent[1]*radius_y)/sx;
      const float ey=(extent[2]*radius_x+extent[3]*radius_y)/sy;
      if (x+ex<bounds.x0 || x-ex>bounds.x1 || y+ey<bounds.y0 || y-ey>bounds.y1) return true;
    }
    if (!Reserve(writer,count,segments*3*(2*kActionEffectGlowRings-1))) return false;
    for (unsigned i=0;i<count;++i) {
      const float *c=corners[i];
      writer->vertices[writer->vertex_count++]=(ArRenderVertex2D){
        .position={anchor.x+c[0]*radius_x+c[1]*radius_y,
                   anchor.y+c[2]*radius_x+c[3]*radius_y},
        .color=colors[i?1+(i-1)/segments:0]};
    }
  }
  for (unsigned r = 0; r < kActionEffectGlowRings; ++r) for (unsigned s = 0; s < segments; ++s) {
    const unsigned next = (s + 1) % segments;
    const unsigned outer0 = 1 + r * segments + s, outer1 = 1 + r * segments + next;
    const unsigned inner0 = r ? outer0 - segments : 0, inner1 = r ? outer1 - segments : 0;
    const unsigned triangles[6] = {inner0, outer0, outer1, inner0, outer1, inner1};
    for (unsigned t = 0; t < (r ? 2u : 1u); ++t) {
      if (!writer->source) {
        for (unsigned j=0;j<3;++j) writer->indices[writer->index_count++]=base+triangles[t*3+j];
        continue;
      }
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
      memcpy(p->points[5],extent,sizeof(extent));
    }
  }
  if (writer->source) writer->vertex_count = (int)writer->source->count;
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
