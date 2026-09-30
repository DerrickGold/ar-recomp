/* Bloodpool Act 1: moonlight, low lake mist and red-lake surface reflections.
 * World-anchored, paused-clock geometry only; no uploads, textures or particles
 * with mutable lifetime. Phase: pure. Tests: action_effect_render_test.c. */
#include "action/action_effect_render_internal.h"
#include "action_bloodpool_surface.h"

/* The fixed 768-pixel field admits at most 16 mist cells including edge puffs.
 * Clipping each triangle against a rectangle yields at most seven vertices. */
_Static_assert(16 * 48 * 7 <= kActionSceneEffectRenderMaxVertices &&
               16 * 48 * 15 <= kActionSceneEffectRenderMaxIndices,
               "Bloodpool mist must fit the bounded geometry workspace");

/* Shared by the rear rays and their footprint on foreground water. */
static float MoonLowRayStrength(float slope) {
  static const struct { float slope, width, strength; } rays[] = {
    {-1.40f,.42f,.50f}, {-.72f,.36f,.86f}, {-.22f,.32f,1},
    {.35f,.40f,.86f}, {1.04f,.43f,.66f}, {1.65f,.30f,.36f},
  };
  float light = 0;
  for (unsigned i = 0; i < sizeof(rays)/sizeof(rays[0]); i++)
    light += rays[i].strength*SceneSoftFalloff((slope-rays[i].slope)/rays[i].width);
  return light;
}

