/* Bloodpool marsh details: immutable scenery anchors and paused-clock motion.
 * Bounded CPU geometry using existing alpha/additive layers; no particle pool,
 * texture uploads or backend-specific rendering. Tests: action_effect_render. */
#include "action/action_effect_render_internal.h"
#include "action_bloodpool_surface.h"

static float Falloff(float x) {
  const float t = fmaxf(0,1-x*x);
  return t*t;
}

static void CloudStyle(uint16_t ticks, float *x, float *y, float *alpha) {
  const float slow = (ticks&16383u)*.0003834952f;
  *x = -24+30*sinf(slow+.7f);
  *y = -2+5*cosf(slow*2+1.2f);
  *alpha = .07f+.07f*(.5f+.5f*sinf(slow+2.3f));
}

float BloodpoolCloudTransmission(uint16_t ticks) {
  float x,y,alpha;
  CloudStyle(ticks,&x,&y,&alpha);
  return 1-alpha*Falloff((-x+y*8/18)/85)*Falloff(y/18);
}

static bool DetailClip(const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
    void *userdata, ActionEffectLocalRect *clip) {
  *clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata,effect,clip)) return false;
  if (effect->flags&kActionEffectFlag_ClipToRect) {
    if (!RectIsSane(&effect->clip_rect)) return false;
    clip->x0 = fmaxf(clip->x0,effect->clip_rect.x0);
    clip->y0 = fmaxf(clip->y0,effect->clip_rect.y0);
    clip->x1 = fminf(clip->x1,effect->clip_rect.x1);
    clip->y1 = fminf(clip->y1,effect->clip_rect.y1);
  }
  clip->x0 = fmaxf(clip->x0,-384);
  clip->x1 = fminf(clip->x1,384);
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

bool AppendBloodpoolCloud(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!DetailClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  float x,y,alpha;
  CloudStyle(effect->phase_ticks,&x,&y,&alpha);
  return AppendSceneSoftPatch(writer,&mesh,&clip,x,y,85,18,
      (ArRenderColorF){.055f,.055f,.09f,alpha},8,project_point,userdata);
}

static bool DetailSpark(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float x, float y, float width, float height,
    ArRenderColorF color, ActionEffectProjectPointFn project_point, void *userdata) {
  const ArRenderVertex2D vertices[] = {
    {{x-width,y},color,{0,0}},{{x,y-height},color,{0,0}},
    {{x+width,y},color,{0,0}},{{x,y+height},color,{0,0}},
  };
  int mapped[] = {-1,-1,-1,-1};
  const int triangles[] = {0,1,3,1,2,3};
  for (unsigned t = 0; t < 6; t += 3)
    if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],clip,
            project_point,userdata)) return false;
  return true;
}

static uint32_t TimberSeed(const ActionBloodpoolTimber *edge) {
  return DeterministicHash_Mix32((unsigned)edge->x0+(unsigned)edge->y*4096u);
}

static float DripAge(const ActionBloodpoolTimber *edge, uint16_t ticks, float *fall_time) {
  const float distance = edge->landing_y-edge->drip_y;
  if (distance < 6 || distance > 480) { *fall_time = 0; return -1; }
  const uint32_t seed = TimberSeed(edge);
  const float gravity = .13f+.04f*HashUnit(seed^0x41u);
  *fall_time = sqrtf(distance/gravity);
  const unsigned mask = (seed&8u) ? 511u : 1023u;
  return (float)((ticks+(seed>>8))&mask);
}

static bool WaterClip(const ActionEffectInstance *effect, const ActionEffectLocalRect *clip,
    float world_x, float water_top, ActionEffectLocalRect *region) {
  for (unsigned pool = 0; pool < kBloodpoolWaterSpanCount; pool++) {
    if (!(effect->source_mask&(1u<<pool))) continue;
    const float left = kBloodpoolWaterSpans[pool].left+6.0f;
    const float right = kBloodpoolWaterSpans[pool].right-6.0f;
    if (world_x < left || world_x >= right) continue;
    *region = *clip;
    region->x0 = fmaxf(region->x0,left-effect->world_x);
    region->x1 = fminf(region->x1,right-effect->world_x);
    region->y0 = fmaxf(region->y0,water_top-effect->world_y);
    region->y1 = fminf(region->y1,511-effect->world_y);
    return region->x0 < region->x1 && region->y0 < region->y1;
  }
  return false;
}

