/* Small portable emitter kernels: deterministic time, finite geometry and no
 * particle simulation or GPU allocation. All backends consume these batches. */
#include "action_effect_render_internal.h"
#include "action_authored_particles.h"

/* Three density slices over fixed world-space samples. Clip the grid in local
 * space before projection; never clamp projected beam/mesh vertices to screen
 * edges. Work is bounded at 45 vertices / 144 indices per support span. */
static bool FloorMist(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectFloorField *field, const ActionEffectLocalRect *clip,
    ArRenderColorF tint, ActionEffectProjectPointFn project, void *context) {
  if (!field || field->count>kActionAuthoredFloorMaxSpans) return false;
  ActionEffectInstance mesh=*e;mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float phase=(float)(EffectVisualTicks(e,e->pulse_ticks)%e->particle_lifetime)/e->particle_lifetime*6.2831853f;
  const float seed=HashUnit(e->generation)*6.2831853f;
  for (unsigned i=0;i<field->count;++i) {
    const ActionEffectFloorSpan *s=&field->spans[i];
    const float left=s->x0-e->world_x,right=s->x1-e->world_x,floor=s->y-e->world_y;
    if (!isfinite(left)||!isfinite(right)||!isfinite(floor)||!isfinite(s->height)||
        right<=left||right-left>512||s->height<=0||s->height>64) return false;
    if (left<e->geometry.data.rect.x0||right>e->geometry.data.rect.x1||
        floor>e->geometry.data.rect.y1||floor-s->height<e->geometry.data.rect.y0) return false;
    if (right<=clip->x0||left>=clip->x1||floor<=clip->y0||floor-s->height>=clip->y1) continue;
    for (unsigned slice=0;slice<3;++slice) {
      if (!Reserve(w,15,48)) return false;
      const int base=w->vertex_count;
      for (unsigned row=0;row<3;++row) for (unsigned col=0;col<5;++col) {
        const float x=fmaxf(clip->x0,fminf(clip->x1,left+(right-left)*col*.25f));
        const float wx=x+e->world_x;
        const float billow=.73f+.16f*sinf(wx*.053f+phase+seed+slice)+.11f*cosf(wx*.097f-phase+slice);
        const float top=s->height*(.9f-.15f*slice)*billow;
        const float y=fmaxf(clip->y0,fminf(clip->y1,floor-top*(1-row*.5f)));
        const float rise=fmaxf(0,fminf(1,(floor-y)/top));
        const float edge=fmaxf(0,fminf(1,fminf(x-e->geometry.data.rect.x0,e->geometry.data.rect.x1-x)/12));
        ArRenderVertex2D v={.color=tint};
        v.color.a=fminf(1,e->tuning.intensity*(.14f+.08f*billow)*(1-rise)*(1-rise)*edge);
        if (!project(context,&mesh,x,y,&v.position)) return false;
        w->vertices[w->vertex_count++]=v;
      }
      for (int row=0;row<2;++row) for (int col=0;col<4;++col) {
        const int a=base+row*5+col,b=a+5;
        const int indices[]={a,a+1,b,a+1,b+1,b};
        for (unsigned k=0;k<6;++k)w->indices[w->index_count++]=indices[k];
      }
    }
  }
  return true;
}

