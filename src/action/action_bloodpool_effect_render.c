/* Bloodpool Act 1: moonlight, low lake mist and red-lake surface reflections.
 * World-anchored, paused-clock geometry only; no uploads, textures or particles
 * with mutable lifetime. Phase: pure. Tests: action_effect_render_test.c. */
#include "action/action_effect_render_internal.h"

/* The fixed 768-pixel field admits at most 16 mist cells including edge puffs.
 * Clipping each triangle against a rectangle yields at most seven vertices. */
_Static_assert(16 * 48 * 7 <= kActionSceneEffectRenderMaxVertices &&
               16 * 48 * 15 <= kActionSceneEffectRenderMaxIndices,
               "Bloodpool mist must fit the bounded geometry workspace");

/* Shared by the rear rays and their footprint on foreground water. */
static float MoonLowRayStrength(const ActionMoonField *f, float slope) {
  const float (*rays)[3] = f->low;
  float light = 0;
  for (unsigned i = 0; i < 6; i++)
    light += rays[i][2]*SceneSoftFalloff((slope-rays[i][0])/rays[i][1]);
  return light;
}

static bool WaterReflection(const float *color, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry, float opacity,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderColorF clear = {color[0],color[1],color[2],0}, bright = {clear.r,clear.g,clear.b,opacity};
  const ArRenderVertex2D vertices[] = {
    {{x-rx,y},clear,{0,0}}, {{x,y-ry},bright,{0,0}},
    {{x+rx,y},clear,{0,0}}, {{x,y+ry},bright,{0,0}},
  };
  int mapped[] = {-1,-1,-1,-1};
  const int triangles[] = {0,1,3,1,2,3};
  for (int t = 0; t < 6; t += 3)
    if (!AppendSceneClippedTriangle(writer,effect,vertices,mapped,&triangles[t],
            clip,project_point,userdata)) return false;
  return true;
}

