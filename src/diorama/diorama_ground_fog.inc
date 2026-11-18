/* Floor mist is gathered from the same physical caps as terrain. Three
 * feathered sections of each puff give it height and depth without a
 * screen-facing billboard or a full-screen volume pass. */
#include "deterministic_hash.h"
enum {
  kGroundFogMaxSpans = kActionSceneDecorationMaxInstances + kActionMarshFieldMaxSpans +
      kActionBloodpoolMaxTimber +
      kActionAuthoredMaxInstances * kActionAuthoredFloorMaxSpans,
  kGroundFogMaxSections = 1536,
  kGroundFogRingPoints = 8,
  kGroundFogVertices = kGroundFogRingPoints + 1,
  kGroundFogIndices = kGroundFogRingPoints * 3,
  kGroundFogSpacing = 24,
};
static const float kGroundFogSupportTolerance = 2.f;

typedef struct GroundFogSpan {
  ActionEffectFloorSpan floor;
  ArRenderColorF color;
  float phase, spacing, radius_x;
  uint8_t claim;
} GroundFogSpan;

typedef struct GroundFogSection {
  ArRenderVertex2D vertices[kGroundFogVertices];
  float depths[kGroundFogVertices], clip_w[kGroundFogVertices];
} GroundFogSection;

static struct {
  unsigned span_count, section_count;
  uint8_t claims;
  const ActionVoxelGeometry *geometry;
  float min_x, max_x, min_y, max_y;
  GroundFogSpan spans[kGroundFogMaxSpans];
  GroundFogSection sections[kGroundFogMaxSections];
  GroundCoverFaceOrder order[kGroundFogMaxSections];
  ArRenderVertex2D vertices[kGroundFogMaxSections * kGroundFogVertices];
  float depths[kGroundFogMaxSections * kGroundFogVertices];
  float clip_w[kGroundFogMaxSections * kGroundFogVertices];
  int32_t indices[kGroundFogMaxSections * kGroundFogIndices];
} s_ground_fog;

static void GroundFogAddSpan(GroundFogSpan span) {
  const ActionEffectFloorSpan floor = span.floor;
  if (floor.x1 <= floor.x0 || floor.height <= 0 || span.color.a <= 0) return;
  if (s_ground_fog.span_count >= kGroundFogMaxSpans) return;
  s_ground_fog.spans[s_ground_fog.span_count++] = span;
  s_ground_fog.claims |= span.claim;
  s_ground_fog.min_x = fminf(s_ground_fog.min_x, floor.x0);
  s_ground_fog.max_x = fmaxf(s_ground_fog.max_x, floor.x1);
  s_ground_fog.min_y = fminf(s_ground_fog.min_y, floor.y - kGroundFogSupportTolerance);
  s_ground_fog.max_y = fmaxf(s_ground_fog.max_y, floor.y + kGroundFogSupportTolerance);
}

static void GroundFogAddMarshTimber(const ActionBloodpoolDetails *marsh,
    const ActionEffectInstance *effect, float left, float right, float fade) {
  if (!marsh->valid || marsh->timber_count > kActionBloodpoolMaxTimber) return;
  const ActionMarshField *field = &marsh->field;
  const float *style = field->UnderMist;
  const float *tint = field->UnderMistColor;
  for (unsigned i=0; i<marsh->timber_count; ++i) {
    const ActionBloodpoolTimber *edge = &marsh->timber[i];
    if (!edge->water_landing || edge->y < style[0] || edge->drip_x < left || edge->drip_x >= right) continue;
    const uint32_t seed = DeterministicHash_Mix32((unsigned)edge->x0+(unsigned)edge->y*(unsigned)field->Seeds[0]);
    if (seed&3u) continue;
    const float phase = ((effect->phase_ticks+seed)&((unsigned)style[2]-1))*style[3];
    const float x = edge->drip_x+style[4]*sinf(phase);
    const float radius = style[5]+style[6]*sinf(phase);
    GroundFogAddSpan((GroundFogSpan){
      .floor={fmaxf(left,x-radius),fminf(right,x+radius),field->Surface[0],2*style[7]+4},
      .color={tint[0],tint[1],tint[2],tint[3]*fade},.phase=phase,
      .spacing=radius*2,.radius_x=radius,.claim=kDioramaFog_Marsh,
    });
  }
}