static bool WaterReflection(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionEffectLocalRect *clip, float x, float y, float rx, float ry, float opacity,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderColorF clear = {.88f,.52f,.58f,0}, bright = {.88f,.52f,.58f,opacity};
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

static bool MoonClip(const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
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
  clip->x0 = fmaxf(clip->x0,-384);
  clip->x1 = fminf(clip->x1,384);
  clip->y0 = fmaxf(clip->y0,0);
  clip->y1 = fminf(clip->y1,194);
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static bool MoonReflection(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!MoonClip(effect,clip_bounds,userdata,&clip)) return true;
  clip.y0 = fmaxf(clip.y0,82); /* Native distant lake begins below the dark horizon. */
  if (clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float cloud = BloodpoolCloudTransmission(effect->phase_ticks);
  if (!AppendSceneSoftPatch(writer,&mesh,&clip,0,145,66,61,
          (ArRenderColorF){.58f,.30f,.40f,.10f*cloud},0,project_point,userdata)) return false;
  for (unsigned row = 0; row < 23; row++) {
    const uint32_t seed = DeterministicHash_Mix32(row*0x9E3779B9u+0xB101u);
    const float t = ((effect->phase_ticks+seed)&511u)*.012271846f;
    const float spread = 12+row*2.2f;
    for (int part = -1; part <= 1; part++) {
      if (part && ((seed >> (part+2)) & 3u) == 0) continue;
      const float x = part*spread*.68f+4*sinf(t+part);
      const float y = 87+row*4.5f+1.8f*HashUnit(seed^0x48u);
      const float strength = part ? .20f : .38f;
      const float alpha = cloud*strength*(.7f+.3f*cosf(t+part)) *
          fminf(1,(y-82)/20)*fminf(1,(194-y)/18);
      const float width = spread*(.45f+.24f*HashUnit(seed^((unsigned)part+2u)));
      if (!WaterReflection(writer,&mesh,&clip,x,y,width,1.0f,alpha,project_point,userdata))
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

bool AppendBloodpoolWaveCaps(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionBloodpoolDetails *details, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if (!details || !details->water_scroll_valid) return true;
  ActionEffectLocalRect clip;
  if (!MoonClip(effect,clip_bounds,userdata,&clip)) return true;
  clip.y0 = fmaxf(clip.y0,82);
  if (clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float cloud = BloodpoolCloudTransmission(effect->phase_ticks);
  for (unsigned row = 0; row < kDistantWaveRows; row++) {
    const unsigned source_y = 147+row*8;
    const float y = source_y+.5f-effect->world_y;
    if (y+.55f < clip.y0 || y-.55f > clip.y1) continue;
    unsigned scroll_row = source_y-kActionBloodpoolWaterScrollFirstRow;
    if (scroll_row >= kActionBloodpoolWaterScrollRows)
      scroll_row = kActionBloodpoolWaterScrollRows-1;
    if (details->water_scroll[scroll_row] > 1023) return true;
    const uint32_t seed = DeterministicHash_Mix32(row*0x9E3779B9u+0xB1CAu);
    const float width = 3+row*.30f+2*HashUnit(seed^0x31u);
    /* Source CHR repeats every 256 pixels. Translate the crest with its actual
     * raster row; animate only exposure, never a second invented drift speed.
     * Each thin cap stays inside one native row rather than bridging shear. */
    const float origin = (float)(seed&255u)-
        (details->water_scroll[scroll_row]&255u)-effect->world_x;
    const int first = (int)ceilf((clip.x0-width-origin)/256);
    const int last = (int)floorf((clip.x1+width-origin)/256);
    const float phase = ((effect->phase_ticks+(seed>>8))&255u)*.024543693f;
    const float shimmer = .5f+.5f*sinf(phase);
    float flash = fmaxf(0,sinf(phase*2+row));
    flash *= flash;
    flash *= flash;
    flash *= flash;
    for (int copy = first; copy <= last; copy++) {
      const float x = origin+copy*256;
      const float exposure = .10f+.60f*SceneSoftFalloff(x/(44+row*5));
      const float alpha = cloud*exposure*(.28f+.32f*shimmer);
      if (!WaterReflection(writer,&mesh,&clip,x,y,width,.28f,alpha,project_point,userdata))
        return false;
      if (!WaterReflection(writer,&mesh,&clip,x+width*.2f,y,.65f,.50f,
              cloud*exposure*.80f*flash,project_point,userdata)) return false;
    }
  }
  return true;
}

/* Light reaching distant haze has not crossed the foreground platforms.
 * Its visibility is handled by normal BG1 painter order. Only the nearer
 * scattering volume receives platform shadows. Sample that finite volume,
 * rather than treating an earlier screen-space hit as an infinite shadow. */
static float MoonRow(unsigned row) {
  return 12+row*(182.0f/(kActionMoonlightRows-1));
}

static float MoonSlope(unsigned column) {
  return -2+column*(4.0f/(kActionMoonlightColumns-1));
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
  caster.geometry.data.rect = (ActionEffectLocalRect){0,0,4096,512};
  ActionEffectLocalRect clip = caster.geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,&caster,&clip)) return true;
  if (!RectIsSane(&clip)) return false;
  for (unsigned i = 0; i < occlusion->count; i++) {
    const ActionMoonlightOccluder *r = &occlusion->rectangles[i];
    if (r->x0 < 0 || r->y0 < 0 || r->x1 > 4096 || r->y1 > 512 ||
        r->x0 >= r->x1 || r->y0 >= r->y1) return false;
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
static bool PrepareMoonCoverage(const ActionEffectInstance *mesh,
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
    const float radius = 6*sqrtf((source+.5f)/kMoonSources);
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

static bool MoonVisibilityField(const ActionEffectInstance *mesh,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  for (unsigned col = 0; col < kActionMoonlightColumns; col++) {
    for (unsigned row = 0; row < kActionMoonlightRows; row++) {
      const unsigned at = col*kActionMoonlightRows+row;
      const float y = MoonRow(row);
      if (!project_point(userdata,mesh,MoonSlope(col)*y,y,&scratch->points[at])) return false;
    }
  }
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(mesh,occlusion,scratch,kActionMoonlightColumns*kActionMoonlightRows,
          &field,sources,project_point,clip_bounds,userdata)) return false;
  /* Sample the nearer haze at three finite depths. These authored transport
   * ratios approximate where light-to-haze paths intersect the platform plane.
   * A thin ledge shades a bounded region, not everything beneath its top edge.
   * Distant rays never consume this mask: light reaches that haze before BG1.
   * Both contributions use the rear batch so opaque artwork stays untouched. */
  static const float transport[] = {.74f,.85f,.96f};
  const float weight = 1.0f/(kMoonSources*(sizeof(transport)/sizeof(transport[0])));
  for (unsigned i = 0; i < kActionMoonlightColumns*kActionMoonlightRows; i++) {
    const ArRenderPointF point = scratch->points[i];
    float blocked = 0;
    for (unsigned source = 0; source < kMoonSources; source++) {
      const ArRenderPointF light = sources[source];
      const ArRenderPointF delta = {
        point.x-light.x,point.y-light.y};
      for (unsigned slice = 0; slice < sizeof(transport)/sizeof(transport[0]); slice++) {
        const ArRenderPointF crossing = {
          light.x+delta.x*transport[slice],light.y+delta.y*transport[slice]};
        blocked += MoonCoverage(&field,crossing)*weight;
      }
    }
    scratch->visibility[i] = 1-blocked;
  }
  return true;
}

/* Share the exact Act 1 ray shapes with castle exteriors. A NULL visibility
 * field denotes distant sky scattering; foreground artwork supplies occlusion
 * through the normal layer order / winner mask, without a shadow readback. */
static bool AppendMoonRayMesh(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *mesh, const ActionEffectLocalRect *clip,
    const float *visibility, ActionEffectProjectPointFn project_point, void *userdata) {
  /* Two overlapping depth families. Broad, steep rays reach the low lake;
   * shorter, brighter fans catch the haze around the middle walkways. A local
   * near shadow can dim the latter without erasing the distant ray beneath it. */
  static const struct { float slope, width, strength, reach; } middle[] = {
    {-1.67f,.24f,.90f,122}, {-.99f,.23f,1,142}, {0,.22f,.80f,146},
    {.65f,.25f,.92f,138}, {1.36f,.25f,.76f,120},
  };
  const float pulse = (.96f+.04f*sinf((mesh->phase_ticks&2047u)*.0030679616f)) *
      BloodpoolCloudTransmission(mesh->phase_ticks);
  /* The enclosed gallery catches more scattered moonlight than the open
   * sky. Preserve the same source and angular profile behind its pillars. */
  const float gain = mesh->kind == kActionEffect_CastleSky && mesh->environment_room == 7 ? 1.6f : 1;
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
      const float slope = MoonSlope(sample*step);
      const float far_light = .045f+MoonLowRayStrength(slope);
      const float angular_fade = SceneSoftFalloff(slope/2.05f);
      for (unsigned row = 0; row < rows; row++) {
        const float y = MoonRow(row*step);
        const float start = fmaxf(0,fminf(1,(y-12)/22));
        const float far_fade = fminf(1,(194-y)/32);
        float near_light = 0;
        for (unsigned fan = 0; fan < sizeof(middle)/sizeof(middle[0]); fan++) {
          const float remaining = fmaxf(0,fminf(1,(middle[fan].reach-y)/52));
          const float fade = remaining*remaining*(3-2*remaining);
          near_light += middle[fan].strength*fade *
              SceneSoftFalloff((slope-middle[fan].slope)/middle[fan].width);
        }
        const float exposure = visibility ? visibility[(sample*kActionMoonlightRows+row)*step] : 1;
        const float alpha = fminf(.95f,(.24f*far_light*far_fade+.40f*near_light*exposure) *
            start*angular_fade*pulse*gain);
        const unsigned at = row*2+column;
        vertices[at] = (ArRenderVertex2D){{slope*y,y},{.48f,.68f,1,alpha},{0,0}};
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

bool AppendBloodpoolMoonlight(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionMoonlightOcclusion *occlusion, ActionMoonlightRenderScratch *scratch,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!occlusion || !occlusion->valid || occlusion->count > kActionMoonlightMaxOccluders ||
      !MoonClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  if (!MoonVisibilityField(&mesh,occlusion,scratch,project_point,clip_bounds,userdata)) return true;
  return AppendMoonRayMesh(writer,&mesh,&clip,scratch->visibility,project_point,userdata);
}

bool AppendBloodpoolSkyRays(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!MoonClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  return AppendMoonRayMesh(writer,&mesh,&clip,NULL,project_point,userdata);
}

static float Cross(ArRenderPointF a, ArRenderPointF b) {
  return a.x*b.y-a.y*b.x;
}

bool BloodpoolMoonProjection_Init(BloodpoolMoonProjection *projection,
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
  *projection = (BloodpoolMoonProjection){origin,axis,rays[1],Cross(axis,rays[1])};
  return true;
}

float BloodpoolMoonProjection_Light(
    const BloodpoolMoonProjection *projection, ArRenderPointF point) {
  const ArRenderPointF direction = {point.x-projection->origin.x,point.y-projection->origin.y};
  const float down = Cross(projection->axis,direction);
  if (!isfinite(down) || fabsf(down) <= .0001f || down*projection->orientation <= 0) return 0;
  const float slope = Cross(direction,projection->vertical)/down;
  return MoonLowRayStrength(slope)*SceneSoftFalloff(slope/2.05f);
}

bool AppendBloodpoolWaterMoonlight(ActionEffectGeometryWriter *writer,
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
  clip.y0 = fmaxf(clip.y0,8);
  clip.y1 = fminf(clip.y1,31);
  if (!RectIsSane(&clip) || clip.x0 >= clip.x1 || clip.y0 >= clip.y1) return true;
  ActionEffectInstance mesh = *water, source = *moon;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  source.flags |= kActionEffectFlag_ClippedMesh;

  /* Recover the projected pencil of rays from three collinear BG2 samples.
   * Its slope ratio is invariant under perspective, unlike subtracting BG1
   * and BG2 world coordinates or assuming equal screen scales. */
  BloodpoolMoonProjection projection;
  if (!BloodpoolMoonProjection_Init(&projection,&source,project_point,userdata)) return true;

  enum { kWaterRows = 5, kWaterColumns = 194 };
  _Static_assert(kWaterRows*kWaterColumns <= kActionMoonlightColumns*kActionMoonlightRows,
                 "Water lighting must fit the shared moonlight scratch");
  static const float rows[kWaterRows] = {8,12,19,26,31};
  static const float exposure[kWaterRows] = {0,.8f,1,.72f,0};
  const int first = (int)floorf((water->world_x+clip.x0)/4);
  const int last = (int)ceilf((water->world_x+clip.x1)/4);
  const unsigned columns = (unsigned)(last-first+1);
  if (columns > kWaterColumns) return false;
  const float pulse = (.96f+.04f*sinf((moon->phase_ticks&2047u)*.0030679616f)) *
      BloodpoolCloudTransmission(moon->phase_ticks);
  for (unsigned col = 0; col < columns; col++) {
    const float x = (first+(int)col)*4-water->world_x;
    for (unsigned row = 0; row < kWaterRows; row++) {
      const unsigned at = col*kWaterRows+row;
      ArRenderPointF p;
      if (!project_point(userdata,&mesh,x,rows[row],&p)) return true;
      scratch->points[at] = p;
      scratch->visibility[at] =
          .48f*BloodpoolMoonProjection_Light(&projection,p)*exposure[row]*pulse;
    }
  }
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(&source,occlusion,scratch,columns*kWaterRows,&field,sources,
          project_point,clip_bounds,userdata)) return true;
  for (unsigned at = 0; at < columns*kWaterRows; at++) {
    float blocked = 0;
    const ArRenderPointF point = scratch->points[at];
    /* The receiving water is just in front of the platform plane. A single
     * authored intersection depth gives it a surface shadow, unlike the three
     * depths integrated through haze. The finite moon softens the edge. */
    for (unsigned i = 0; i < kMoonSources; i++) {
      const ArRenderPointF crossing = {
        sources[i].x*.12f+point.x*.88f,sources[i].y*.12f+point.y*.88f};
      blocked += MoonCoverage(&field,crossing)/kMoonSources;
    }
    scratch->visibility[at] *= fmaxf(0,1-blocked);
  }
  /* Add light only to validated exposed water, in its existing BG1-high
   * additive batch. Dry banks, upper shoreline art and actors are untouched. */
  for (unsigned pool = 0; pool < kBloodpoolWaterSpanCount; pool++) {
    if (!(water->source_mask & (1u << pool))) continue;
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,kBloodpoolWaterSpans[pool].left+6.0f-water->world_x);
    region.x1 = fminf(region.x1,kBloodpoolWaterSpans[pool].right-6.0f-water->world_x);
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
          vertices[at] = (ArRenderVertex2D){{x+side*4,rows[row]},{.88f,.48f,.56f,alpha},{0,0}};
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

bool AppendBloodpoolTimberMoonlight(ActionEffectGeometryWriter *writer,
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
  if (!BloodpoolMoonProjection_Init(&projection,&source,project_point,userdata)) return true;
  unsigned selected[kActionBloodpoolMaxTimber], count = 0;
  for (unsigned i = 0; i < details->timber_count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[i];
    const float x = (edge->x0+edge->x1)*.5f-effect->world_x;
    const float y = edge->y-effect->world_y;
    if (edge->x0 < 0 || edge->x1 > 4096 || edge->x1 <= edge->x0 || edge->y < 0 || edge->y >= 480)
      return true;
    if (x < clip.x0-8 || x > clip.x1+8 || y < clip.y0-1 || y > clip.y1) continue;
    if (!project_point(userdata,&mesh,x,y,&scratch->points[count])) return true;
    scratch->visibility[count] = BloodpoolMoonProjection_Light(&projection,scratch->points[count]);
    selected[count++] = i;
  }
  if (!count) return true;
  MoonCoverageField field;
  ArRenderPointF sources[kMoonSources];
  if (!PrepareMoonCoverage(&source,occlusion,scratch,count,&field,sources,
          project_point,clip_bounds,userdata)) return true;
  const float cloud = BloodpoolCloudTransmission(effect->phase_ticks);
  for (unsigned i = 0; i < count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[selected[i]];
    float blocked = 0;
    for (unsigned s = 0; s < kMoonSources; s++) {
      const ArRenderPointF crossing = {sources[s].x*.12f+scratch->points[i].x*.88f,
                                      sources[s].y*.12f+scratch->points[i].y*.88f};
      blocked += MoonCoverage(&field,crossing)/kMoonSources;
    }
    const uint32_t seed = DeterministicHash_Mix32((unsigned)edge->x0+(unsigned)edge->y*4096u);
    const float t = ((effect->phase_ticks+seed)&1023u)*.006135923f;
    const float alpha = .28f*cloud*scratch->visibility[i]*fmaxf(0,1-blocked)*(.55f+.45f*cosf(t));
    const float x0 = edge->x0+.5f-effect->world_x, x1 = edge->x1-.5f-effect->world_x;
    const float mid = (x0+x1)*.5f, y = edge->y+.15f-effect->world_y;
    const ArRenderColorF clear = {.72f,.71f,.76f,0}, lit = {.72f,.71f,.76f,alpha};
    const ArRenderVertex2D vertices[] = {
      {{x0,y},clear,{0,0}},{{mid,y},lit,{0,0}},{{x1,y},clear,{0,0}},
      {{x0,y+.8f},clear,{0,0}},{{mid,y+.8f},clear,{0,0}},{{x1,y+.8f},clear,{0,0}},
    };
    int mapped[] = {-1,-1,-1,-1,-1,-1};
    const int triangles[] = {0,1,3,1,4,3,1,2,4,2,5,4};
    for (unsigned t = 0; t < 12; t += 3)
      if (!AppendSceneClippedTriangle(writer,&mesh,vertices,mapped,&triangles[t],&clip,
              project_point,userdata)) return false;
  }
  return true;
}

bool AppendBloodpoolEnvironment(
    ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if (effect->kind == kActionEffect_BloodpoolMoonReflection)
    return MoonReflection(writer,effect,project_point,clip_bounds,userdata);
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
  for (unsigned pool = 0; pool < kBloodpoolWaterSpanCount; pool++) {
    if (!(effect->source_mask & (1u << pool))) continue;
    const float left = kBloodpoolWaterSpans[pool].left + 6.0f;
    const float right = kBloodpoolWaterSpans[pool].right - 6.0f;
    ActionEffectLocalRect region = clip;
    region.x0 = fmaxf(region.x0,left-effect->world_x);
    region.x1 = fminf(region.x1,right-effect->world_x);
    region.y0 = fmaxf(region.y0,mist ? -40 : 8);
    region.y1 = fminf(region.y1,mist ? 6 : 31);
    if (region.x0 >= region.x1 || region.y0 >= region.y1) continue;
    const int spacing = mist ? 64 : 24;
    const int first = (int)floorf((effect->world_x+region.x0-(mist ? 64 : 8))/spacing);
    const int last = (int)floorf((effect->world_x+region.x1+(mist ? 64 : 8))/spacing);
    for (int cell = first; cell <= last; cell++) {
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)cell*0x9E3779B9u);
      const float anchor = cell*spacing + spacing*(.2f+.6f*HashUnit(seed));
      if (anchor < left || anchor >= right) continue;
      const float edge = fminf(1,fminf(anchor-left,right-anchor)/24);
      if (mist) {
        const float t = ((effect->phase_ticks+seed) & 2047u)*.0030679616f;
        const float drift = sinf(t), rx = 38+24*HashUnit(seed ^ 0x52u);
        const float ry = 8+8*HashUnit(seed ^ 0x94u);
        const float x = anchor + 10*drift-effect->world_x;
        const float y = -5-6*HashUnit(seed ^ 0x71u)+2*cosf(t);
        const float opacity = edge*(.24f+.13f*HashUnit(seed ^ 0xA3u))*(.9f+.1f*drift);
        if (!AppendSceneSoftPatch(writer,&mesh,&region,x,y,rx,ry,
                (ArRenderColorF){.43f,.43f,.59f,opacity},6*drift,project_point,userdata))
          return false;
      } else {
        /* Two shallow rows with independent phases, spacing and exposure.
         * No screen-position clamp: whole quads clip at the shoreline/window. */
        for (unsigned row = 0; row < 2; row++) {
          const uint32_t salt = DeterministicHash_Mix32(seed ^ (row+1)*0x85EBCA6Bu);
          if ((salt & 3u) == 0) continue;
          const unsigned mask = (salt & 4u) ? 511u : 255u;
          const float t = ((effect->phase_ticks+salt) & mask)/(float)(mask+1);
          const float pulse = sinf(t*3.14159265f);
          const float x = anchor+3*sinf(t*6.2831853f)-effect->world_x;
          const float y = 12+row*10+3*HashUnit(salt ^ 0x64u);
          const float opacity = edge*(.60f+.30f*HashUnit(salt ^ 0xA2u))*(.3f+.7f*pulse*pulse);
          const float width = 5+8*HashUnit(salt);
          if (!WaterReflection(writer,&mesh,&region,x,y,width*1.2f,1.7f,opacity*.22f,
                  project_point,userdata)) return false;
          if (!WaterReflection(writer,&mesh,&region,x,y,width,.65f,opacity,
                  project_point,userdata)) return false;
        }
      }
    }
  }
  return true;
}