static bool MoonClip(const ActionMoonField *f, const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
    void *userdata, ActionEffectLocalRect *clip) {
  *clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,effect,clip)) return false;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&effect->clip_rect)) return false;
    clip->x0 = fmaxf(clip->x0,effect->clip_rect.x0);
    clip->y0 = fmaxf(clip->y0,effect->clip_rect.y0);
    clip->x1 = fminf(clip->x1,effect->clip_rect.x1);
    clip->y1 = fminf(clip->y1,effect->clip_rect.y1);
  }
  clip->x0 = fmaxf(clip->x0,f->Window[0]);
  clip->x1 = fminf(clip->x1,f->Window[2]);
  clip->y0 = fmaxf(clip->y0,f->Window[1]);
  clip->y1 = fminf(clip->y1,f->Window[3]);
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static bool MoonReflection(const ActionMoonField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if(!((unsigned)f->Components[0]&2))return true;
  ActionEffectLocalRect clip;
  if (!MoonClip(f, effect,clip_bounds,userdata,&clip)) return true;
  clip.y0 = fmaxf(clip.y0,f->ReflectionRows[1]); /* Native distant lake begins below the dark horizon. */
  if (clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float cloud = BloodpoolCloudTransmission(f, effect->phase_ticks);
  if (!AppendSceneSoftPatch(writer,&mesh,&clip,f->ReflectionGlow[0],f->ReflectionGlow[1],f->ReflectionGlow[2],f->ReflectionGlow[3],
          (ArRenderColorF){f->ReflectionGlowColor[0],f->ReflectionGlowColor[1],f->ReflectionGlowColor[2],f->ReflectionGlowColor[3]*cloud},0,project_point,userdata)) return false;
  for (unsigned row = 0; row < (unsigned)f->ReflectionRows[0]; row++) {
    const uint32_t seed = DeterministicHash_Mix32(row*0x9E3779B9u+(unsigned)f->Seeds[0]);
    const float t = ((effect->phase_ticks+seed)&((unsigned)f->ReflectionMotion[0]-1))*f->ReflectionMotion[1];
    const float spread = f->ReflectionMotion[2]+row*f->ReflectionMotion[3];
    for (int part = -1; part <= 1; part++) {
      if (part && ((seed >> (part+2)) & 3u) == 0) continue;
      const float x = part*spread*f->ReflectionMotion[4]+f->ReflectionMotion[5]*sinf(t+part);
      const float y = f->ReflectionRows[2]+row*f->ReflectionRows[3]+f->ReflectionRows[4]*HashUnit(seed^0x48u);
      const float strength = part ? f->ReflectionGain[0] : f->ReflectionGain[1];
      const float alpha = cloud*strength*(f->ReflectionGain[2]+f->ReflectionGain[3]*cosf(t+part)) *
          fminf(1,(y-f->ReflectionRows[1])/f->ReflectionGain[4])*fminf(1,(f->Window[3]-y)/f->ReflectionGain[5]);
      const float width = spread*(f->ReflectionShape[0]+f->ReflectionShape[1]*HashUnit(seed^((unsigned)part+2u)));
      if (!WaterReflection(f->ReflectionColor, writer,&mesh,&clip,x,y,width,f->ReflectionShape[2],alpha,project_point,userdata))
        return false;
    }
  }
  return true;
}

enum { kDistantWaveRows = 12, kDistantWaveCopies = 4 };
_Static_assert(kDistantWaveRows*kDistantWaveCopies*4*7 + 7400 <=
                   kActionSceneEffectRenderMaxVertices &&
               kDistantWaveRows*kDistantWaveCopies*4*15 + 19000 <=
                   kActionSceneEffectRenderMaxIndices,
               "distant wave caps must leave room for moon rays and reflections");

bool AppendBloodpoolWaveCaps(const ActionMoonField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionBloodpoolDetails *details, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if(!((unsigned)f->Components[0]&8))return true;
  if (!details || !details->water_scroll_valid) return true;
  ActionEffectLocalRect clip;
  if (!MoonClip(f, effect,clip_bounds,userdata,&clip)) return true;
  clip.y0 = fmaxf(clip.y0,f->ReflectionRows[1]);
  if (clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float cloud = BloodpoolCloudTransmission(f, effect->phase_ticks);
  for (unsigned row = 0; row < (unsigned)f->WaveRows[0]; row++) {
    const unsigned source_y = (unsigned)f->WaveRows[1]+row*(unsigned)f->WaveRows[2];
    const float y = source_y+.5f-effect->world_y;
    if (y+.55f < clip.y0 || y-.55f > clip.y1) continue;
    unsigned scroll_row = source_y-kActionBloodpoolWaterScrollFirstRow;
    if (scroll_row >= kActionBloodpoolWaterScrollRows)
      scroll_row = kActionBloodpoolWaterScrollRows-1;
    if (details->water_scroll[scroll_row] > 1023) return true;
    const uint32_t seed = DeterministicHash_Mix32(row*0x9E3779B9u+(unsigned)f->Seeds[1]);
    const float width = f->WaveShape[0]+row*f->WaveShape[1]+f->WaveShape[2]*HashUnit(seed^0x31u);
    /* Source CHR repeats every 256 pixels. Translate the crest with its actual
     * raster row; animate only exposure, never a second invented drift speed.
     * Each thin cap stays inside one native row rather than bridging shear. */
    const float origin = (float)(seed&255u)-
        (details->water_scroll[scroll_row]&255u)-effect->world_x;
    const int first = (int)ceilf((clip.x0-width-origin)/256);
    const int last = (int)floorf((clip.x1+width-origin)/256);
    const float phase = ((effect->phase_ticks+(seed>>8))&((unsigned)f->WaveMotion[0]-1))*f->WaveMotion[1];
    const float shimmer = .5f+.5f*sinf(phase);
    float flash = fmaxf(0,sinf(phase*f->WaveMotion[2]+row));
    flash *= flash;
    flash *= flash;
    flash *= flash;
    for (int copy = first; copy <= last; copy++) {
      const float x = origin+copy*256;
      const float exposure = f->WaveGain[0]+f->WaveGain[1]*SceneSoftFalloff(x/(f->WaveGain[2]+row*f->WaveGain[3]));
      const float alpha = cloud*exposure*(f->WaveGain[4]+f->WaveGain[5]*shimmer);
      if (!WaterReflection(f->ReflectionColor, writer,&mesh,&clip,x,y,width,f->WaveShape[3],alpha,project_point,userdata))
        return false;
      if (!WaterReflection(f->ReflectionColor, writer,&mesh,&clip,x+width*f->WaveShape[4],y,f->WaveShape[5],f->WaveShape[6],
              cloud*exposure*f->WaveGain[6]*flash,project_point,userdata)) return false;
    }
  }
  return true;
}

/* Light reaching distant haze has not crossed the foreground platforms.
 * Its visibility is handled by normal BG1 painter order. Only the nearer
 * scattering volume receives platform shadows. Sample that finite volume,
 * rather than treating an earlier screen-space hit as an infinite shadow. */
static float MoonRow(const ActionMoonField *f, unsigned row) {
  return f->rows[row][0];
}

static float MoonSlope(const ActionMoonField *f, unsigned column) {
  return f->columns[column][0];
}

typedef struct MoonCoverageField {
  ArRenderPointF origin;
  float texel_size;
  uint8_t *pixels;
} MoonCoverageField;

static void RasterMoonCaster(MoonCoverageField *field, ArRenderPointF corners[4]) {
  float top = kActionMoonlightMaskHeight, bottom = 0;
  for (unsigned i = 0; i < 4; i++) {
    corners[i].x = (corners[i].x-field->origin.x)/field->texel_size;
    corners[i].y = (corners[i].y-field->origin.y)/field->texel_size;
    top = fminf(top,corners[i].y);
    bottom = fmaxf(bottom,corners[i].y);
  }
  const int first = (int)floorf(fmaxf(0,top));
  const int last = (int)ceilf(fminf(kActionMoonlightMaskHeight,bottom));
  for (int y = first; y < last; y++) {
    /* Two vertical coverage samples and analytic horizontal coverage preserve
     * narrow holes and soften fractional projection movement. Rectangles form
     * a nonoverlapping partition of the native opaque pixels. */
    for (unsigned sample = 0; sample < 2; sample++) {
      const float scan = y+.25f+.5f*sample;
      float left = kActionMoonlightMaskWidth, right = 0;
      unsigned crossings = 0;
      for (unsigned edge = 0; edge < 4; edge++) {
        const ArRenderPointF a = corners[edge], b = corners[(edge+1)&3];
        if ((a.y <= scan && b.y > scan) || (b.y <= scan && a.y > scan)) {
          const float x = a.x+(b.x-a.x)*(scan-a.y)/(b.y-a.y);
          left = fminf(left,x);
          right = fmaxf(right,x);
          crossings++;
        }
      }
      if (crossings != 2 || left >= right) continue;
      const int begin = (int)floorf(fmaxf(0,left));
      const int end = (int)ceilf(fminf(kActionMoonlightMaskWidth,right));
      for (int x = begin; x < end; x++) {
        const float coverage = fminf(x+1,right)-fmaxf(x,left);
        const unsigned at = (unsigned)y*kActionMoonlightMaskWidth+(unsigned)x;
        const unsigned value = field->pixels[at]+(unsigned)(coverage*127.5f+.5f);
        field->pixels[at] = (uint8_t)(value < 255 ? value : 255);
      }
    }
  }
}

static bool ProjectMoonOccluders(const ActionMoonlightOcclusion *occlusion,
    const ActionEffectInstance *moon, MoonCoverageField *field,
    ArRenderPointF origin, float scale, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectInstance caster = *moon;
  caster.world_x = caster.world_y = 0;
  caster.projection_plane = kActionEffectProjectionPlane_Bg1;
  caster.flags = kActionEffectFlag_Visible | kActionEffectFlag_ClippedMesh;
  caster.geometry.data.rect = (ActionEffectLocalRect){-2048, -2048, 16384, 16384};
  ActionEffectLocalRect clip = caster.geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,&caster,&clip)) return true;
  if (!RectIsSane(&clip)) return false;
  for (unsigned i = 0; i < occlusion->count; i++) {
    const ActionMoonlightOccluder *r = &occlusion->rectangles[i];
    if (r->x0 < -2048 || r->y0 < -2048 || r->x1 > 16384 || r->y1 > 16384 || r->x0 >= r->x1 ||
        r->y0 >= r->y1)
      return false;
    const float x0 = fmaxf(r->x0,clip.x0), x1 = fminf(r->x1,clip.x1);
    const float y0 = fmaxf(r->y0,clip.y0), y1 = fminf(r->y1,clip.y1);
    if (x0 >= x1 || y0 >= y1) continue;
    const ArRenderPointF source[] = {{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    ArRenderPointF corners[4];
    for (unsigned j = 0; j < 4; j++) {
      ArRenderPointF p;
      if (!project_point(userdata,&caster,source[j].x,source[j].y,&p)) return false;
      corners[j] = (ArRenderPointF){(p.x-origin.x)/scale,(p.y-origin.y)/scale};
      if (!isfinite(corners[j].x) || !isfinite(corners[j].y) ||
          fabsf(corners[j].x) > 1000000 || fabsf(corners[j].y) > 1000000) return false;
    }
    RasterMoonCaster(field,corners);
  }
  return true;
}

static float MoonCoverage(const MoonCoverageField *field, ArRenderPointF point) {
  const float x = point.x-.5f, y = point.y-.5f;
  if (x < 0 || y < 0 || x >= kActionMoonlightMaskWidth-1 ||
      y >= kActionMoonlightMaskHeight-1) return 0;
  const int ix = (int)x, iy = (int)y;
  const float fx = x-ix, fy = y-iy;
  const uint8_t *p = &field->pixels[iy*kActionMoonlightMaskWidth+ix];
  const float a = p[0]+(p[1]-p[0])*fx;
  const float b = p[kActionMoonlightMaskWidth]+(p[kActionMoonlightMaskWidth+1]-
      p[kActionMoonlightMaskWidth])*fx;
  return (a+(b-a)*fy)/255;
}

enum { kMoonSources = 9 };

/* Input points are projected pixels; output points/sources use mask texels.
 * Both haze and water build this bounded workspace independently, so callback
 * order or a generated presentation frame cannot leave a stale shadow field. */
static bool PrepareMoonCoverage(const ActionMoonField *f, const ActionEffectInstance *mesh,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    unsigned point_count, MoonCoverageField *field, ArRenderPointF sources[kMoonSources],
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ArRenderPointF origin, below;
  if (!project_point(userdata,mesh,0,0,&origin) ||
      !project_point(userdata,mesh,0,20,&below)) return false;
  const float scale = hypotf(below.x-origin.x,below.y-origin.y)/20;
  if (!isfinite(scale) || scale < .0001f) return false;
  ActionEffectLocalRect bounds = {0,0,0,0};
  for (unsigned source = 0; source < kMoonSources; source++) {
    const float radius = f->Shadow[0]*sqrtf((source+.5f)/kMoonSources);
    const float angle = source*2.39996323f;
    ArRenderPointF p;
    if (!project_point(userdata,mesh,radius*cosf(angle),radius*sinf(angle),&p)) return false;
    sources[source] = (ArRenderPointF){(p.x-origin.x)/scale,(p.y-origin.y)/scale};
    if (!isfinite(sources[source].x) || !isfinite(sources[source].y)) return false;
    bounds.x0 = fminf(bounds.x0,sources[source].x);
    bounds.y0 = fminf(bounds.y0,sources[source].y);
    bounds.x1 = fmaxf(bounds.x1,sources[source].x);
    bounds.y1 = fmaxf(bounds.y1,sources[source].y);
  }
  for (unsigned at = 0; at < point_count; at++) {
    const ArRenderPointF p = scratch->points[at];
    const ArRenderPointF point = {(p.x-origin.x)/scale,(p.y-origin.y)/scale};
    if (!isfinite(point.x) || !isfinite(point.y)) return false;
    scratch->points[at] = point;
    bounds.x0 = fminf(bounds.x0,point.x);
    bounds.y0 = fminf(bounds.y0,point.y);
    bounds.x1 = fmaxf(bounds.x1,point.x);
    bounds.y1 = fmaxf(bounds.y1,point.y);
  }
  *field = (MoonCoverageField){
    .origin = {floorf(bounds.x0)-8,floorf(bounds.y0)-8},
    .pixels = scratch->coverage,
  };
  /* Resolution stays bounded at any window size or Diorama camera angle.
   * Every transport sample lies between one source and one mesh point. */
  field->texel_size = fmaxf(1,fmaxf(
      (bounds.x1-field->origin.x+8)/kActionMoonlightMaskWidth,
      (bounds.y1-field->origin.y+8)/kActionMoonlightMaskHeight));
  memset(field->pixels,0,sizeof(scratch->coverage));
  if (!ProjectMoonOccluders(occlusion,mesh,field,origin,scale,
          project_point,clip_bounds,userdata)) return false;
  /* Transform once; the many transport taps below need only affine blends. */
  for (unsigned source = 0; source < kMoonSources; source++) {
    sources[source].x = (sources[source].x-field->origin.x)/field->texel_size;
    sources[source].y = (sources[source].y-field->origin.y)/field->texel_size;
  }
  for (unsigned i = 0; i < point_count; i++) {
    scratch->points[i].x = (scratch->points[i].x-field->origin.x)/field->texel_size;
    scratch->points[i].y = (scratch->points[i].y-field->origin.y)/field->texel_size;
  }
  return true;
}

static bool MoonVisibilityField(const ActionMoonField *f, const ActionEffectInstance *mesh,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionMoonVisibilityCache *cache = &scratch->cache;
  const bool cacheable = project_point == ActionEffectProjection_ProjectPoint &&
      clip_bounds == ActionEffectProjection_ClipBounds && userdata;
  ActionEffectProjectionContext context = {0};
  DioramaProjection projection;
  if (cacheable) {
    context = *(const ActionEffectProjectionContext *)userdata;
    const DioramaProjection *p = context.diorama_projection;
    if (p && p->bg2_skybox.count && !p->bg2_skybox.world_plane.valid &&
        (mesh->flags & kActionEffectFlag_StaticAnchor)) {
      const int y = (int16_t)(uint16_t)(mesh->world_y-context.bg2_camera_y);
      const DioramaSkyboxBandProjection *source =
          Diorama_SkyboxAnchorBand(p, y+context.ws_extra_top);
      if (!source) return false;
      projection = *p;
      /* Visibility uses the complete ray field, independent of the output
       * strip clipping applied later. A static moon uses its source band's
       * transform even while the water's other UV bands animate. Keep that
       * exact band and BG1's caster projection; unrelated planes cannot shade
       * this field. This also permits reuse between skybox strip callbacks. */
      projection.bg2_skybox = (DioramaSkyboxProjection){
          .count=1, .active_band=0, .bands={*source}};
      projection.bg2_plane = (DioramaPlaneProjection){0};
      projection.bg1_high_plane = projection.bg2_high_plane = (DioramaPlaneProjection){0};
      memset(projection.object_planes, 0, sizeof(projection.object_planes));
      context.diorama_projection = &projection;
    }
  }
  /* Only projection inputs affect visibility. The light's clock/color still
   * animate in AppendMoonRayMesh on every presentation. */
  const ActionEffectInstance anchor = {.world_x = mesh->world_x, .world_y = mesh->world_y,
      .flags = mesh->flags, .projection_plane = mesh->projection_plane,
      .obj_priority = mesh->obj_priority, .render_layer = mesh->render_layer,
      .geometry = mesh->geometry, .clip_rect = mesh->clip_rect};
  if (cacheable && cache->ready && cache->occlusion.count == occlusion->count &&
      !memcmp(cache->occlusion.rectangles, occlusion->rectangles,
              occlusion->count * sizeof(occlusion->rectangles[0])) &&
      !memcmp(&cache->anchor, &anchor, sizeof(anchor)) &&
      !memcmp(&cache->field, f, sizeof(*f)) &&
      ActionEffectProjection_Matches(&cache->projection, &context)) {
    memcpy(scratch->visibility, cache->visibility, sizeof(cache->visibility));
    return true;
  }
  cache->ready = false;
  for (unsigned col = 0; col < kActionMoonlightColumns; col++) {
    for (unsigned row = 0; row < kActionMoonlightRows; row++) {
      const unsigned at = col*kActionMoonlightRows+row;
      const float y = MoonRow(f, row);
      if (!project_point(userdata,mesh,MoonSlope(f, col)*y,y,&scratch->points[at])) return false;
    }
  }
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(f, mesh,occlusion,scratch,kActionMoonlightColumns*kActionMoonlightRows,
          &field,sources,project_point,clip_bounds,userdata)) return false;
  /* Sample the nearer haze at three finite depths. These authored transport
   * ratios approximate where light-to-haze paths intersect the platform plane.
   * A thin ledge shades a bounded region, not everything beneath its top edge.
   * Distant rays never consume this mask: light reaches that haze before BG1.
   * Both contributions use the rear batch so opaque artwork stays untouched. */
  const float *transport = f->Shadow+1;
  const float weight = 1.0f/(kMoonSources*3);
  for (unsigned i = 0; i < kActionMoonlightColumns*kActionMoonlightRows; i++) {
    const ArRenderPointF point = scratch->points[i];
    float blocked = 0;
    for (unsigned source = 0; source < kMoonSources; source++) {
      const ArRenderPointF light = sources[source];
      const ArRenderPointF delta = {
        point.x-light.x,point.y-light.y};
      for (unsigned slice = 0; slice < 3; slice++) {
        const ArRenderPointF crossing = {
          light.x+delta.x*transport[slice],light.y+delta.y*transport[slice]};
        blocked += MoonCoverage(&field,crossing)*weight;
      }
    }
    scratch->visibility[i] = 1-blocked;
  }
  if (cacheable) {
    cache->occlusion.count = occlusion->count;
    memcpy(cache->occlusion.rectangles, occlusion->rectangles,
            occlusion->count * sizeof(occlusion->rectangles[0]));
    cache->anchor = anchor;
    cache->field = *f;
    memcpy(cache->visibility, scratch->visibility, sizeof(cache->visibility));
    ActionEffectProjection_Remember(&cache->projection, &context);
    cache->ready = true;
  }
  return true;
}

/* Share the exact Act 1 ray shapes with castle exteriors. A NULL visibility
 * field denotes distant sky scattering; foreground artwork supplies occlusion
 * through the normal layer order / winner mask, without a shadow readback. */
static bool AppendMoonRayMesh(const ActionMoonField *f, ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *mesh, const ActionEffectLocalRect *clip,
    const float *visibility, float gain, ActionEffectProjectPointFn project_point, void *userdata) {
  /* Two overlapping depth families. Broad, steep rays reach the low lake;
   * shorter, brighter fans catch the haze around the middle walkways. A local
   * near shadow can dim the latter without erasing the distant ray beneath it. */
  _Static_assert((int)kActionMoonFieldColumns==(int)kActionMoonlightColumns &&
      (int)kActionMoonFieldRows==(int)kActionMoonlightRows,"prepared mesh matches fixed renderer budget");
  const float pulse = (f->Pulse[2]+f->Pulse[3]*sinf((mesh->phase_ticks&((unsigned)f->Pulse[0]-1))*f->Pulse[1])) *
      BloodpoolCloudTransmission(f, mesh->phase_ticks);
  /* Unshadowed exterior haze needs fewer samples than Act 1's fine platform
   * penumbras. Both densities sample the same fixed angular/radial profile. */
  const unsigned step = visibility ? 1 : 2;
  const unsigned rows = (kActionMoonlightRows-1)/step+1;
  const unsigned columns = (kActionMoonlightColumns-1)/step+1;
  int previous_column[kActionMoonlightRows];
  for (unsigned row = 0; row < rows; row++) previous_column[row] = -1;
  for (unsigned strip = 0; strip+1 < columns; strip++) {
    ArRenderVertex2D vertices[2*kActionMoonlightRows];
    int mapped[2*kActionMoonlightRows];
    for (unsigned column = 0; column < 2; column++) {
      const unsigned sample = strip+column;
      const float *column_profile=f->columns[sample*step];
      const float slope = column_profile[0];
      const float far_light = column_profile[1];
      const float angular_fade = column_profile[2];
      for (unsigned row = 0; row < rows; row++) {
        const float *row_profile=f->rows[row*step];
        const float y=row_profile[0],start=row_profile[1],far_fade=row_profile[2];
        float near_light = 0;
        for (unsigned fan = 0; fan < 5; fan++)
          near_light += row_profile[3+fan]*column_profile[3+fan];
        const float exposure = visibility ? visibility[(sample*kActionMoonlightRows+row)*step] : 1;
        const float alpha = fminf(f->RayGain[0],(f->RayGain[1]*far_light*far_fade+f->RayGain[2]*near_light*exposure) *
            start*angular_fade*pulse*gain);
        const unsigned at = row*2+column;
        vertices[at] = (ArRenderVertex2D){{slope*y,y},{f->RayColor[0],f->RayColor[1],f->RayColor[2],alpha},{0,0}};
        mapped[at] = column ? -1 : previous_column[row];
      }
    }
    for (unsigned row = 0; row+1 < rows; row++) {
      const int a = (int)row*2;
      const int triangles[] = {a,a+1,a+2,a+1,a+3,a+2};
      for (unsigned t = 0; t < 6; t += 3)
        if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],clip,
                project_point,userdata)) return false;
    }
    for (unsigned row = 0; row < rows; row++)
      previous_column[row] = mapped[row*2+1];
  }
  return true;
}