static void GroundFogAddMarshSpans(const DioramaScene *scene, const ActionBloodpoolDetails *marsh,
    const ActionEffectInstance *effect, float fade) {
  if (!marsh->field_valid ||
      ActionVoxel_Profile(scene->voxel_geometry) != kActionVoxelProfile_BloodpoolMarsh) return;
  const ActionMarshField *field = &marsh->field;
  unsigned surface_count;
  const VoxelWaterSurface *surfaces = ActionVoxel_WaterSurfaces(scene->voxel_geometry, &surface_count);
  const float height = fminf(-field->MistWindow[0], 2*field->MistShape[2]+field->MistShape[3]);
  const float spacing = field->MistCells[0]*.75f;
  const float radius = field->MistShape[0] + field->MistShape[1]*.5f;
  const float phase = (effect->phase_ticks & ((unsigned)field->MistMotion[0]-1))*field->MistMotion[1];
  const ArRenderColorF color = {field->MistColor[0],field->MistColor[1],field->MistColor[2],
      (field->MistGain[0]+field->MistGain[1]*.5f)*field->MistGain[2]*fade};
  const unsigned count = (unsigned)field->SpanCount[0];
  if (count > kActionMarshFieldMaxSpans || spacing <= 0 || radius <= 0) return;
  for (unsigned i=0; i<count; ++i) {
    if (!(effect->source_mask & (1u<<i))) continue;
    const float left = field->spans[i][0]+field->Surface[3], right = field->spans[i][1]-field->Surface[3];
    /* Claim the native effect only when the current room has a receiving
     * water cap. Checked native spans still bound the mist at the shore. */
    for (unsigned j=0; j<surface_count; ++j) {
      const VoxelWaterSurface *water = &surfaces[j];
      if (water->background || water->opaque || water->y != (int)field->Surface[0] ||
          water->x >= right || water->x+water->width <= left) continue;
      GroundFogAddSpan((GroundFogSpan){
        .floor={left,right,field->Surface[0],height}, .color=color, .phase=phase,
        .spacing=spacing,.radius_x=radius,.claim=kDioramaFog_Marsh,
      });
      GroundFogAddMarshTimber(marsh,effect,left,right,fade);
      break;
    }
  }
}