bool AppendAuthoredEnvironment(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionEffectFloorField *floor, bool lighting, bool particles,
    ActionEffectProjectPointFn project, ActionEffectClipBoundsFn clip, void *context) {
  if (!writer || !effect || !project) return false;
  const ActionEffectLocalRect *r = &effect->geometry.data.rect;
  if (effect->geometry.kind != kActionEffectGeometry_Rect || !RectIsSane(r) ||
      r->x1-r->x0 > ((effect->kind==kActionEffect_AuthoredParticleArea||effect->kind==kActionEffect_AuthoredExposure||effect->kind==kActionEffect_AuthoredGradient)?16384:512) ||
      r->y1-r->y0 > ((effect->kind==kActionEffect_AuthoredParticleArea||effect->kind==kActionEffect_AuthoredExposure||effect->kind==kActionEffect_AuthoredGradient)?16384:512) || effect->particle_count > 128 ||
      effect->particle_lifetime < 16 || effect->particle_lifetime > 4096 ||
      !isfinite(effect->tuning.intensity) || effect->tuning.intensity < 0 || effect->tuning.intensity > 4)
    return false;
  if(effect->kind>=kActionEffect_AuthoredParticleArea&&effect->kind!=kActionEffect_AuthoredFlame)
    return AppendAuthoredField(writer,effect,lighting,particles,project,clip,context);
  ActionEffectParticleStyle style=effect->particle_style.active ? effect->particle_style :
      ActionAuthoredParticles_Default(r->y1-r->y0,effect->generation);
  if (!effect->particle_style.active) style.color_end=effect->tuning.color;
  ActionEffectInstance motion=*effect;
  if (effect->kind==kActionEffect_AuthoredMotes||effect->kind==kActionEffect_AuthoredFlame) {
    if (!ActionAuthoredParticles_Valid(&style)) return false;
    motion.geometry.data.rect=ActionAuthoredParticles_Bounds(r,&style);
    motion.pulse_generation=style.seed;
    if(effect->kind==kActionEffect_AuthoredFlame) {
      motion.geometry.data.rect.x0=fminf(motion.geometry.data.rect.x0,r->x0);
      motion.geometry.data.rect.x1=fmaxf(motion.geometry.data.rect.x1,r->x1);
      motion.geometry.data.rect.y0=fminf(motion.geometry.data.rect.y0,r->y0-(r->y1-r->y0)*.3f);
      motion.geometry.data.rect.y1=fmaxf(motion.geometry.data.rect.y1,r->y1);
    }
  }
  ActionEffectLocalRect visible=motion.geometry.data.rect;
  if (clip && !clip(context,&motion,&visible)) return true;
  const uint32_t color = effect->tuning.color;
  const ArRenderColorF tint = {((color>>16)&255)/255.0f,((color>>8)&255)/255.0f,
      (color&255)/255.0f,1};
  const float intensity = effect->tuning.intensity;
  const float width = r->x1-r->x0, height = r->y1-r->y0;
  if (effect->kind == kActionEffect_AuthoredLight) {
    if (!lighting) return true;
    ActionEffectGlowStyle glow = {.radius_x=width*.5f,.radius_y=height*.5f,
      .ring_scale={.16f,.58f,1}, .centre=tint, .ring={tint,tint,tint}, .axis_x=1};
    glow.centre.a=.20f*intensity;glow.ring[0].a=.16f*intensity;
    glow.ring[1].a=.05f*intensity;glow.ring[2].a=0;
    return AppendGlow(writer,effect,&glow,1,0,0,project,context);
  }
  if(effect->kind==kActionEffect_AuthoredFlame&&lighting) {
    const unsigned ticks=EffectVisualTicks(effect,effect->pulse_ticks);
    const uint32_t seed=effect->generation^style.seed;
    const float pulse=.78f+.14f*HashUnit(seed^(ticks/7))+.08f*HashUnit(seed^(ticks/23));
    ActionEffectGlowStyle glow={.radius_x=width*.5f,.radius_y=height*.5f,
      .ring_scale={.16f,.54f,1},.centre=tint,.ring={tint,tint,tint},.axis_x=1,.lift_y=-1,.flare=.2f,.rise=.25f};
    glow.centre.a=.18f*intensity;glow.ring[0].a=.14f*intensity;glow.ring[1].a=.045f*intensity;glow.ring[2].a=0;
    if(!AppendGlow(writer,effect,&glow,pulse,0,-height*.15f,project,context))return false;
    const uint32_t hot=style.color_end;
    glow.radius_x=width*.13f;glow.radius_y=height*.32f;glow.flare=.34f;glow.rise=.5f;
    glow.centre=(ArRenderColorF){((hot>>16)&255)/255.f,((hot>>8)&255)/255.f,(hot&255)/255.f,.8f*intensity};
    glow.ring[0]=glow.centre;glow.ring[0].a=.6f*intensity;glow.ring[1]=tint;glow.ring[1].a=.15f*intensity;
    if(!AppendGlow(writer,effect,&glow,pulse,0,-height*.16f,project,context))return false;
  }
  if (!particles) return true;
  if (effect->kind == kActionEffect_AuthoredFloorMist)
    return FloorMist(writer,effect,floor,&visible,tint,project,context);
  const unsigned ticks=EffectVisualTicks(effect,effect->pulse_ticks);
  if (effect->kind == kActionEffect_AuthoredMotes || effect->kind==kActionEffect_AuthoredFlame) {
    const ArRenderColorF end={((style.color_end>>16)&255)/255.0f,
      ((style.color_end>>8)&255)/255.0f,(style.color_end&255)/255.0f,1};
    for (unsigned i=0;i<effect->particle_count;++i) {
      const uint32_t seed=style.seed ^ (i*0x9e3779b9u);
      const SceneParticleClock clock=SceneParticleClockAt(&motion,ticks,i,
          (SceneParticleLifetime){effect->particle_lifetime,0,0});
      const float x=(r->x0+r->x1)*.5f+width*style.spread*(HashUnit(seed)-.5f)+
          style.travel_x*clock.t+sinf(clock.t*6.283185f+HashUnit(seed^71)*6)*style.wander;
      const float y=style.travel_y < 0 ? (effect->kind==kActionEffect_AuthoredFlame?0:r->y1)+style.travel_y*clock.t :
          style.travel_y > 0 ? r->y0+style.travel_y*clock.t : r->y0+height*HashUnit(seed^91);
      const float size=style.size_min+(style.size_max-style.size_min)*HashUnit(seed^27);
      ArRenderColorF c=MixColor(tint,end,clock.t);
      c.a=fmaxf(0,fminf(1,intensity*.6f*sinf(clock.t*3.141593f)));
      if (!AppendSceneParticle(writer,&motion,x,y,x-style.travel_x*.003f,y-style.travel_y*.003f,
          size,fmaxf(.6f,size*.8f),c,project,context)) return false;
    }
    return true;
  }
  if (effect->kind == kActionEffect_AuthoredMist) {
    for (unsigned i=0;i<8;++i) {
      const uint32_t seed=effect->generation ^ (i*0x9e3779b9u);
      const float phase=(float)(ticks % effect->particle_lifetime)/effect->particle_lifetime*6.283185f;
      const float x=r->x0+width*(.15f+.7f*HashUnit(seed))+
          sinf(phase+HashUnit(seed^7)*6)*width*.04f;
      const float y=r->y0+height*(.3f+.4f*HashUnit(seed^91));
      if (!AppendSceneSoftCloud(writer,effect,x,y,width*(.16f+.08f*HashUnit(seed^11)),
          height*.32f,tint,.10f*intensity,seed,project,context)) return false;
    }
    return true;
  }
  return false;
}