bool AppendBloodpoolMoonlight(const ActionMoonField *f,
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if(!((unsigned)f->Components[0]&1))return true;
  ActionEffectLocalRect clip;
  if (!occlusion || !occlusion->valid || occlusion->count > kActionMoonlightMaxOccluders ||
      !MoonClip(f, effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  if (!MoonVisibilityField(f, &mesh,occlusion,scratch,project_point,clip_bounds,userdata)) return true;
  return AppendMoonRayMesh(f, writer,&mesh,&clip,scratch->visibility,1,project_point,userdata);
}

bool AppendBloodpoolSkyRays(const ActionMoonField *f,float gain,ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!MoonClip(f, effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  return AppendMoonRayMesh(f, writer,&mesh,&clip,NULL,gain,project_point,userdata);
}

static float Cross(ArRenderPointF a, ArRenderPointF b) {
  return a.x*b.y-a.y*b.x;
}

bool BloodpoolMoonProjection_Init(const ActionMoonField *f, BloodpoolMoonProjection *projection,
    const ActionEffectInstance *moon, ActionEffectProjectPointFn project_point, void *userdata) {
  ActionEffectInstance source = *moon;
  source.flags |= kActionEffectFlag_ClippedMesh;
  ArRenderPointF origin, rays[3];
  if (!project_point(userdata,&source,0,0,&origin)) return false;
  for (unsigned i = 0; i < 3; i++) {
    ArRenderPointF p;
    if (!project_point(userdata,&source,((int)i-1)*256,128,&p)) return false;
    rays[i] = (ArRenderPointF){p.x-origin.x,p.y-origin.y};
  }
  const float determinant = Cross(rays[2],rays[0]);
  if (!isfinite(determinant) || fabsf(determinant) < .0001f) return false;
  const float ratio = 2*Cross(rays[1],rays[0])/determinant;
  const ArRenderPointF axis = {
    (ratio*rays[2].x-rays[1].x)*.5f,(ratio*rays[2].y-rays[1].y)*.5f};
  *projection = (BloodpoolMoonProjection){origin,axis,rays[1],Cross(axis,rays[1]),f};
  return true;
}

float BloodpoolMoonProjection_Light(
    const BloodpoolMoonProjection *projection, ArRenderPointF point) {
  const ActionMoonField *f=projection->field;
  const ArRenderPointF direction = {point.x-projection->origin.x,point.y-projection->origin.y};
  const float down = Cross(projection->axis,direction);
  if (!isfinite(down) || fabsf(down) <= .0001f || down*projection->orientation <= 0) return 0;
  const float slope = Cross(direction,projection->vertical)/down;
  return MoonLowRayStrength(f, slope)*SceneSoftFalloff(slope/f->RayShape[1]);
}

bool AppendBloodpoolWaterMoonlight(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *water, const ActionEffectInstance *moon,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if (!moon || !occlusion || !occlusion->valid ||
      occlusion->count > kActionMoonlightMaxOccluders || !water->source_mask) return true;
  ActionEffectLocalRect clip = water->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,water,&clip)) return true;
  if (water->flags & kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&water->clip_rect)) return true;
    clip.x0 = fmaxf(clip.x0,water->clip_rect.x0);
    clip.y0 = fmaxf(clip.y0,water->clip_rect.y0);
    clip.x1 = fminf(clip.x1,water->clip_rect.x1);
    clip.y1 = fminf(clip.y1,water->clip_rect.y1);
  }
  clip.x0 = fmaxf(clip.x0,-384);
  clip.x1 = fminf(clip.x1,384);
  clip.y0 = fmaxf(clip.y0,m->WaterRows[0]);
  clip.y1 = fminf(clip.y1,m->WaterRows[4]);
  if (!RectIsSane(&clip) || clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *water, source = *moon;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  source.flags |= kActionEffectFlag_ClippedMesh;

  /* Recover the projected pencil of rays from three collinear BG2 samples.
   * Its slope ratio is invariant under perspective, unlike subtracting BG1
   * and BG2 world coordinates or assuming equal screen scales. */
  BloodpoolMoonProjection projection;
  if (!BloodpoolMoonProjection_Init(f, &projection,&source,project_point,userdata)) return true;

  enum { kWaterRows = 5, kWaterColumns = 194 };
  _Static_assert(kWaterRows*kWaterColumns <= kActionMoonlightColumns*kActionMoonlightRows,
                 "Water lighting must fit the shared moonlight scratch");
  const float *rows=m->WaterRows;
  const float *exposure=m->WaterExposure;
  const int first = (int)floorf((water->world_x+clip.x0)/4);
  const int last = (int)ceilf((water->world_x+clip.x1)/4);
  const unsigned columns = (unsigned)(last-first+1);
  if (columns > kWaterColumns) return false;
  const float pulse = (f->Pulse[2]+f->Pulse[3]*sinf((moon->phase_ticks&((unsigned)f->Pulse[0]-1))*f->Pulse[1])) *
      BloodpoolCloudTransmission(f, moon->phase_ticks);
  for (unsigned col = 0; col < columns; col++) {
    const float x = (first+(int)col)*4-water->world_x;
    for (unsigned row = 0; row < kWaterRows; row++) {
      const unsigned at = col*kWaterRows+row;
      ArRenderPointF p;
      if (!project_point(userdata,&mesh,x,rows[row],&p)) return true;
      scratch->points[at] = p;
      scratch->visibility[at] =
          m->WaterLight[0]*BloodpoolMoonProjection_Light(&projection,p)*exposure[row]*pulse;
    }
  }
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(f, &source,occlusion,scratch,columns*kWaterRows,&field,sources,
          project_point,clip_bounds,userdata)) return true;
  for (unsigned at = 0; at < columns*kWaterRows; at++) {
    float blocked = 0;
    const ArRenderPointF point = scratch->points[at];
    /* The receiving water is just in front of the platform plane. A single
     * authored intersection depth gives it a surface shadow, unlike the three
     * depths integrated through haze. The finite moon softens the edge. */
    for (unsigned i = 0; i < kMoonSources; i++) {
      const ArRenderPointF crossing = {
        sources[i].x*f->Shadow[4]+point.x*f->Shadow[5],sources[i].y*f->Shadow[4]+point.y*f->Shadow[5]};
      blocked += MoonCoverage(&field,crossing)/kMoonSources;
    }
    scratch->visibility[at] *= fmaxf(0,1-blocked);
  }
  /* Add light only to validated exposed water, in its existing BG1-high
   * additive batch. Dry banks, upper shoreline art and actors are untouched. */
  for (unsigned pool = 0; pool < (unsigned)m->SpanCount[0]; pool++) {
    if (!(water->source_mask & (1u << pool))) continue;
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,m->spans[pool][0]+m->Surface[3]-water->world_x);
    region.x1 = fminf(region.x1,m->spans[pool][1]-m->Surface[3]-water->world_x);
    if (region.x0 >= region.x1) continue;
    int previous[kWaterRows];
    for (unsigned row = 0; row < kWaterRows; row++) previous[row] = -1;
    for (unsigned col = 0; col+1 < columns; col++) {
      const float x = (first+(int)col)*4-water->world_x;
      if (x+4 <= region.x0 || x >= region.x1) continue;
      ArRenderVertex2D vertices[2*kWaterRows];
      int mapped[2*kWaterRows];
      for (unsigned side = 0; side < 2; side++) {
        for (unsigned row = 0; row < kWaterRows; row++) {
          const unsigned at = row*2+side;
          const float alpha = scratch->visibility[(col+side)*kWaterRows+row];
          vertices[at] = (ArRenderVertex2D){{x+side*4,rows[row]},{m->WaterLight[1],m->WaterLight[2],m->WaterLight[3],alpha},{0,0}};
          mapped[at] = side ? -1 : previous[row];
        }
      }
      for (unsigned row = 0; row+1 < kWaterRows; row++) {
        const int a = (int)row*2, triangles[] = {a,a+1,a+2,a+1,a+3,a+2};
        for (unsigned t = 0; t < 6; t += 3)
          if (!AppendSceneClippedTriangle(writer,&mesh,vertices,mapped,&triangles[t],&region,
                  project_point,userdata)) return false;
      }
      for (unsigned row = 0; row < kWaterRows; row++) previous[row] = mapped[row*2+1];
    }
  }
  return true;
}

