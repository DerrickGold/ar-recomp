/* Shallow water on an exposed cap, with a short spill down its front. The
 * map still owns the support, silhouette and native art. */
#include "action/action_effect_members.h"
#include "diorama_voxel_cull.h"

/* A zoomed-out Northwall room has over 1,000 reviewed ice cap strips. Reserve
 * room for their three-part finish and sparse glints without changing draws. */
enum { kWetSurfaceMaxQuads = 4096 };

typedef struct WetSurfacePatch {
  float x, y, half_width, depth, spill, phase;
  ArRenderColorF body, lip;
  const ActionCaveWetSource *contour;
  bool pool;
} WetSurfacePatch;

static struct {
  unsigned patches, quads;
  float left, right, top, bottom;
  WetSurfacePatch patch[kActionWaterFieldMaxWet + kActionAuthoredMaxInstances];
  ArRenderVertex2D vertices[kWetSurfaceMaxQuads * 4];
  float depths[kWetSurfaceMaxQuads * 4], clip_w[kWetSurfaceMaxQuads * 4];
  int32_t indices[kWetSurfaceMaxQuads * 6];
} s_wet_surface;

static void WetSurfaceAdd(WetSurfacePatch patch) {
  if (patch.half_width <= 0 || patch.body.a <= 0 ||
      s_wet_surface.patches == sizeof(s_wet_surface.patch) / sizeof(*s_wet_surface.patch)) return;
  s_wet_surface.patch[s_wet_surface.patches++] = patch;
  s_wet_surface.left = fminf(s_wet_surface.left, patch.x - patch.half_width);
  s_wet_surface.right = fmaxf(s_wet_surface.right, patch.x + patch.half_width);
  const float margin = patch.contour ? 24 : 7;
  s_wet_surface.top = fminf(s_wet_surface.top, patch.y - margin);
  s_wet_surface.bottom = fmaxf(s_wet_surface.bottom, patch.y + margin);
}

static void WetSurfacePrepare(const DioramaScene *scene, const DioramaViewGeometry *g) {
  s_wet_surface.patches = s_wet_surface.quads = 0;
  s_wet_surface.left = s_wet_surface.top = INFINITY;
  s_wet_surface.right = s_wet_surface.bottom = -INFINITY;
  const ActionSceneEffectFrame *frame = scene->trunk_effects;
  if (!frame || !g->depth_sink || scene->render->voxel_inspection == kDioramaVoxelInspect_Backing ||
      !ActionVoxel_Active(scene->voxel_geometry, scene->map_group, scene->map_number)) return;
  const float fade = 1.f - scene->native_bloom_dimming;
  const ActionVoxelProfile profile = ActionVoxel_Profile(scene->voxel_geometry);
  if (profile == kActionVoxelProfile_FillmoreCave && scene->trunk_lighting && frame->water_field_valid &&
      !frame->decoration_overflow && frame->decoration_count <= kActionSceneDecorationMaxInstances) {
    const ActionWaterField *field = &frame->water_field;
    for (unsigned i = 0; i < frame->decoration_count; ++i) {
      const ActionEffectInstance *e = &frame->decorations[i];
      if (e->kind != kActionEffect_CaveSheen || !(e->flags & kActionEffectFlag_Visible) ||
          (e->flags & kActionEffectFlag_LightingOff)) continue;
      for (unsigned j = 0; j < (unsigned)field->Counts[2] && j < kActionWaterFieldMaxWet; ++j) {
        const ActionCaveWetSource *source = &field->wet[j];
        if (!(e->source_mask & (1u << j)) || source->water) continue;
        const ActionNativeMember *member = ActionEffectMembers_Find(&frame->members, e->kind, j);
        if (member && !member->enabled) continue;
        const float *body = field->SheenBody, *lip = field->SheenLip;
        ArRenderVertex2D tint[2] = {{.color={body[0],body[1],body[2],body[3]}},
            {.color={lip[0],lip[1],lip[2],lip[3]}}};
        ActionEffectMembers_Tint(member, tint, 0, 2, false);
        tint[0].color.a = fminf(.55f, tint[0].color.a * fade);
        tint[1].color.a = fminf(.40f, tint[1].color.a * fade);
        WetSurfaceAdd((WetSurfacePatch){
          .x = source->x + (member ? roundf(member->offset_x) : 0),
          .y = source->landing_y + (member ? roundf(member->offset_y) : 0),
          .half_width = 16 * (member ? member->width_scale : 1),
          .depth = .30f, .spill = 3 * (member ? member->length_scale : 1),
          .phase = e->phase_ticks * field->SheenMotion[1] + j,
          .body = tint[0].color, .lip = tint[1].color, .contour = source,
        });
      }
    }
  }
  if (!scene->trunk_particles || frame->authored_count > kActionAuthoredMaxInstances) return;
  for (unsigned i = 0; i < frame->authored_count; ++i) {
    const ActionEffectInstance *e = &frame->authored[i];
    if (!(e->flags & kActionEffectFlag_Visible) || e->projection_plane != kActionEffectProjectionPlane_Bg1) continue;
    const bool jungle_drip = profile == kActionVoxelProfile_MarahnaJungle &&
        e->kind == kActionEffect_AuthoredDrips && e->field_style.placement == 2 && !e->field_style.debris;
    const bool wet_edge = e->kind == kActionEffect_AuthoredContour && e->field_style.placement == 1 &&
        e->field_style.point_count == 2 && e->field_style.points[0].y == e->field_style.points[1].y;
    if (!jungle_drip && !wet_edge) continue;
    const uint32_t rgb = e->tuning.color;
    const float strength = fminf(1, e->tuning.intensity * .4f) * fade;
    const ArRenderColorF tint = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
        (rgb & 255) / 255.f, strength};
    const float left = wet_edge ? e->field_style.points[0].x : -12;
    const float right = wet_edge ? e->field_style.points[1].x : 12;
    const float contact_y = wet_edge ? e->field_style.points[0].y : e->geometry.data.rect.y1;
    WetSurfaceAdd((WetSurfacePatch){.x = e->world_x + (left + right) * .5f, .y = e->world_y + contact_y,
        .half_width = (right - left) * .5f, .depth = .23f, .spill = 2.5f,
        .phase = e->pulse_ticks * .035f + i, .pool = e->field_style.ripple != 0,
        .body = {tint.r * .46f, tint.g * .62f, tint.b * .68f, tint.a * .40f},
        .lip = {tint.r, tint.g, tint.b, tint.a * .35f}});
  }
}