static void GroundFogPrepare(const DioramaScene *scene, const DioramaViewGeometry *g) {
  s_ground_fog.span_count = s_ground_fog.section_count = s_ground_fog.claims = 0;
  s_ground_fog.geometry = scene->voxel_geometry;
  s_ground_fog.min_x = s_ground_fog.min_y = INFINITY;
  s_ground_fog.max_x = s_ground_fog.max_y = -INFINITY;
  const ActionSceneEffectFrame *frame = scene->trunk_effects;
  const float fade = 1.f - scene->native_bloom_dimming;
  /* Attached atmosphere follows Environmental Effects, like the planar
   * BG1 mist pass. The gameplay particle switch only controls loose grains. */
  if (!frame || !g->depth_sink || fade <= 0 ||
      scene->render->voxel_inspection == kDioramaVoxelInspect_Backing ||
      !ActionVoxel_Active(scene->voxel_geometry, scene->map_group, scene->map_number))
    return;
  if (!frame->decoration_overflow && frame->decoration_count <= kActionSceneDecorationMaxInstances)
    for (unsigned i = 0; i < frame->decoration_count; ++i) {
      const ActionEffectInstance *effect = &frame->decorations[i];
      if (!(effect->flags & kActionEffectFlag_Visible) ||
          effect->geometry.kind != kActionEffectGeometry_Rect) continue;
      if (effect->kind == kActionEffect_BloodpoolMist) {
        GroundFogAddMarshSpans(scene, &frame->bloodpool, effect, fade);
        continue;
      }
      ArRenderColorF color;
      float height_scale = 1.f;
      if (effect->kind == kActionEffect_CastleMist && frame->castle_field_valid) {
        const float *rgb = frame->castle_field.MistColor;
        /* The translucent castle tint needs some scattered light to read
         * against blue stone, especially between the foreground merlons. */
        const float scatter = .22f;
        color = (ArRenderColorF){rgb[0] + scatter*(1-rgb[0]),
            rgb[1] + scatter*(1-rgb[1]), rgb[2] + scatter*(1-rgb[2]), rgb[3]*1.4f};
        /* A low skirt is completely hidden by the battlements' near lip
         * in the straight-on view. Give the mist room to rise above it. */
        height_scale = 1.35f;
      } else if (effect->kind == kActionEffect_TempleGroundMist && frame->atmosphere_field_valid) {
        const ActionAtmosphereField *field = &frame->atmosphere_field;
        const float *rgb = field->MistColor;
        /* Match the native mist model: gain lights the color and density
         * controls opacity. The main billow sets the average bank density. */
        const float billow = field->MistBillow[0];
        const float light = field->MistLighting[0] + field->MistLighting[2]*billow;
        color = (ArRenderColorF){fminf(1, rgb[0] + field->MistGain[0]*light),
            fminf(1, rgb[1] + field->MistGain[1]*light),
            fminf(1, rgb[2] + field->MistGain[2]*light),
            field->MistDensity[0] + field->MistDensity[1]*billow};
      } else continue;
      color.a *= fade;
      const ActionEffectLocalRect *rect = &effect->geometry.data.rect;
      GroundFogAddSpan((GroundFogSpan){.floor={effect->world_x + rect->x0,
          effect->world_x + rect->x1, effect->world_y, (rect->y1 - rect->y0)*height_scale},
          .color=color,.phase=(effect->phase_ticks % 1024) * (6.2831853f / 1024),
          .spacing=kGroundFogSpacing,.radius_x=kGroundFogSpacing*1.3f,.claim=kDioramaFog_Floor});
    }
  if (frame->authored_count > kActionAuthoredMaxInstances) return;
  for (unsigned i = 0; i < frame->authored_count; ++i) {
    const ActionEffectInstance *effect = &frame->authored[i];
    const ActionEffectFloorField *field = &frame->authored_floor[i];
    if (effect->kind != kActionEffect_AuthoredFloorMist ||
        !(effect->flags & kActionEffectFlag_Visible) || !effect->particle_lifetime ||
        field->count > kActionAuthoredFloorMaxSpans)
      continue;
    const uint32_t rgb = effect->tuning.color;
    const ArRenderColorF color = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f,
        (rgb & 255) / 255.f, .2f * effect->tuning.intensity * fade};
    const float phase = (effect->pulse_ticks % effect->particle_lifetime) *
        (6.2831853f / effect->particle_lifetime);
    for (unsigned j = 0; j < field->count; ++j)
      GroundFogAddSpan((GroundFogSpan){.floor=field->spans[j],.color=color,.phase=phase,
          .spacing=kGroundFogSpacing,.radius_x=kGroundFogSpacing*1.3f,.claim=kDioramaFog_Floor});
  }
}

static bool GroundFogMayTouch(float x0, float x1, float y0, float y1) {
  if (!(s_ground_fog.claims & kDioramaFog_Floor) || fmaxf(x0,x1)<=s_ground_fog.min_x ||
      fminf(x0,x1)>=s_ground_fog.max_x) return false;
  if(fmaxf(y0,y1)>=s_ground_fog.min_y && fminf(y0,y1)<=s_ground_fog.max_y)return true;
  /* Keep a floor underneath a native colliding spike row in the candidate
   * set. Gather still checks the exact connected bed at each sample. */
  unsigned count;const VoxelPatch *patches=ActionVoxel_Patches(s_ground_fog.geometry,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelPatch *p=&patches[i];
    if(p->kind==kVoxelPatch_Spikes && p->x<fmaxf(x0,x1) && p->x+p->width>fminf(x0,x1) &&
        p->y+p->height>=fminf(y0,y1)-kGroundFogSupportTolerance &&
        p->y+p->height<=fmaxf(y0,y1)+kGroundFogSupportTolerance)return true;
  }
  return false;
}