static bool Ripple(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float world_x, float world_y, float water_top, float age,
    float strength, ActionEffectProjectPointFn project_point, void *userdata) {
  if (age < 0 || age >= 80) return true;
  ActionEffectLocalRect region;
  if (!WaterClip(mesh,clip,world_x,water_top,&region)) return true;
  const float t = age/80, radius = 1+14*t;
  const float alpha = strength*sinf(t*3.14159265f)*(1-t);
  const float x = world_x-mesh->world_x, y = world_y-mesh->world_y;
  if (x+radius < region.x0 || x-radius > region.x1 || y+4 < region.y0 || y-4 > region.y1)
    return true;
  /* Fixed unit circle: no per-vertex trigonometry or evolving particle state. */
  static const ArRenderPointF circle[] = {
    {1,0},{.92388f,.38268f},{.70711f,.70711f},{.38268f,.92388f},
    {0,1},{-.38268f,.92388f},{-.70711f,.70711f},{-.92388f,.38268f},
    {-1,0},{-.92388f,-.38268f},{-.70711f,-.70711f},{-.38268f,-.92388f},
    {0,-1},{.38268f,-.92388f},{.70711f,-.70711f},{.92388f,-.38268f},
  };
  ArRenderVertex2D vertices[48];
  int mapped[48];
  for (unsigned i = 0; i < 16; i++) for (unsigned band = 0; band < 3; band++) {
    const float r = radius+((int)band-1)*.65f;
    const unsigned at = i*3+band;
    vertices[at] = (ArRenderVertex2D){
      {x+circle[i].x*r,y+circle[i].y*r*.22f},
      {.85f,.34f,.43f,band == 1 ? alpha : 0},{0,0}};
    mapped[at] = -1;
  }
  for (unsigned i = 0; i < 16; i++) for (unsigned band = 0; band < 2; band++) {
    const int a = (int)i*3+(int)band, b = (int)((i+1)&15)*3+(int)band;
    const int triangles[] = {a,b,a+1,b,b+1,a+1};
    for (unsigned t = 0; t < 6; t += 3)
      if (!AppendSceneClippedTriangle(writer,mesh,vertices,mapped,&triangles[t],&region,
              project_point,userdata)) return false;
  }
  return true;
}

static bool TimberDrops(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionBloodpoolDetails *details, const ActionEffectLocalRect *clip,
    bool water, ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < details->timber_count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[i];
    float fall_time;
    const float age = DripAge(edge,mesh->phase_ticks,&fall_time);
    if (age < 0) continue;
    if (water) {
      if (!edge->water_landing) continue;
      if (!Ripple(writer,mesh,clip,edge->drip_x,edge->landing_y+1,488,age-fall_time,.75f,
              project_point,userdata) ||
          !Ripple(writer,mesh,clip,edge->drip_x,edge->landing_y+1,488,age-fall_time-17,.35f,
              project_point,userdata)) return false;
    } else if (age < fall_time) {
      const float t = age/fall_time;
      const float y = edge->drip_y+(edge->landing_y-edge->drip_y)*t*t-mesh->world_y;
      const float alpha = fminf(1,age/4)*.7f;
      const float height = fminf(1+1.4f*t,edge->landing_y-mesh->world_y-y);
      if (!DetailSpark(writer,mesh,clip,edge->drip_x-mesh->world_x,y,.42f,height,
              (ArRenderColorF){.78f,.65f,.67f,alpha},project_point,userdata)) return false;
    }
  }
  return true;
}