static bool WetSurfaceMayTouch(float x0, float x1, float y0, float y1, bool pool) {
  const float top = fminf(y0, y1), bottom = fmaxf(y0, y1);
  if (!s_wet_surface.patches || x1 <= s_wet_surface.left || x0 >= s_wet_surface.right ||
      bottom < s_wet_surface.top || top > s_wet_surface.bottom) return false;
  for (unsigned i = 0; i < s_wet_surface.patches; ++i) {
    const WetSurfacePatch *patch = &s_wet_surface.patch[i];
    const float margin = patch->contour ? 24 : 7;
    if (patch->pool == pool && x1 > patch->x - patch->half_width &&
        x0 < patch->x + patch->half_width && bottom >= patch->y - margin && top <= patch->y + margin)
      return true;
  }
  return false;
}

static float WetSurfaceCoverage(const WetSurfacePatch *patch, float x, float y) {
  const float distance = (x - patch->x) / patch->half_width;
  float landing = patch->y;
  if (patch->contour) {
    const unsigned column = (unsigned)lroundf(fmaxf(0, fminf(32, (distance + 1) * 16)));
    const int offset = patch->contour->contour[column];
    if (offset == 127) return 0;
    landing += offset;
  }
  const float tolerance = patch->contour ? 3 : 7;
  return fmaxf(0, 1 - distance * distance) * fmaxf(0, 1 - fabsf(y - landing) / tolerance);
}