bool AppendBloodpoolTimberMoonlight(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectInstance *moon,
    const ActionBloodpoolDetails *details, const ActionMoonlightOcclusion *occlusion,
    ActionMoonlightRenderScratch *scratch, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if (!moon || !details || !details->valid || !details->timber_count ||
      details->timber_count > kActionBloodpoolMaxTimber || !occlusion || !occlusion->valid ||
      occlusion->count > kActionMoonlightMaxOccluders) return true;
  ActionEffectLocalRect clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,effect,&clip)) return true;
  ActionEffectInstance mesh = *effect, source = *moon;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  source.flags |= kActionEffectFlag_ClippedMesh;
  BloodpoolMoonProjection projection;
  if (!BloodpoolMoonProjection_Init(f, &projection,&source,project_point,userdata)) return true;
  unsigned selected[kActionBloodpoolMaxTimber], count = 0;
  for (unsigned i = 0; i < details->timber_count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[i];
    const float x = (edge->x0+edge->x1)*.5f-effect->world_x;
    const float y = edge->y-effect->world_y;
    if (edge->x0 < m->Bounds[0] || edge->x1 > m->Bounds[2] || edge->x1 <= edge->x0 || edge->y < m->Bounds[1] || edge->y >= m->Surface[0])
      return true;
    if (x < clip.x0-8 || x > clip.x1+8 || y < clip.y0-1 || y > clip.y1) continue;
    if (!project_point(userdata,&mesh,x,y,&scratch->points[count])) return true;
    scratch->visibility[count] = BloodpoolMoonProjection_Light(&projection,scratch->points[count]);
    selected[count++] = i;
  }
  if (!count) return true;
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(f, &source,occlusion,scratch,count,&field,sources,
          project_point,clip_bounds,userdata)) return true;
  const float cloud = BloodpoolCloudTransmission(f, effect->phase_ticks);
  for (unsigned i = 0; i < count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[selected[i]];
    float blocked = 0;
    for (unsigned s = 0; s < kMoonSources; s++) {
      const ArRenderPointF crossing = {sources[s].x*f->Shadow[4]+scratch->points[i].x*f->Shadow[5],
                                      sources[s].y*f->Shadow[4]+scratch->points[i].y*f->Shadow[5]};
      blocked += MoonCoverage(&field,crossing)/kMoonSources;
    }
    const uint32_t seed = DeterministicHash_Mix32((unsigned)edge->x0+(unsigned)edge->y*(unsigned)m->Seeds[0]);
    const float t = ((effect->phase_ticks+seed)&((unsigned)m->TimberMotion[0]-1))*m->TimberMotion[1];
    const float alpha = m->TimberGain[0]*cloud*scratch->visibility[i]*fmaxf(0,1-blocked)*(m->TimberGain[1]+m->TimberGain[2]*cosf(t));
    const float x0 = edge->x0+m->TimberShape[0]-effect->world_x, x1 = edge->x1-m->TimberShape[0]-effect->world_x;
    const float mid = (x0+x1)*.5f, y = edge->y+m->TimberShape[1]-effect->world_y;
    const ArRenderColorF clear = {m->TimberColor[0],m->TimberColor[1],m->TimberColor[2],0}, lit = {m->TimberColor[0],m->TimberColor[1],m->TimberColor[2],alpha};
    const ArRenderVertex2D vertices[] = {
      {{x0,y},clear,{0,0}},{{mid,y},lit,{0,0}},{{x1,y},clear,{0,0}},
      {{x0,y+m->TimberShape[2]},clear,{0,0}},{{mid,y+m->TimberShape[2]},clear,{0,0}},{{x1,y+m->TimberShape[2]},clear,{0,0}},
    };
    int mapped[] = {-1,-1,-1,-1,-1,-1};
    const int triangles[] = {0,1,3,1,4,3,1,2,4,2,5,4};
    for (unsigned t = 0; t < 12; t += 3)
      if (!AppendSceneClippedTriangle(writer,&mesh,vertices,mapped,&triangles[t],&clip,
              project_point,userdata)) return false;
  }
  return true;
}

