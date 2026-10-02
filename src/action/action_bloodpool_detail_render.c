/* Bloodpool marsh details: immutable scenery anchors and paused-clock motion.
 * Bounded CPU geometry using existing alpha/additive layers; no particle pool,
 * texture uploads or backend-specific rendering. Tests: action_effect_render. */
#include "action/action_effect_render_internal.h"

static float Falloff(float x) {
  const float t = fmaxf(0,1-x*x);
  return t*t;
}

static void CloudStyle(const ActionMoonField *f, uint16_t ticks, float *x, float *y, float *alpha) {
  const float slow = (ticks&((unsigned)f->CloudMotion[0]-1))*f->CloudMotion[1];
  *x = f->CloudMotion[2]+f->CloudMotion[3]*sinf(slow+f->CloudMotion[4]);
  *y = f->CloudMotion[5]+f->CloudMotion[6]*cosf(slow*f->CloudMotion[7]+f->CloudMotion[8]);
  *alpha = f->CloudOpacity[0]+f->CloudOpacity[1]*(.5f+.5f*sinf(slow+f->CloudMotion[9]));
}

float BloodpoolCloudTransmission(const ActionMoonField *f, uint16_t ticks) {
  if(!((unsigned)f->Components[0]&4))return 1;
  float x,y,alpha;
  CloudStyle(f, ticks,&x,&y,&alpha);
  return 1-alpha*Falloff((-x+y*f->CloudShape[2]/f->CloudShape[1])/f->CloudShape[0])*Falloff(y/f->CloudShape[1]);
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

bool AppendBloodpoolCloud(const ActionMoonField *f, ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  if(!((unsigned)f->Components[0]&4))return true;
  ActionEffectLocalRect clip;
  if (!DetailClip(effect,clip_bounds,userdata,&clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  float x,y,alpha;
  CloudStyle(f, effect->phase_ticks,&x,&y,&alpha);
  return AppendSceneSoftPatch(writer,&mesh,&clip,x,y,f->CloudShape[0],f->CloudShape[1],
      (ArRenderColorF){f->CloudColor[0],f->CloudColor[1],f->CloudColor[2],alpha},f->CloudShape[2],project_point,userdata);
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

static uint32_t TimberSeed(const ActionMarshField *m,const ActionBloodpoolTimber *edge) {
  return DeterministicHash_Mix32((unsigned)edge->x0+(unsigned)edge->y*(unsigned)m->Seeds[0]);
}

static float DripAge(const ActionMarshField *m,const ActionBloodpoolTimber *edge, uint16_t ticks, float *fall_time) {
  const float distance = edge->landing_y-edge->drip_y;
  if (distance < m->DripTiming[0] || distance > m->DripTiming[1]) { *fall_time = 0; return -1; }
  const uint32_t seed = TimberSeed(m,edge);
  const float gravity = m->DripTiming[2]+m->DripTiming[3]*HashUnit(seed^(unsigned)m->Seeds[1]);
  *fall_time = sqrtf(distance/gravity);
  const unsigned mask = (unsigned)((seed&8u) ? m->DripTiming[4] : m->DripTiming[5])-1;
  return (float)((ticks+(seed>>8))&mask);
}

static bool WaterClip(const ActionMarshField *m,const ActionEffectInstance *effect, const ActionEffectLocalRect *clip,
    float world_x, float water_top, ActionEffectLocalRect *region) {
  for (unsigned pool = 0; pool < (unsigned)m->SpanCount[0]; pool++) {
    if (!(effect->source_mask&(1u<<pool))) continue;
    const float left = m->spans[pool][0]+m->Surface[3];
    const float right = m->spans[pool][1]-m->Surface[3];
    if (world_x < left || world_x >= right) continue;
    *region = *clip;
    region->x0 = fmaxf(region->x0,left-effect->world_x);
    region->x1 = fminf(region->x1,right-effect->world_x);
    region->y0 = fmaxf(region->y0,water_top-effect->world_y);
    region->y1 = fminf(region->y1,m->Surface[2]-effect->world_y);
    return region->x0 < region->x1 && region->y0 < region->y1;
  }
  return false;
}

static bool Ripple(const ActionMarshField *m,ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectLocalRect *clip, float world_x, float world_y, float water_top, float age,
    float strength, ActionEffectProjectPointFn project_point, void *userdata) {
  if (age < 0 || age >= m->RippleShape[0]) return true;
  ActionEffectLocalRect region;
  if (!WaterClip(m,mesh,clip,world_x,water_top,&region)) return true;
  const float t = age/m->RippleShape[0], radius = m->RippleShape[1]+m->RippleShape[2]*t;
  const float alpha = strength*sinf(t*3.14159265f)*(1-t);
  const float x = world_x-mesh->world_x, y = world_y-mesh->world_y;
  if (x+radius < region.x0 || x-radius > region.x1 || y+5 < region.y0 || y-5 > region.y1)
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
    const float r = radius+((int)band-1)*m->RippleShape[3];
    const unsigned at = i*3+band;
    vertices[at] = (ArRenderVertex2D){
      {x+circle[i].x*r,y+circle[i].y*r*m->RippleShape[4]},
      {m->RippleColor[0],m->RippleColor[1],m->RippleColor[2],band == 1 ? alpha : 0},{0,0}};
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

static bool TimberDrops(const ActionMarshField *m,ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionBloodpoolDetails *details, const ActionEffectLocalRect *clip,
    bool water, ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < details->timber_count; i++) {
    const ActionBloodpoolTimber *edge = &details->timber[i];
    float fall_time;
    const float age = DripAge(m,edge,mesh->phase_ticks,&fall_time);
    if (age < 0) continue;
    if (water) {
      if (!edge->water_landing) continue;
      if (!Ripple(m,writer,mesh,clip,edge->drip_x,edge->landing_y+m->RippleDrip[0],m->Surface[1],age-fall_time,m->RippleDrip[1],
              project_point,userdata) ||
          !Ripple(m,writer,mesh,clip,edge->drip_x,edge->landing_y+m->RippleDrip[0],m->Surface[1],age-fall_time-m->RippleDrip[2],m->RippleDrip[3],
              project_point,userdata)) return false;
    } else if (age < fall_time) {
      const float t = age/fall_time;
      const float y = edge->drip_y+(edge->landing_y-edge->drip_y)*t*t-mesh->world_y;
      const float alpha = fminf(1,age/m->DripShape[0])*m->DripShape[1];
      const float height = fminf(m->DripShape[2]+m->DripShape[3]*t,edge->landing_y-mesh->world_y-y);
      if (!DetailSpark(writer,mesh,clip,edge->drip_x-mesh->world_x,y,m->DripShape[4],height,
              (ArRenderColorF){m->DripColor[0],m->DripColor[1],m->DripColor[2],alpha},project_point,userdata)) return false;
    }
  }
  return true;
}

static bool MarshInsects(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer, const ActionEffectInstance *mesh,
    const ActionEffectInstance *moon, const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project_point, void *userdata) {
  BloodpoolMoonProjection light;
  const bool lit = moon && BloodpoolMoonProjection_Init(f, &light,moon,project_point,userdata);
  for (unsigned pool = 0; pool < (unsigned)m->SpanCount[0]; pool++) {
    if (!(mesh->source_mask&(1u<<pool))) continue;
    for (unsigned side = 0; side < 2; side++) {
      const float bank = side ? m->spans[pool][1]-m->InsectRegion[0] :
                                m->spans[pool][0]+m->InsectRegion[0];
      if (bank-mesh->world_x < clip->x0-16 || bank-mesh->world_x > clip->x1+16) continue;
      for (unsigned fly = 0; fly < (unsigned)m->InsectCount[0]; fly++) {
        const uint32_t seed = DeterministicHash_Mix32(pool*32+side*11+fly*3+(unsigned)m->Seeds[3]);
        const float t = ((mesh->phase_ticks+seed)&((unsigned)m->InsectMotion[0]-1))*m->InsectMotion[1];
        const float x = bank+m->InsectMotion[2]*sinf(t*m->InsectMotion[3])+m->InsectMotion[4]*cosf(t*m->InsectMotion[5])-mesh->world_x;
        const float y = m->InsectRegion[1]+m->InsectRegion[2]*HashUnit(seed)+m->InsectMotion[6]*sinf(t*m->InsectMotion[7]+m->InsectMotion[8])-mesh->world_y;
        ArRenderPointF p;
        if (!project_point(userdata,mesh,x,y,&p)) continue;
        const float exposure = lit ? BloodpoolMoonProjection_Light(&light,p) : 0;
        float flash = fmaxf(0,sinf(t+m->InsectMotion[9]));
        flash *= flash;
        flash *= flash;
        flash *= flash;
        const float alpha = m->InsectGain[0]+m->InsectGain[1]*exposure+m->InsectGain[2]*flash;
        if (flash > m->InsectGain[3] && !DetailSpark(writer,mesh,clip,x,y,m->InsectShape[0],m->InsectShape[1],
                (ArRenderColorF){m->InsectColor[0],m->InsectColor[1],m->InsectColor[2],m->InsectGain[4]*flash},project_point,userdata)) return false;
        if (!DetailSpark(writer,mesh,clip,x,y,m->InsectShape[2]+m->InsectShape[3]*flash,m->InsectShape[4]+m->InsectShape[5]*flash,
                (ArRenderColorF){m->InsectColor[0],m->InsectColor[1],m->InsectColor[2],alpha},project_point,userdata)) return false;
      }
    }
  }
  return true;
}

bool AppendBloodpoolDetailParticles(const ActionMoonField *f, const ActionMarshField *m, ActionEffectGeometryWriter *writer,
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
    return TimberDrops(m,writer,&mesh,details,&clip,false,project_point,userdata) &&
        MarshInsects(f, m, writer,&mesh,moon,&clip,project_point,userdata);
  if (effect->kind == kActionEffect_BloodpoolWater) {
    if (!TimberDrops(m,writer,&mesh,details,&clip,true,project_point,userdata)) return false;
    for (unsigned i = 0; i < details->post_count; i++) {
      const ActionBloodpoolPost *post = &details->posts[i];
      const float x = (post->x0+post->x1)*.5f;
      const uint32_t seed = DeterministicHash_Mix32((unsigned)(post->x0+post->x1)+(unsigned)m->Seeds[2]);
      const float age = (float)((effect->phase_ticks+seed)&((unsigned)m->RipplePost[0]-1));
      if (!Ripple(m,writer,&mesh,&clip,x,post->y,m->Surface[0],age,m->RipplePost[1],project_point,userdata) ||
          !Ripple(m,writer,&mesh,&clip,x,post->y,m->Surface[0],age-m->RipplePost[2],m->RipplePost[3],project_point,userdata))
        return false;
    }
  } else if (effect->kind == kActionEffect_BloodpoolMist) {
    for (unsigned i = 0; i < details->timber_count; i++) {
      const ActionBloodpoolTimber *edge = &details->timber[i];
      const uint32_t seed = TimberSeed(m,edge);
      if (!edge->water_landing || edge->y < m->UnderMist[0] || (seed&3u)) continue;
      const float t = ((effect->phase_ticks+seed)&((unsigned)m->UnderMist[2]-1))*m->UnderMist[3];
      const float x = edge->drip_x-effect->world_x+m->UnderMist[4]*sinf(t);
      ActionEffectLocalRect region = clip;
      /* Gather at water level beneath low walkways, with the same shoreline
       * clipping as the base mist. Never invent a floating elevated floor. */
      bool found = false;
      for (unsigned pool = 0; pool < (unsigned)m->SpanCount[0]; pool++) {
        const float left = m->spans[pool][0]+m->Surface[3];
        const float right = m->spans[pool][1]-m->Surface[3];
        if (!(effect->source_mask&(1u<<pool)) ||
            edge->drip_x < left || edge->drip_x >= right) continue;
        region.x0 = fmaxf(region.x0,left-effect->world_x);
        region.x1 = fminf(region.x1,right-effect->world_x);
        found = true;
        break;
      }
      if (!found || region.x0 >= region.x1) continue;
      if (!AppendSceneSoftPatch(writer,&mesh,&region,x,m->UnderMist[1]-effect->world_y,
              m->UnderMist[5]+m->UnderMist[6]*sinf(t),m->UnderMist[7],(ArRenderColorF){m->UnderMistColor[0],m->UnderMistColor[1],m->UnderMistColor[2],m->UnderMistColor[3]},m->UnderMist[8]*cosf(t),
              project_point,userdata)) return false;
    }
  }
  return true;
}