static void WetSurfaceQuad(const DioramaCapture *capture, const DioramaViewGeometry *g,
    const DioramaGroundCoverCap *cap, const float u[2], const float opacity[2],
    const float across[2], const float lift[2], const ArRenderColorF colors[2], bool spill) {
  if (s_wet_surface.quads == kWetSurfaceMaxQuads || (opacity[0] <= 0 && opacity[1] <= 0)) return;
  const unsigned base = s_wet_surface.quads * 4;
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned x = i == 1 || i == 2, row = i >= 2;
    float point[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
      const DioramaGroundCoverCorner *a = &cap->corners[0], *b = &cap->corners[1];
      const DioramaGroundCoverCorner *c = &cap->corners[3], *d = &cap->corners[2];
      const float front = a->position[axis] + u[x] * (b->position[axis] - a->position[axis]);
      const float back = c->position[axis] + u[x] * (d->position[axis] - c->position[axis]);
      const float up = a->lift[axis] + u[x] * (b->lift[axis] - a->lift[axis]);
      point[axis] = front + across[row] * (back - front) + lift[row] * up;
    }
    /* Offset the thin film in world units. A translucent spill should never
     * fight the native front pixels at the same depth. */
    if (spill) point[2] -= .0025f;
    ArRenderVertex2D *v = &s_wet_surface.vertices[base + i];
    if (!GroundCoverProject(capture, g, point, false, &v->position,
            &s_wet_surface.depths[base + i], &s_wet_surface.clip_w[base + i])) return;
    v->color = colors[row];
    v->color.a *= opacity[x];
  }
  if (DioramaVoxelQuadOutsideViewport(&s_wet_surface.vertices[base], g->width, g->height)) return;
  int32_t *indices = &s_wet_surface.indices[s_wet_surface.quads * 6];
  indices[0] = base; indices[1] = base + 1; indices[2] = base + 2;
  indices[3] = base; indices[4] = base + 2; indices[5] = base + 3;
  ++s_wet_surface.quads;
}

static void WetSurfaceGather(const DioramaCapture *capture, const DioramaViewGeometry *g,
    const DioramaGroundCoverCap *cap, bool pool) {
  const float width = cap->to.x - cap->from.x;
  const float near = cap->corners[0].depth_fraction, far = cap->corners[3].depth_fraction;
  if (width <= 0 || far - near <= .0001f) return;
  for (unsigned i = 0; i < s_wet_surface.patches; ++i) {
    const WetSurfacePatch *patch = &s_wet_surface.patch[i];
    if (patch->pool != pool || near >= patch->depth) continue;
    const float left = fmaxf(cap->from.x, patch->x - patch->half_width);
    const float right = fminf(cap->to.x, patch->x + patch->half_width);
    if (left >= right) continue;
    /* World-aligned samples round the footprint and keep a cap split at a
     * texture boundary or actor band from moving the visible wet line. */
    const float finish = fminf(far, patch->depth);
    for (float x = left, next; x < right; x = next) {
      next = fminf(right, 2 * (floorf(x / 2) + 1));
      const float u[2] = {(x - cap->from.x) / width, (next - cap->from.x) / width};
      const float opacity[2] = {
        WetSurfaceCoverage(patch, x, cap->from.y + u[0] * (cap->to.y - cap->from.y)),
        WetSurfaceCoverage(patch, next, cap->from.y + u[1] * (cap->to.y - cap->from.y)),
      };
      const float shimmer = .87f + .13f * sinf(patch->phase + x * .3f);
      ArRenderColorF body[2] = {patch->body, patch->body};
      body[0].a *= shimmer * (1 - near / patch->depth);
      body[1].a *= shimmer * (1 - finish / patch->depth);
      WetSurfaceQuad(capture, g, cap, u, opacity, (float[2]){0,(finish-near)/(far-near)},
          (float[2]){.22f,.22f}, body, false);
      if (near <= .0001f) {
        ArRenderColorF lip = patch->lip; lip.a *= shimmer;
        WetSurfaceQuad(capture, g, cap, u, opacity, (float[2]){0,0},
            (float[2]){.22f,-patch->spill * shimmer * opacity[0]},
            (ArRenderColorF[2]){lip, {lip.r,lip.g,lip.b,0}}, true);
      }
    }
  }
}

static bool WetSurfaceSubmit(ArRenderDevice *device, const DioramaViewGeometry *g, int plane) {
  const unsigned count = s_wet_surface.quads;
  if (!count) return true;
  s_wet_surface.quads = 0;
  const DioramaDepthSubmission draw = {.vertices = s_wet_surface.vertices,
      .indices = s_wet_surface.indices, .vertex_count = count * 4, .index_count = count * 6,
      .depths = s_wet_surface.depths, .clip_w = s_wet_surface.clip_w,
      .blend = kArRenderBlendMode_Alpha, .plane = plane, .read_only_depth = true};
  return g->depth_sink->submit(g->depth_sink->userdata, device, &draw);
}