bool AppendBloodpoolEnvironment(const ActionMoonField *f, const ActionMarshField *m,
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if (effect->kind == kActionEffect_BloodpoolMoonReflection)
    return MoonReflection(f, writer,effect,project_point,clip_bounds,userdata);
  ActionEffectLocalRect clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,effect,&clip)) return true;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&effect->clip_rect)) return true;
    clip.x0 = fmaxf(clip.x0,effect->clip_rect.x0);
    clip.y0 = fmaxf(clip.y0,effect->clip_rect.y0);
    clip.x1 = fminf(clip.x1,effect->clip_rect.x1);
    clip.y1 = fminf(clip.y1,effect->clip_rect.y1);
  }
  /* Keep work bounded even if a caller supplies a larger projection window. */
  clip.x0 = fmaxf(clip.x0,-384);
  clip.x1 = fminf(clip.x1,384);
  clip.y0 = fmaxf(clip.y0,-48);
  clip.y1 = fminf(clip.y1,32);
  if (!RectIsSane(&clip) || clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const bool mist = effect->kind == kActionEffect_BloodpoolMist;
  for (unsigned pool = 0; pool < (unsigned)m->SpanCount[0]; pool++) {
    if (!(effect->source_mask & (1u << pool))) continue;
    const float left = m->spans[pool][0] + m->Surface[3];
    const float right = m->spans[pool][1] - m->Surface[3];
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,left-effect->world_x);
    region.x1 = fminf(region.x1,right-effect->world_x);
    region.y0 = fmaxf(region.y0,mist ? m->MistWindow[0] : m->WaterRows[0]);
    region.y1 = fminf(region.y1,mist ? m->MistWindow[1] : m->WaterRows[4]);
    if (region.x0 >= region.x1 || region.y0 >= region.y1) continue;
    const float *cells=mist?m->MistCells:m->WaterCells;
    const int spacing=(int)cells[0];
    const int first = (int)floorf((effect->world_x+region.x0-cells[1])/spacing);
    const int last = (int)floorf((effect->world_x+region.x1+cells[1])/spacing);
    for (int cell = first; cell <= last; cell++) {
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)cell*0x9E3779B9u);
      const float anchor = cell*spacing + spacing*(cells[3]+cells[4]*HashUnit(seed));
      if (anchor < left || anchor >= right) continue;
      const float edge = fminf(1,fminf(anchor-left,right-anchor)/cells[2]);
      if (mist) {
        const float t = ((effect->phase_ticks+seed) & ((unsigned)m->MistMotion[0]-1))*m->MistMotion[1];
        const float drift = sinf(t), rx = m->MistShape[0]+m->MistShape[1]*HashUnit(seed ^ (unsigned)m->Seeds[4]);
        const float ry = m->MistShape[2]+m->MistShape[3]*HashUnit(seed ^ (unsigned)m->Seeds[5]);
        const float x = anchor + m->MistMotion[2]*drift-effect->world_x;
        const float y = m->MistMotion[3]-m->MistMotion[4]*HashUnit(seed ^ (unsigned)m->Seeds[6])+m->MistMotion[5]*cosf(t);
        const float opacity = edge*(m->MistGain[0]+m->MistGain[1]*HashUnit(seed ^ (unsigned)m->Seeds[7]))*(m->MistGain[2]+m->MistGain[3]*drift);
        if (!AppendSceneSoftPatch(writer,&mesh,&region,x,y,rx,ry,
                (ArRenderColorF){m->MistColor[0],m->MistColor[1],m->MistColor[2],opacity},m->MistMotion[6]*drift,project_point,userdata))
          return false;
      } else {
        /* Two shallow rows with independent phases, spacing and exposure.
         * No screen-position clamp: whole quads clip at the shoreline/window. */
        for (unsigned row = 0; row < 2; row++) {
          const uint32_t salt = DeterministicHash_Mix32(seed ^ (row+1)*0x85EBCA6Bu);
          if ((salt & 3u) == 0) continue;
          const unsigned mask = (unsigned)((salt & 4u) ? m->WaterMotion[0] : m->WaterMotion[1])-1;
          const float t = ((effect->phase_ticks+salt) & mask)/(float)(mask+1);
          const float pulse = sinf(t*3.14159265f);
          const float x = anchor+m->WaterMotion[2]*sinf(t*m->WaterMotion[3])-effect->world_x;
          const float y = m->WaterMotion[4]+row*m->WaterMotion[5]+m->WaterMotion[6]*HashUnit(salt ^ (unsigned)m->Seeds[8]);
          const float opacity = edge*(m->WaterGain[0]+m->WaterGain[1]*HashUnit(salt ^ (unsigned)m->Seeds[9]))*(m->WaterGain[2]+m->WaterGain[3]*pulse*pulse);
          const float width = m->WaterShape[0]+m->WaterShape[1]*HashUnit(salt);
          if (!WaterReflection(m->WaterColor, writer,&mesh,&region,x,y,width*m->WaterShape[2],m->WaterShape[3],opacity*m->WaterShape[4],
                  project_point,userdata)) return false;
          if (!WaterReflection(m->WaterColor, writer,&mesh,&region,x,y,width,m->WaterShape[5],opacity,
                  project_point,userdata)) return false;
        }
      }
    }
  }
  return true;
}