static void GroundFogAddSection(const DioramaCapture *capture, const DioramaViewGeometry *g,
    const float center[3], const float a[3], const float b[3], ArRenderColorF color) {
  if (s_ground_fog.section_count == kGroundFogMaxSections) return;
  static const float ring[kGroundFogRingPoints][2] = {
    {1,0}, {.7071f,.7071f}, {0,1}, {-.7071f,.7071f},
    {-1,0}, {-.7071f,-.7071f}, {0,-1}, {.7071f,-.7071f},
  };
  GroundFogSection *section = &s_ground_fog.sections[s_ground_fog.section_count];
  float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
  for (unsigned i = 0; i < kGroundFogVertices; ++i) {
    const float *offset = i ? ring[i - 1] : (const float[2]){0};
    float point[3];
    for (unsigned axis = 0; axis < 3; ++axis)
      point[axis] = center[axis] + a[axis]*offset[0] + b[axis]*offset[1];
    ArRenderVertex2D *v = &section->vertices[i];
    if (!GroundCoverProject(capture, g, point, false, &v->position,
                            &section->depths[i], &section->clip_w[i])) return;
    v->color = color;
    if (i) v->color.a = 0;
    min_x = fminf(min_x, v->position.x); max_x = fmaxf(max_x, v->position.x);
    min_y = fminf(min_y, v->position.y); max_y = fmaxf(max_y, v->position.y);
  }
  if (max_x < 0 || min_x > g->width || max_y < 0 || min_y > g->height) return;
  const unsigned index = s_ground_fog.section_count++;
  s_ground_fog.order[index] = (GroundCoverFaceOrder){section->depths[0], index};
}

/* A spike bed is open air above its supporting floor, not a raised solid
 * receiver. Trace only contiguous compiled spike profiles touching that floor;
 * a ceiling strip or nearby platform must not lift the mist. */
static float GroundFogSpikeHeight(float x, float floor_y) {
  unsigned count;
  const VoxelPatch *patches=ActionVoxel_Patches(s_ground_fog.geometry,&count);
  float top=floor_y;
  for(unsigned joined=0;joined<count;++joined) {
    float next=top;
    for(unsigned i=0;i<count;++i) {
      const VoxelPatch *p=&patches[i];
      if(p->kind==kVoxelPatch_Spikes && x>=p->x && x<p->x+p->width &&
          fabsf(p->y+p->height-top)<.001f)
        next=fminf(next,p->y);
    }
    if(next==top)return floor_y-top;
    top=next;
  }
  return floor_y-top;
}

static void GroundFogGather(const DioramaCapture *capture, const DioramaViewGeometry *g,
    const DioramaGroundCoverCap *cap, unsigned claim, float left_edge, float right_edge) {
  const float width = cap->to.x - cap->from.x;
  if (width <= .0001f) return;
  const float depth0 = cap->corners[0].depth_fraction, depth1 = cap->corners[3].depth_fraction;
  if (depth1 - depth0 <= .0001f) return;
  for (unsigned i = 0; i < s_ground_fog.span_count; ++i) {
    const GroundFogSpan *span = &s_ground_fog.spans[i];
    if (span->claim != claim) continue;
    const float left = fmaxf(fmaxf(cap->from.x, left_edge), span->floor.x0);
    const float right = fminf(fminf(cap->to.x, right_edge), span->floor.x1);
    if (left >= right) continue;
    /* Stable samples, independent of capture crops, depth passes and camera. */
    const float spacing = fminf(span->spacing, span->floor.x1 - span->floor.x0);
    const float first = span->floor.x0 + spacing * .5f;
    for (float x = first + ceilf((left-first)/spacing)*spacing; x < right; x += spacing) {
      const float u = (x - cap->from.x) / width;
      const float y = cap->from.y + u*(cap->to.y-cap->from.y);
      const float spikes=claim==kDioramaFog_Floor?GroundFogSpikeHeight(x,y):0;
      const float support_gap=y-span->floor.y;
      /* Some native variants mark the lower spike row as colliding. The
       * captured mist then names that row, while the physical receiver is
       * the floor underneath. Accept only the connected hazard interval. */
      if (fabsf(support_gap) > kGroundFogSupportTolerance &&
          !(spikes>0 && support_gap>0 && support_gap<=spikes)) continue;
      const float ripple = sinf(x*.067f - span->phase);
      const float rx = fminf(span->radius_x, fminf(x-fmaxf(left_edge, span->floor.x0),
          fminf(right_edge, span->floor.x1)-x));
      for (unsigned lane = 0; lane < 2; ++lane) {
        const float f = lane ? .63f : .27f;
        if (f < depth0 || f >= depth1) continue;
        const float v = (f - depth0) / (depth1 - depth0);
        float center[3], across[3], into[3], up[3];
        const float height = (span->floor.height+spikes) * (.42f + .05f*ripple);
        for (unsigned axis = 0; axis < 3; ++axis) {
          const DioramaGroundCoverCorner *c = cap->corners;
          const float front = c[0].position[axis] + u*(c[1].position[axis]-c[0].position[axis]);
          const float back = c[3].position[axis] + u*(c[2].position[axis]-c[3].position[axis]);
          across[axis] = rx*((1-v)*(c[1].position[axis]-c[0].position[axis]) +
              v*(c[2].position[axis]-c[3].position[axis])) / width;
          into[axis] = (back-front)*fminf(f, 1-f)/(depth1-depth0);
          up[axis] = c[0].lift[axis]*height;
          center[axis] = front + v*(back-front) + up[axis]*1.02f;
        }
        ArRenderColorF color = span->color;
        color.a = fminf(.5f, color.a * (.92f + .14f*ripple));
        GroundFogAddSection(capture, g, center, across, up, color);
        GroundFogAddSection(capture, g, center, into, up, color);
        GroundFogAddSection(capture, g, center, across, into, color);
      }
    }
  }
}