static bool MarshInsects(ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectInstance *moon, const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project_point, void *userdata) {
  BloodpoolMoonProjection light;
  const bool lit = moon && BloodpoolMoonProjection_Init(&light,moon,project_point,userdata);
  for (unsigned pool = 0; pool < kBloodpoolWaterSpanCount; pool++) {
    if (!(mesh->source_mask&(1u<<pool))) continue;
    for (unsigned side = 0; side < 2; side++) {
      const float bank = side ? kBloodpoolWaterSpans[pool].right-24.0f :
                                kBloodpoolWaterSpans[pool].left+24.0f;
      if (bank-mesh->world_x < clip->x0-16 || bank-mesh->world_x > clip->x1+16) continue;
      for (unsigned fly = 0; fly < 4; fly++) {
        const uint32_t seed = DeterministicHash_Mix32(pool*32+side*11+fly*3+0xB100u);
        const float t = ((mesh->phase_ticks+seed)&1023u)*.006135923f;
        const float x = bank+10*sinf(t*2)+4*cosf(t*5)-mesh->world_x;
        const float y = 450+14*HashUnit(seed)+4*sinf(t*3+.8f)-mesh->world_y;
        ArRenderPointF p;
        if (!project_point(userdata,mesh,x,y,&p)) continue;
        const float exposure = lit ? BloodpoolMoonProjection_Light(&light,p) : 0;
        float flash = fmaxf(0,sinf(t+2));
        flash *= flash;
        flash *= flash;
        flash *= flash;
        const float alpha = .22f+.38f*exposure+.22f*flash;
        if (flash > .8f && !DetailSpark(writer,mesh,clip,x,y,1.4f,1.1f,
                (ArRenderColorF){.78f,.70f,.58f,.10f*flash},project_point,userdata)) return false;
        if (!DetailSpark(writer,mesh,clip,x,y,.50f+.15f*flash,.55f+.12f*flash,
                (ArRenderColorF){.78f,.70f,.58f,alpha},project_point,userdata)) return false;
      }
    }
  }
  return true;
}

bool AppendBloodpoolDetailParticles(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectInstance *moon,
    const ActionBloodpoolDetails *details, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  if (!details || !details->valid || details->timber_count > kActionBloodpoolMaxTimber ||
      details->post_count > kActionBloodpoolMaxPosts) return true;
  ActionEffectLocalRect clip;
  if (!DetailClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  if (effect->kind == kActionEffect_BloodpoolAir)
    return TimberDrops(writer,&mesh,details,&clip,false,project_point,userdata) &&
        MarshInsects(writer,&mesh,moon,&clip,project_point,userdata);
  if (effect->kind == kActionEffect_BloodpoolWater) {
    if (!TimberDrops(writer,&mesh,details,&clip,true,project_point,userdata)) return false;
    for (unsigned i = 0; i < details->post_count; i++) {
      const ActionBloodpoolPost *post = &details->posts[i];
      const float x = (post->x0+post->x1)*.5f;
      const uint32_t seed = DeterministicHash_Mix32((unsigned)(post->x0+post->x1)+0xBB21u);
      const float age = (float)((effect->phase_ticks+seed)&255u);
      if (!Ripple(writer,&mesh,&clip,x,post->y,480,age,.6f,project_point,userdata) ||
          !Ripple(writer,&mesh,&clip,x,post->y,480,age-23,.28f,project_point,userdata))
        return false;
    }
  } else if (effect->kind == kActionEffect_BloodpoolMist) {
    for (unsigned i = 0; i < details->timber_count; i++) {
      const ActionBloodpoolTimber *edge = &details->timber[i];
      const uint32_t seed = TimberSeed(edge);
      if (!edge->water_landing || edge->y < 352 || (seed&3u)) continue;
      const float t = ((effect->phase_ticks+seed)&2047u)*.0030679616f;
      const float x = edge->drip_x-effect->world_x+4*sinf(t);
      ActionEffectLocalRect region = clip;
      /* Gather at water level beneath low walkways, with the same shoreline
       * clipping as the base mist. Never invent a floating elevated floor. */
      bool found = false;
      for (unsigned pool = 0; pool < kBloodpoolWaterSpanCount; pool++) {
        const float left = kBloodpoolWaterSpans[pool].left+6.0f;
        const float right = kBloodpoolWaterSpans[pool].right-6.0f;
        if (!(effect->source_mask&(1u<<pool)) ||
            edge->drip_x < left || edge->drip_x >= right) continue;
        region.x0 = fmaxf(region.x0,left-effect->world_x);
        region.x1 = fminf(region.x1,right-effect->world_x);
        found = true;
        break;
      }
      if (!found || region.x0 >= region.x1) continue;
      if (!AppendSceneSoftPatch(writer,&mesh,&region,x,472-effect->world_y,
              23+6*sinf(t),8,(ArRenderColorF){.40f,.37f,.50f,.22f},4*cosf(t),
              project_point,userdata)) return false;
    }
  }
  return true;
}