static void GroundFogGatherWater(const DioramaCapture *capture, const DioramaViewGeometry *g,
    const VoxelWaterSurface *surface, float x0, float x1, float world_y, float depth) {
  if (!(s_ground_fog.claims & kDioramaFog_Marsh) || surface->background || surface->opaque) return;
  DioramaGroundCoverCap cap = {.from={x0,surface->y}, .to={x1,surface->y}};
  for (unsigned j=0; j<4; ++j) {
    const bool end = j==1 || j==2, back = j>=2;
    cap.corners[j] = (DioramaGroundCoverCorner){
      .position={end?x1:x0,world_y,back?depth:0},
      .lift={0,-1,0},.depth_fraction=back?1:0,
    };
  }
  GroundFogGather(capture, g, &cap, kDioramaFog_Marsh, surface->x, surface->x+surface->width);
}

static bool GroundFogSubmit(ArRenderDevice *device, const DioramaViewGeometry *g, int plane,
    ArRenderTexture lightmap) {
  const unsigned count = s_ground_fog.section_count;
  if (!count) return true;
  s_ground_fog.section_count = 0;
  qsort(s_ground_fog.order, count, sizeof(*s_ground_fog.order), GroundCoverFaceCompare);
  for (unsigned i = 0; i < count; ++i) {
    const GroundFogSection *section = &s_ground_fog.sections[s_ground_fog.order[i].index];
    const unsigned base = i*kGroundFogVertices;
    memcpy(s_ground_fog.vertices+base, section->vertices, sizeof(section->vertices));
    memcpy(s_ground_fog.depths+base, section->depths, sizeof(section->depths));
    memcpy(s_ground_fog.clip_w+base, section->clip_w, sizeof(section->clip_w));
    for (unsigned j = 0; j < kGroundFogRingPoints; ++j) {
      int32_t *tri = s_ground_fog.indices+i*kGroundFogIndices+j*3;
      tri[0] = base;
      tri[1] = base + 1 + j;
      tri[2] = base + 1 + (j + 1) % kGroundFogRingPoints;
    }
  }
  /* White supplies the material shader's art sample. Vertex alpha still
   * controls fog density, while the shared light map tints only fog pixels. */
  const ArRenderTexture art = ArRenderTexture_IsValid(lightmap)
      ? DioramaSolidPixel(device, &s_diorama_white_pixel, 0xffffffffu)
      : ArRenderTexture_Invalid();
  const DioramaSceneryLightEffectParams lighting = {
    .illumination=lightmap, .texel_width=1, .texel_height=1,
    .shade={.32f,.32f,.32f,1},
  };
  DioramaDepthShadingSubmit submit = {.device=device, .sink=g->depth_sink,
      .draw={.texture=art, .blend=kArRenderBlendMode_Alpha,
          .lighting=ArRenderTexture_IsValid(art)?&lighting:NULL,
          .plane=plane, .read_only_depth=true, .clip_w=s_ground_fog.clip_w},
      .performance=DioramaPerformance_Begin(kDioramaPerformance_Mesh)};
  const bool ok = SubmitShadedDioramaDepth(&submit, s_ground_fog.vertices, s_ground_fog.depths,
      count*kGroundFogVertices, s_ground_fog.indices, count*kGroundFogIndices);
  DioramaPerformance_End(submit.performance);
  return ok;
}
