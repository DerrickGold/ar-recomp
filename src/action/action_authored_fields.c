#include "action_woodland.h"
#include "action_trunk_life.h"
#include "render/cloud_motion.h"
#include "render/mist_rainbow.h"
/* Pure authoring kernels. All storage belongs to the caller's retained batch;
 * no native actor allocation, simulation state, texture or extra GPU resolve. */
#include "action_effect_render_internal.h"
#include "action_authored_particles.h"
#include "action_exposure.h"
#include "action_effect_masks.h"
#include "action_light_kinds.h"
#include "action_soft_light.h"
#include "action_storm.h"
#include "action_starfield.h"

typedef struct ExposureWriter {
  ActionEffectGeometryWriter *writer;
  ActionEffectInstance mesh;
  ActionEffectLocalRect clip;
  ActionEffectProjectPointFn project;
  void *context;
} ExposureWriter;
static bool ExposureQuad(void *context,const float xs[2],const float ys[2],const float factors[4]) {
  ExposureWriter *out=context;
  const bool bright=ActionExposure_Brightness(&out->mesh)>1;
  const float points[4][2]={{xs[0],ys[0]},{xs[1],ys[0]},{xs[1],ys[1]},{xs[0],ys[1]}};
  ArRenderVertex2D vertices[4];int mapped[4]={-1,-1,-1,-1};
  for(unsigned i=0;i<4;++i) {
    const float delta=factors[i]-1;
    vertices[i]=(ArRenderVertex2D){.position={points[i][0]-out->mesh.world_x,points[i][1]-out->mesh.world_y},
      .color=bright?(ArRenderColorF){delta,delta,delta,1}:(ArRenderColorF){0,0,0,-delta}};
  }
  const int indices[]={0,1,2,0,2,3};
  for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(out->writer,&out->mesh,vertices,mapped,indices+t,&out->clip,out->project,out->context))return false;
  return true;
}
bool AppendExposureGroup(ActionEffectGeometryWriter *writer,const ActionEffectInstance *const *boxes,unsigned count,
    ActionEffectProjectPointFn project,ActionEffectClipBoundsFn clip_fn,void *context) {
  if(!count)return true;
  ExposureWriter out={.writer=writer,.mesh=*boxes[0],.project=project,.context=context};
  ActionEffectLocalRect bounds=boxes[0]->geometry.data.rect;
  bounds.x0+=out.mesh.world_x;bounds.x1+=out.mesh.world_x;bounds.y0+=out.mesh.world_y;bounds.y1+=out.mesh.world_y;
  for(unsigned i=0;i<count;++i) {
    const ActionEffectInstance *e=boxes[i];const ActionEffectLocalRect *r=&e->geometry.data.rect;
    const float brightness=ActionExposure_Brightness(e);
    if(!isfinite(brightness)||brightness<0||brightness>2)return false;
    for(unsigned j=0;j<4;++j)if(!isfinite(e->field_style.fade[j])||e->field_style.fade[j]<0||e->field_style.fade[j]>16384)return false;
    bounds.x0=fminf(bounds.x0,e->world_x+r->x0);bounds.x1=fmaxf(bounds.x1,e->world_x+r->x1);
    bounds.y0=fminf(bounds.y0,e->world_y+r->y0);bounds.y1=fmaxf(bounds.y1,e->world_y+r->y1);
  }
  if(ActionExposure_Brightness(&out.mesh)==1)return true;
  out.mesh.flags|=kActionEffectFlag_ClippedMesh;
  out.clip=(ActionEffectLocalRect){bounds.x0-out.mesh.world_x,bounds.y0-out.mesh.world_y,bounds.x1-out.mesh.world_x,bounds.y1-out.mesh.world_y};
  out.mesh.geometry.data.rect=out.clip;
  if(clip_fn&&!clip_fn(context,&out.mesh,&out.clip))return true;
  bounds=(ActionEffectLocalRect){out.clip.x0+out.mesh.world_x,out.clip.y0+out.mesh.world_y,out.clip.x1+out.mesh.world_x,out.clip.y1+out.mesh.world_y};
  return ActionExposure_Visit(boxes,count,&bounds,ExposureQuad,&out);
}
static ArRenderColorF FieldColor(uint32_t color) {
  return (ArRenderColorF){((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
}
/* A few broad, staggered reflections per flight, on the same quad. Keeping
 * the lifetime envelope separate prevents glints from reviving fading grains
 * or popping at the emitter boundary. Camera position never enters the clock. */
static void Twinkle(float strength, uint32_t seed, float age, float envelope, ArRenderColorF *color) {
  if(strength<=0)return;
  float pulse=fmaxf(0,sinf(age*(12+10*HashUnit(seed^103))+HashUnit(seed^53)*6.2831853f));
  pulse*=pulse;pulse*=pulse;pulse*=pulse;
  const float reflection=strength*pulse;
  color->r+=(1-color->r)*reflection;
  color->g+=(1-color->g)*reflection;
  color->b+=(1-color->b)*reflection;
  color->a+=fmaxf(0,envelope-color->a)*reflection;
}
static unsigned LightingWorldPlane(unsigned plane) {
  /* Priority changes compositing order; BG2 has a different scrolling origin. */
  if(plane==kActionEffectProjectionPlane_Obj||plane==kActionEffectProjectionPlane_Bg1High)
    return kActionEffectProjectionPlane_Bg1;
  if(plane==kActionEffectProjectionPlane_Bg2High)return kActionEffectProjectionPlane_Bg2;
  return plane;
}
/* Direct sunlight only: ambient halos must not create water reflections in
 * dark pockets. Cache the authored field once per batch. Samples remain in
 * world coordinates, including the BG1/BG2 translation captured for mist. */
static ArRenderColorF DirectSkyLight(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    float x,float y) {
  ArRenderColorF incoming={0};
  if(!w->frame||w->atmosphere_unlit)return incoming;
  if(!w->cloud_light_ready) {
    w->cloud_light=ActionCloudLight_PrepareWithSun(w->frame,0,0,0,0,w->sun_directions);
    w->cloud_light_ready=true;
  }
  const bool foreground=LightingWorldPlane(e->projection_plane)==kActionEffectProjectionPlane_Bg1;
  x+=e->world_x+(foreground?w->frame->bg1_to_bg2_x:0);
  y+=e->world_y+(foreground?w->frame->bg1_to_bg2_y:0);
  for(unsigned i=0;i<(unsigned)w->cloud_light.meta[0];++i) {
    const DioramaCloudLightRegion *light=&w->cloud_light.regions[i];
    if(light->core[2]==0||light->color[3]==0)continue;
    const CloudLightSample s=DioramaCloudLight_Sample(light,x,y);
    const float strength=s.streak*light->core[2]*light->color[3];
    incoming.r+=(light->color[0]*.8f+light->spill[0]*.2f)*strength;
    incoming.g+=(light->color[1]*.8f+light->spill[1]*.2f)*strength;
    incoming.b+=(light->color[2]*.8f+light->spill[2]*.2f)*strength;
    incoming.a+=strength;
  }
  return incoming;
}
static ArRenderColorF FieldIncidentLight(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    float x,float y) {
  if(!w->frame||w->atmosphere_unlit)return (ArRenderColorF){0};
  const bool foreground=LightingWorldPlane(e->projection_plane)==kActionEffectProjectionPlane_Bg1;
  const ActionSceneEffectFrame *f=w->frame;float strength=0;ArRenderColorF incoming={0};
  for(unsigned i=0;i<f->authored_count&&i<kActionAuthoredMaxInstances;++i) {
    const ActionEffectInstance *light=&f->authored[i];
    if(!(light->flags&kActionEffectFlag_Visible)||(light->flags&kActionEffectFlag_LightingOff))continue;
    if(light->kind==kActionEffect_AuthoredLight &&
        LightingWorldPlane(light->projection_plane)==LightingWorldPlane(e->projection_plane)) {
      const ArRenderColorF sample=ActionSoftLight_Sample(light,e->world_x+x,e->world_y+y);
      strength+=sample.a;incoming.r+=sample.r;incoming.g+=sample.g;incoming.b+=sample.b;
      continue;
    }
    if(!foreground||light->kind!=kActionEffect_AuthoredCanopy)continue;
    const float amount=ActionCanopy_Sample(light,&f->authored_floor[i],e->world_x+x,e->world_y+y);
    const ArRenderColorF color=FieldColor(light->tuning.color);
    strength+=amount;incoming.r+=color.r*amount;incoming.g+=color.g*amount;incoming.b+=color.b*amount;
  }
  const ArRenderColorF sky=DirectSkyLight(w,e,x,y);
  strength+=sky.a;incoming.r+=sky.r;incoming.g+=sky.g;incoming.b+=sky.b;
  if(foreground) {
    if(!w->forest_light_ready){ActionForestLight_Prepare(f,&w->forest_light);w->forest_light_ready=true;}
    const ArRenderColorF native=ActionForestLight_Sample(&w->forest_light,e->world_x+x,e->world_y+y);
    incoming.r+=native.r;incoming.g+=native.g;incoming.b+=native.b;strength+=native.a;
  }
  incoming.a=strength;return incoming;
}
/* Small reflective particles sample the same captured light transport as
 * terrain and mist. Dark pockets cannot sparkle merely because they are in
 * the viewport. This is opt-in for particle regions/paths. */
static void ParticleLightTint(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    float x,float y,ArRenderColorF *tint,bool lighting) {
  if(e->field_style.canopy_response<=0)return;
  if(e->field_style.pattern==kActionParticle_IceCrystals&&
      (!w->frame||w->atmosphere_unlit)) {
    tint->a*=1-e->field_style.canopy_response;
    return;
  }
  if(e->field_style.pattern==kActionParticle_WaterGlints&&!lighting) {
    tint->a*=1-e->field_style.canopy_response;
    return;
  }
  const bool foreground=LightingWorldPlane(e->projection_plane)==kActionEffectProjectionPlane_Bg1;
  if(!w->frame||w->atmosphere_unlit) {
    if(!foreground)tint->a*=1-e->field_style.canopy_response;
    return;
  }
  const ArRenderColorF incoming=FieldIncidentLight(w,e,x,y);
  const float strength=incoming.a;
  const float response=e->field_style.canopy_response;
  tint->a*=1-response+response*fminf(1,strength*3);
  if(strength>0) {
    tint->r*=1-response+response*incoming.r/strength;
    tint->g*=1-response+response*incoming.g/strength;
    tint->b*=1-response+response*incoming.b/strength;
  }
}

static void ReflectedPlantTint(ArRenderColorF light,float response,ArRenderColorF *tint) {
  if(response<=0)return;
  const float amount=fminf(1,light.a*3),shade=1-response*.22f;
  tint->r*=shade; tint->g*=shade; tint->b*=shade;
  if(light.a>0) {
    const float mix=amount*response*.32f;
    tint->r+=mix*(fminf(1,light.r/light.a)-tint->r);
    tint->g+=mix*(fminf(1,light.g/light.a)-tint->g);
    tint->b+=mix*(fminf(1,light.b/light.a)-tint->b);
  }
}
static void PlantLightTint(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    float x,float y,ArRenderColorF *tint,bool lighting) {
  if(!lighting||w->atmosphere_unlit||e->field_style.canopy_response<=0)return;
  ReflectedPlantTint(FieldIncidentLight(w,e,x,y),e->field_style.canopy_response,tint);
}

/* A gentle, periodic energy envelope shared by sky curtains and an optional
 * reflected-light gradient. Matching cycle lengths keeps their breathing in
 * step without hidden emitter links or renderer-owned animation history. */
static float SkyLightEnvelope(const ActionEffectInstance *e) {
  if(e->field_style.twinkle<=0||!e->particle_lifetime)return 1;
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  return 1-e->field_style.twinkle*(.5f-.5f*cosf(phase));
}
/* The gradient's mesh and incident-light samples use the same vertices. This
 * keeps snow reflections aligned with the light illuminating nearby actors. */
static ArRenderVertex2D GradientVertex(const ActionEffectInstance *e,unsigned col,unsigned row) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float dx=sinf(e->field_style.angle*.0174532925f),dy=cosf(e->field_style.angle*.0174532925f);
  const float width=r->x1-r->x0,height=r->y1-r->y0,span=fabsf(dx)*width+fabsf(dy)*height;
  const float x=r->x0+width*col*.25f,y=r->y0+height*row*.25f;
  const float t=fmaxf(0,fminf(1,.5f+(x*dx+y*dy)/span));
  const float edge=fminf(fminf(col,4-col),fminf(row,4-row))*.25f;
  const float fade=e->field_style.softness>0?fminf(1,edge/(e->field_style.softness*.5f)):1;
  ArRenderVertex2D out={.position={x,y},.color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),t)};
  out.color.a=fminf(1,e->tuning.intensity*.18f)*fade*SkyLightEnvelope(e);
  return out;
}
static ArRenderColorF GradientLight(const ActionEffectInstance *e,float x,float y) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  x-=e->world_x;y-=e->world_y;
  if(x<=r->x0||x>=r->x1||y<=r->y0||y>=r->y1)return (ArRenderColorF){0};
  const float u=(x-r->x0)*4/(r->x1-r->x0),v=(y-r->y0)*4/(r->y1-r->y0);
  const unsigned col=(unsigned)u,row=(unsigned)v;
  const float tx=u-col,ty=v-row;
  const bool upper=tx+ty<=1;
  const ArRenderColorF a=GradientVertex(e,col+(upper?0:1),row+(upper?0:1)).color;
  const ArRenderColorF b=GradientVertex(e,col+1,row).color,c=GradientVertex(e,col,row+1).color;
  const float wa=upper?1-tx-ty:tx+ty-1,wb=upper?tx:1-ty,wc=upper?ty:1-tx;
  ArRenderColorF out={a.r*wa+b.r*wb+c.r*wc,a.g*wa+b.g*wb+c.g*wc,
      a.b*wa+b.b*wb+c.b*wc,a.a*wa+b.a*wb+c.a*wc};
  out.a*=e->tuning.illumination_set?e->tuning.illumination:ActionLightKind_Illumination(e->kind);
  out.r*=out.a;out.g*=out.a;out.b*=out.a;
  return out;
}
/* Snow stays visible in shade. Only the extra specular flash needs incident
 * sunlight; turning lighting off cannot leave self-illuminated sparkles. */
static void ReflectiveGrainTint(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    float x,float y,uint32_t seed,float age,float envelope,ArRenderColorF *tint,bool lighting,bool glint) {
  const float response=e->field_style.canopy_response;
  const float twinkle=glint?e->field_style.twinkle:0;
  if(response<=0) {Twinkle(twinkle,seed,age,envelope,tint);return;}
  ArRenderColorF incoming={0};
  if(lighting&&w->frame&&!w->atmosphere_unlit) {
    incoming=DirectSkyLight(w,e,x,y);
    const unsigned plane=LightingWorldPlane(e->projection_plane);
    for(unsigned i=0;i<w->frame->authored_count&&i<kActionAuthoredMaxInstances;++i) {
      const ActionEffectInstance *light=&w->frame->authored[i];
      if(light->kind!=kActionEffect_AuthoredGradient||!(light->flags&kActionEffectFlag_Visible)||
          (light->flags&kActionEffectFlag_LightingOff)||LightingWorldPlane(light->projection_plane)!=plane)continue;
      const ArRenderColorF sample=GradientLight(light,e->world_x+x,e->world_y+y);
      incoming.r+=sample.r;incoming.g+=sample.g;incoming.b+=sample.b;incoming.a+=sample.a;
    }
  }
  const float lit=fminf(1,incoming.a*6);
  if(incoming.a>0) {
    const float mix=response*lit;
    tint->r+=(incoming.r/incoming.a-tint->r)*mix;
    tint->g+=(incoming.g/incoming.a-tint->g)*mix;
    tint->b+=(incoming.b/incoming.a-tint->b)*mix;
  }
  Twinkle(twinkle*lit,seed,age,envelope,tint);
}

static bool MaskQuad(ActionEffectGeometryWriter *,const ActionEffectInstance *,const ActionEffectLocalRect *,
    const ArRenderPointF [4],const ArRenderPointF [4],ArRenderColorF,const float [4],ActionEffectProjectPointFn,void *);
static bool MaskPatch(ActionEffectGeometryWriter *,const ActionEffectInstance *,const ActionEffectLocalRect *,
    unsigned,float,float,float,float,ArRenderColorF,ActionEffectProjectPointFn,void *);
/* Long holds and eased births/settling avoid the busy blinking of snow sparks.
 * Soft crystals reuse the glow atlas; near crystals keep a small faceted core. */
static float CrystalEnvelope(float age) {
  const float t=fmaxf(0,fminf(1,fminf(age,1-age)*6));
  return t*t*(3-2*t);
}
static bool IceCrystal(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,float x,float y,float dx,float dy,float size,
    ArRenderColorF color,ActionEffectProjectPointFn project,void *context) {
  const float softness=e->field_style.softness;
  if(!isfinite(softness)||softness<0||softness>1)return false;
  if(softness>0) {
    ArRenderColorF halo=color;halo.a*=softness*.7f;
    const float radius=size*(1.25f+softness);
    if(!MaskPatch(w,e,clip,kActionMaskGlow,x,y,radius,radius,halo,project,context))return false;
  }
  color.a*=1-softness*.85f;
  return AppendSceneParticle(w,e,x,y,x-dx,y-dy,size*.5f,size*1.15f,color,project,context);
}
static float WakeDisplacement(const ActionFieldWakes *wakes,float *x,float *y,bool ground) {
  float dx=0,dy=0,amount=0;
  for(unsigned i=0;i<kActionFieldWakeCount;++i) {
    const ActionFieldWake *w=&wakes->bodies[i];if(w->strength<=0)continue;
    const float u=(*x-w->x)/w->radius,v=(*y-w->y+(ground?8:20))/(ground?32:44);
    const float weight=w->strength*SceneSoftFalloff(u)*SceneSoftFalloff(v);
    const float side=w->dx<0?-1.f:w->dx>0?1.f:u<0?-1.f:1.f;
    dx+=weight*(side*16+u*6);dy-=weight*(14+4*cosf(u*3.1415927f));amount+=weight;
  }
  *x+=dx;*y+=dy;return fminf(1,amount);
}

/* A butterfly remains at a saved perch, then flies to the next with zero
 * endpoint velocity. The next cycle starts at that same perch: no respawn,
 * opacity blink or camera-dependent motion. Activity is the flying fraction. */
static bool ButterflyPath(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context,bool lighting) {
  if(e->particle_count>4||!e->particle_lifetime)return false;
  for(unsigned i=0;i<e->particle_count;++i) {
    const ActionButterflyPose pose=ActionWoodland_Butterfly(e,i,w->frame);
    const uint32_t seed=pose.seed;const float x=pose.x,y=pose.y;
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(seed^27);
    if(x+size*2<clip->x0||x-size*2>clip->x1||y+size*1.5f<clip->y0||y-size*1.5f>clip->y1)continue;
    const float flight=pose.flight,ticks=(e->pulse_ticks%4096)/4096.f*6.2831853f;
    const float flap=.12f+.88f*fabsf(sinf(ticks*448+HashUnit(seed^91)*6.28f));
    const float spread=.16f+.1f*(.5f+.5f*sinf(ticks*11+HashUnit(seed^17)*6.28f))+flight*flap;
    ArRenderColorF wing=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),HashUnit(seed^53));
    wing.a=fminf(1,e->tuning.intensity);PlantLightTint(w,e,x,y,&wing,lighting);
    for(unsigned side=0;side<2;++side) {
      const float sign=side?1:-1,reach=size*spread;
      const ArRenderPointF p[4]={{x,y-size},{x+sign*reach,y-size*1.1f},
          {x+sign*reach,y+size*.9f},{x,y+size*.8f}};
      const ArRenderPointF uv[4]={ActionEffectMasks_Uv(kActionMaskButterflyWing,0,0),
          ActionEffectMasks_Uv(kActionMaskButterflyWing,1,0),ActionEffectMasks_Uv(kActionMaskButterflyWing,1,1),
          ActionEffectMasks_Uv(kActionMaskButterflyWing,0,1)};
      ArRenderColorF tint=wing;if(!side){tint.r*=.83f;tint.g*=.83f;tint.b*=.83f;}
      if(!MaskQuad(w,e,clip,p,uv,tint,NULL,project,context))return false;
    }
    ArRenderColorF body=FieldColor(0x55432d);body.a=wing.a;
    const ArRenderPointF p[4]={{x-.22f,y-size*.75f},{x+.22f,y-size*.75f},
        {x+.22f,y+size*.8f},{x-.22f,y+size*.8f}},uv[4]={{0}};
    if(!MaskQuad(w,e,clip,p,uv,body,NULL,project,context))return false;
  }
  return true;
}

/* Ordered paths support crest streamers, surface slides, falling grit and
 * sparse stationary glints. Reactive butterflies use captured woodland events;
 * every path remains analytic, without a persistent particle simulation. */
static bool ParticlePath(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context,bool lighting) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->point_count<2||s->point_count>kActionContourMaxPoints||!isfinite(s->activity)||s->activity<.05f||s->activity>1||s->pattern>=kActionParticle_Count)return false;
  if(s->pattern==kActionParticle_Butterflies)return ButterflyPath(w,e,clip,project,context,lighting);
  float lengths[kActionContourMaxPoints]={0},total=0;
  for(unsigned i=1;i<s->point_count;++i) {
    const float length=hypotf(s->points[i].x-s->points[i-1].x,s->points[i].y-s->points[i-1].y);
    if(!isfinite(length)||length<.01f)return false;
    lengths[i]=total+=length;
  }
  ActionEffectInstance source=*e;source.pulse_generation=e->particle_style.seed;
  const bool glint=s->pattern==kActionParticle_Glints;
  const ActionEffectInstance *opening=ActionTrunk_Opening(w->frame,e->world_x,e->world_y);
  const float life=w->frame->trunk.gust;
  const float gust=ActionTrunk_Gust(opening,opening?hypotf(e->world_x-opening->world_x,e->world_y-opening->world_y):0);
  const float wind=ActionFieldMotion_Wind(e)*(opening?1-life*(.72f-.72f*gust):1);
  for(unsigned i=0;i<e->particle_count;++i) {
    const SceneParticleClock clock=SceneParticleClockAt(&source,e->pulse_ticks,i,
        (SceneParticleLifetime){e->particle_lifetime,9,127});
    if(clock.t>s->activity)continue;
    const float age=clock.t/s->activity;
    const float t=glint?HashUnit(e->particle_style.seed^i*71):s->pattern==kActionParticle_Dust?age*age:age;
    const float distance=t*total;
    unsigned segment=1;while(segment+1<s->point_count&&distance>lengths[segment])++segment;
    const float u=(distance-lengths[segment-1])/(lengths[segment]-lengths[segment-1]);
    const float dx=s->points[segment].x-s->points[segment-1].x,dy=s->points[segment].y-s->points[segment-1].y;
    const float length=lengths[segment]-lengths[segment-1];
    const float sway=sinf(age*9+HashUnit(clock.seed^57)*6)*e->particle_style.wander*sinf(age*3.1415927f);
    float x=s->points[segment-1].x+dx*u-dy/length*sway;
    float y=s->points[segment-1].y+dy*u+dx/length*sway;
    if(opening) {
      const float carried=life*gust*sinf(age*3.1415927f),dir=opening->world_x<512?1.f:-1.f;
      x+=dir*carried*12;y+=carried*(5*sinf(age*5)-3);
    }
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(clock.seed^27);
    const bool crystal=s->pattern==kActionParticle_IceCrystals;
    const float extent=size*(crystal?2.25f:2);
    if(x+extent<clip->x0||x-extent>clip->x1||y+extent<clip->y0||y-extent>clip->y1)continue;
    ArRenderColorF color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),age);
    float fade=crystal?CrystalEnvelope(age):fmaxf(0,sinf(age*3.1415927f));
    if(glint||s->pattern==kActionParticle_WaterGlints)fade=fade*fade*fade*fade;
    color.a=fminf(1,e->tuning.intensity*.65f*fade*wind);
    if(!crystal)Twinkle(s->twinkle,clock.seed,age,fade*wind*fminf(1,e->tuning.intensity),&color);
    ParticleLightTint(w,e,x,y,&color,lighting);
    if(crystal) {
      Twinkle(s->twinkle,clock.seed,age,fminf(1,color.a*1.25f),&color);
      if(!IceCrystal(w,e,clip,x,y,dx/length,dy/length,size,color,project,context))return false;
      continue;
    }
    if(s->pattern==kActionParticle_WaterGlints&&color.a<=.001f)continue;
    if(!AppendSceneParticle(w,e,x,y,x-dx/length,y-dy/length,size*.65f,size*(glint?1.6f:1),color,project,context))return false;
  }
  return true;
}
static bool LeafParticle(ActionEffectGeometryWriter *,const ActionEffectInstance *,const ActionEffectLocalRect *,
    float,float,float,float,float,float,ArRenderColorF,ActionEffectProjectPointFn,void *);
static bool FieldParticle(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectInstance *clock_source, unsigned index, uint32_t seed, float birth_x, float birth_y, const ActionEffectLocalRect *clip,
    const ActionFieldWakes *wakes,ActionEffectProjectPointFn project, void *context, bool lighting) {
  const ActionEffectParticleStyle *s=&e->particle_style;
  const SceneParticleClock clock=SceneParticleClockAt(clock_source,e->pulse_ticks,index,
      (SceneParticleLifetime){e->particle_lifetime,0,0});
  const float t=clock.t, phase=HashUnit(seed^index)*6.2831853f;
  float x=birth_x+s->travel_x*t+sinf(t*6.2831853f+phase)*s->wander;
  float y=birth_y+s->travel_y*t;
  const float size=s->size_min+(s->size_max-s->size_min)*HashUnit(clock.seed^27);
  float width=size,reach=fmaxf(.6f,size*.8f),dx=s->travel_x,dy=s->travel_y;
  switch(e->field_style.pattern) {
    case kActionParticle_Leaves:
      y+=sinf(t*12+phase)*s->wander*.25f;dx=cosf(t*12+phase);dy=sinf(t*12+phase);
      width=size*(.3f+.7f*fabsf(sinf(t*9+phase)));reach=size*1.5f;break;
    case kActionParticle_Snow: x+=sinf(t*12+phase)*s->wander*.2f;break;
    case kActionParticle_Sand: width=size*.45f;reach=size*1.4f;break;
    case kActionParticle_Insects: case kActionParticle_Scarabs:
      x+=sinf(t*18+phase)*s->wander*.3f;y+=cosf(t*15+phase)*s->wander*.3f;
      width=size*.6f;reach=size*1.4f;break;
    case kActionParticle_Sparks: y+=24*t*t;reach=size*(1+2*(1-t));break;
    case kActionParticle_Glints: x=birth_x;y=birth_y;dx=0;dy=1;reach=size*1.6f;break;
    case kActionParticle_WaterGlints: dx=0;dy=1;width=size*1.5f;reach=size*.65f;break;
    case kActionParticle_IceCrystals: dx=cosf(phase+t*1.5f);dy=sinf(phase+t*1.5f);width=reach=size*2.25f;break;
    default: break;
  }
  if(e->field_style.wake>0)WakeDisplacement(wakes,&x,&y,false);
  const bool leaf=e->field_style.textured&&e->field_style.pattern==kActionParticle_Leaves;
  const float ex=leaf?fabsf(dx)*reach+fabsf(dy)*width:fmaxf(width,reach);
  const float ey=leaf?fabsf(dy)*reach+fabsf(dx)*width:fmaxf(width,reach);
  if (x+ex<clip->x0 || x-ex>clip->x1 || y+ey<clip->y0 || y-ey>clip->y1) return true;
  ArRenderColorF color=MixColor(FieldColor(e->tuning.color),FieldColor(s->color_end),t);
  const bool crystal=e->field_style.pattern==kActionParticle_IceCrystals;
  const float fade=crystal?CrystalEnvelope(t):fmaxf(0,sinf(t*3.1415927f));
  color.a=fminf(1,e->tuning.intensity*.7f*fade);
  if(e->field_style.pattern==kActionParticle_Glints||e->field_style.pattern==kActionParticle_WaterGlints) {
    color.a=fminf(1,e->tuning.intensity*.7f*fade*fade*fade*fade);
  }
  if(e->field_style.pattern==kActionParticle_Insects)color.a*=.5f+.5f*sinf(t*25+phase)*sinf(t*25+phase);
  if(e->field_style.pattern==kActionParticle_Snow&&e->field_style.canopy_response>0)
    ReflectiveGrainTint(w,e,x,y,clock.seed,t,fade*fminf(1,e->tuning.intensity),&color,lighting,true);
  else if(crystal) {
    ParticleLightTint(w,e,x,y,&color,lighting);
    Twinkle(e->field_style.twinkle,clock.seed,t,fminf(1,color.a*1.25f),&color);
    return IceCrystal(w,e,clip,x,y,dx,dy,size,color,project,context);
  } else {
    Twinkle(e->field_style.twinkle,clock.seed,t,fade*fminf(1,e->tuning.intensity),&color);
    ParticleLightTint(w,e,x,y,&color,lighting);
  }
  if(e->field_style.pattern==kActionParticle_WaterGlints&&color.a<=.001f)return true;
  if(e->field_style.textured&&e->field_style.pattern==kActionParticle_Leaves)
    return LeafParticle(w,e,clip,x,y,dx,dy,width,reach,color,project,context);
  return AppendSceneParticle(w,e,x,y,x-dx*.003f,y-dy*.003f,width,reach,color,project,context);
}
static bool ParticleArea(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context,bool lighting) {
  if(!ActionAuthoredParticles_Valid(&e->particle_style) || e->particle_count>16 ||
      e->field_style.pattern>=kActionParticle_Count||!isfinite(e->field_style.wake)||
      e->field_style.wake<0||e->field_style.wake>1)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectParticleStyle *s=&e->particle_style;
  /* Cells are anchored in world space. Panning changes the visited set, never
   * seeds, ages or positions. Backtrack from the visible motion envelope to
   * birth cells; do not traverse or rasterize the complete saved region. */
  const float pad=s->wander*1.6f+s->size_max*3+25+44*e->field_style.wake;
  const float x0=fmaxf(r->x0,clip->x0-fmaxf(0,s->travel_x)-pad)+e->world_x;
  const float x1=fminf(r->x1,clip->x1-fminf(0,s->travel_x)+pad)+e->world_x;
  const float y0=fmaxf(r->y0,clip->y0-fmaxf(0,s->travel_y)-pad)+e->world_y;
  const float y1=fminf(r->y1,clip->y1-fminf(0,s->travel_y)+pad)+e->world_y;
  if(x1<=x0 || y1<=y0)return true;
  const int first_x=(int)floorf(x0/kActionParticleCellSize), last_x=(int)floorf(x1/kActionParticleCellSize);
  const int first_y=(int)floorf(y0/kActionParticleCellSize), last_y=(int)floorf(y1/kActionParticleCellSize);
  if((last_x-first_x+1)*(last_y-first_y+1)>kActionParticleMaxCells)return false;
  const ActionFieldWakes wakes=ActionFieldMotion_Gather(w->frame,e,clip,false);
  ActionEffectInstance clock_source=*e;
  for(int cy=first_y;cy<=last_y;++cy)for(int cx=first_x;cx<=last_x;++cx) {
    const uint32_t seed=DeterministicHash_Mix32(s->seed^(uint32_t)cx*0x9e3779b9u^(uint32_t)cy*0x85ebca6bu);
    clock_source.pulse_generation=seed;
    for(unsigned i=0;i<e->particle_count;++i) {
      const float x=(cx+.5f+(HashUnit(seed^i*71u)-.5f)*s->spread)*kActionParticleCellSize-e->world_x;
      const float y=(cy+HashUnit(seed^i*91u^51u))*kActionParticleCellSize-e->world_y;
      if(x<r->x0 || x>r->x1 || y<r->y0 || y>r->y1)continue;
      if(!FieldParticle(w,e,&clock_source,i,seed,x,y,clip,&wakes,project,context,lighting))return false;
    }
  }
  return true;
}
/* These are geometry-only light treatments. Both use the same clipping,
 * blend pass and receiver sampling as the other authored lights. */
static bool Halo(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const float softness=e->field_style.softness;
  if(!isfinite(softness)||softness<0||softness>1)return false;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float rx=(r->x1-r->x0)*.5f,ry=(r->y1-r->y0)*.5f;
  /* A dark centre with a feathered inner edge, a colored rim and a diffuse
   * outer corona. Increasing softness widens the ring, never fills the disc. */
  const float radius[]={.86f-.30f*softness,.91f-.16f*softness,.94f,1};
  const float alpha[]={0,.22f,.08f,0};
  ArRenderVertex2D vertices[128];int mapped[128];
  for(unsigned ring=0;ring<4;++ring)for(unsigned i=0;i<32;++i) {
    const unsigned n=ring*32+i;
    vertices[n]=(ArRenderVertex2D){.position={kCircle32[i][0]*rx*radius[ring],kCircle32[i][1]*ry*radius[ring]},
      .color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),ring/3.f)};
    vertices[n].color.a=fminf(1,alpha[ring]*e->tuning.intensity);mapped[n]=-1;
  }
  for(unsigned ring=0;ring<3;++ring)for(unsigned i=0;i<32;++i){
    const int a=ring*32+i,b=ring*32+(i+1)%32,triangles[]={a,b,a+32,b,b+32,a+32};
    for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
  }
  return true;
}
static bool LightGradient(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const float softness=e->field_style.softness,angle=e->field_style.angle;
  if(!isfinite(softness)||softness<0||softness>1||!isfinite(angle)||fabsf(angle)>180)return false;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  ArRenderVertex2D vertices[25];int mapped[25];
  for(unsigned row=0;row<5;++row)for(unsigned col=0;col<5;++col){
    const unsigned n=row*5+col;vertices[n]=GradientVertex(e,col,row);mapped[n]=-1;
  }
  for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
    const int a=row*5+col,b=a+5,triangles[]={a,a+1,b,a+1,b+1,b};
    for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
  }
  return true;
}
static bool LightFan(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,length=r->y1-r->y0;
  if(s->strands<1 || s->strands>32 || !isfinite(s->angle)||fabsf(s->angle)>180 ||
      !isfinite(s->fan)||s->fan<0||s->fan>120||!isfinite(s->softness)||s->softness<0||s->softness>1)return false;
  if(!isfinite(s->thickness)||s->thickness<0||s->thickness>1)return false;
  if(!isfinite(s->fade_in)||s->fade_in<0||s->fade_in>.5f)return false;
  /* Cloud openings vary transmission, not ray position. Keep the old static
   * fan exactly unchanged at zero; seeded phases make adjacent fans breathe
   * independently and remain deterministic when the editor seeks backward. */
  if(s->twinkle>0 && !e->particle_lifetime)return false;
  const float phase=s->twinkle>0 ? (e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime : 0;
  const float opening=s->twinkle>0 ? 1-s->twinkle*(.5f+.5f*sinf(phase+HashUnit(e->generation^113)*6.2831853f)) : 1;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const ArRenderColorF tint=FieldColor(e->tuning.color);
  for(unsigned i=0;i<s->strands;++i) {
    const float u=(i+.5f)/s->strands;
    const uint32_t seed=e->generation^i*0x9e3779b9u;
    const float angle=(s->angle+(u-.5f)*s->fan)*.0174532925f;
    const float dx=sinf(angle),dy=cosf(angle),rx=cosf(angle),ry=-sinf(angle);
    const bool fine=s->thickness>0&&s->thickness<1;
    const float origin=(u-.5f)*width+(fine?(HashUnit(seed^119)-.5f)*width/s->strands*.8f:0);
    const float reach=length*(fine?.55f+.45f*HashUnit(seed):.8f+.2f*HashUnit(seed));
    const float radius=fmaxf(fine?.1f:.4f,width/s->strands*(.2f+.2f*HashUnit(seed^71))*(s->thickness>0?s->thickness:1));
    const float transmission=opening*(s->twinkle>0 ? 1-s->twinkle*.35f*(.5f+.5f*sinf(phase+HashUnit(seed^151)*6.2831853f)) : 1);
    const unsigned rows=s->fade_in>0?4:3;
    const float distances[4]={0,s->fade_in,.65f,1},opacity[4]={0,1,.55f,0};
    ArRenderVertex2D vertices[12];int mapped[12];
    for(unsigned row=0;row<rows;++row)for(unsigned col=0;col<3;++col) {
      const unsigned n=row*3+col;const float t=s->fade_in>0?distances[row]:row*.5f,side=(float)col-1;
      const float fade=s->fade_in>0?opacity[row]:(row==2?0:row==0?.65f:1);
      vertices[n]=(ArRenderVertex2D){.position={origin+dx*reach*t+rx*side*radius*(1+2*t),
        dy*reach*t+ry*side*radius*(1+2*t)},.color=tint};
      vertices[n].color.a=col==1?transmission*fminf(1,e->tuning.intensity*(.10f+.06f*HashUnit(seed^27))*fade):0;
      mapped[n]=-1;
    }
    for(unsigned row=0;row+1<rows;++row)for(unsigned col=0;col<2;++col) {
      const int a=row*3+col,b=a+3,triangles[]={a,a+1,b,a+1,b+1,b};
      for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
    }
  }
  ArRenderColorF backdrop=tint;backdrop.a=opening*fminf(1,s->softness*e->tuning.intensity*.10f);
  const float angle=s->angle*.0174532925f;
  return !s->softness || AppendSceneSoftPatch(w,&mesh,clip,sinf(angle)*length*.4f,cosf(angle)*length*.4f,
      width*.5f+length*sinf(s->fan*.008726646f)*.4f,length*.48f,backdrop,sinf(angle)*length*.3f,project,context);
}
static bool WaterField(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;const ActionEffectFieldStyle *s=&e->field_style;
  ArRenderColorF tint=FieldColor(e->tuning.color);
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  ArRenderVertex2D vertices[18];int mapped[18];
  const float phase=ActionWaterWave_Phase(e);
  for(unsigned i=0;i<9;++i)for(unsigned row=0;row<2;++row) {
    const unsigned n=row*9+i;const float x=r->x0+(r->x1-r->x0)*i/8;
    const float crest=r->y0+sinf(x*.05f+phase+s->drift*.01f)*s->amplitude;
    vertices[n]=(ArRenderVertex2D){.position={x,crest+row*2},.color=tint};
    vertices[n].color.a=fminf(1,e->tuning.intensity*.20f)*(row?0:1);mapped[n]=-1;
  }
  for(unsigned i=0;i<8;++i) {
    const int triangles[]={i,i+1,i+9,i+1,i+10,i+9};
    for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
  }
  for(unsigned i=0;i<e->particle_count;++i) {
    const uint32_t seed=e->particle_style.seed^i*0x9e3779b9u;
    const float x=r->x0+(r->x1-r->x0)*HashUnit(seed), y=r->y0+sinf(x*.05f+phase+s->drift*.01f)*s->amplitude;
    if(x<clip->x0||x>clip->x1||y<clip->y0||y>clip->y1)continue;
    const SceneParticleClock clock=SceneParticleClockAt(e,e->pulse_ticks,i,(SceneParticleLifetime){e->particle_lifetime,0,0});
    const float glint=fmaxf(0,sinf(phase+HashUnit(seed^71)*6)),glint_squared=glint*glint;
    ArRenderColorF c=MixColor(tint,FieldColor(e->particle_style.color_end),clock.t);c.a=fminf(1,e->tuning.intensity*.7f*glint_squared*glint_squared);
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(seed^11);
    if(!AppendSceneParticle(w,&mesh,x,y,x-1,y,size,size*2,c,project,context))return false;
  }
  return true;
}
static bool SlopedPowderPatch(ActionEffectGeometryWriter *,const ActionEffectInstance *,
    const ActionEffectLocalRect *,unsigned,float,float,float,float,float,
    ArRenderColorF,ActionEffectProjectPointFn,void *);
static bool FallingWater(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!isfinite(s->activity)||s->activity<.05f||s->activity>1||s->ripple>1||
      s->debris>1||s->landing>2||(s->landing&&!s->debris)||(s->debris&&s->ripple)||
      !isfinite(s->canopy_response)||s->canopy_response<0||s->canopy_response>1)return false;
  ActionEffectInstance source=*e;source.pulse_generation=e->particle_style.seed;
  for(unsigned i=0;i<e->particle_count;++i) {
    const SceneParticleClock clock=SceneParticleClockAt(&source,
        s->debris?(unsigned)e->pulse_ticks+e->particle_lifetime-(i*4)%e->particle_lifetime:e->pulse_ticks,
        s->debris?0:i,(SceneParticleLifetime){e->particle_lifetime,0,0});
    float t=clock.t;
    if(e->kind==kActionEffect_AuthoredDrips) {
      t/=s->activity;
      if(t>=1)continue;
    }
    const unsigned seed=clock.seed^(s->debris?i*0x85ebca6bu:0);
    const float birth=r->x0+(r->x1-r->x0)*HashUnit(seed),height=r->y1-r->y0;
    if(s->debris&&s->landing) {
      if(t>=.72f) {
        const float age=(t-.72f)/.28f;
        ArRenderColorF c=FieldColor(e->particle_style.color_end);
        c.a=fminf(.65f,e->tuning.intensity*.5f)*(1-age)*(1-age);
        if(s->landing==1) {
          if(!MaskPatch(w,e,clip,kActionMaskCloudBodyFirst+seed%kActionMaskCloudCount,
              birth,r->y1-1-3*age,2+7*age,1+3*age,c,project,context))return false;
        } else for(unsigned drop=0;drop<3;++drop) {
          const float dx=((int)drop-1)*8*age;
          const float y=r->y1-24*age*(1-age),size=.6f*(1-age)+.2f;
          c.r=1;c.g=.55f-.3f*age;c.b=.05f;c.a=fminf(1,e->tuning.intensity)*(1-age);
          if(!AppendSceneParticle(w,e,birth+dx,y,birth+dx-(int)drop+1,y+1-2*age,size,size*1.3f,c,project,context))return false;
        }
        continue;
      }
      t/=.72f;
    }
    if(e->kind==kActionEffect_AuthoredDrips) {
      /* Waiting time belongs to the source clock, not the viewport. A ripple
       * is opt-in: its authored bottom edge must be an exposed water surface. */
      if(s->ripple) {
        if(t>=.7f) {
          const float age=(t-.7f)/.3f;
          ArRenderColorF c=FieldColor(e->particle_style.color_end);
          c.a=fminf(1,e->tuning.intensity*.65f)*(1-age)*(1-age);
          ParticleLightTint(w,e,birth,r->y1,&c,true);
          if(!MaskPatch(w,e,clip,kActionMaskRipple,birth,r->y1,2+8*age,.35f+1.3f*age,c,project,context))return false;
          continue;
        }
        t/=.7f;
      }
    }
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(seed^27);
    float x=birth,y=r->y0+height*t*t,dy=1,reach=2;
    if(s->debris)reach=.8f+.5f*HashUnit(seed^37);
    if(e->kind==kActionEffect_AuthoredSpray) {
      x+=(HashUnit(clock.seed^71)-.5f)*height*t;
      y=r->y1-height*3*t*(1-t);dy=t-.5f;reach=1;
    }
    /* Keep the visible tip/edge when its center has left the viewport. The
     * projected particle is clipped by the renderer, not at its source point. */
    if(x+size<clip->x0||x-size>clip->x1||y+size*reach<clip->y0||y-size*reach>clip->y1)continue;
    ArRenderColorF c=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),t);
    c.a=fminf(1,e->tuning.intensity*.65f*sinf(t*3.1415927f));
    if(e->kind==kActionEffect_AuthoredDrips)ParticleLightTint(w,e,x,y,&c,true);
    if(!AppendSceneParticle(w,e,x,y,x,y-dy,size,size*reach,c,project,context))return false;
  }
  return true;
}

/* Small intermittent sources, not a bank oscillating as one slab. Each puff
 * grows and fades while rising from the authored bottom-centre contact. */
static bool RisingPlume(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!s->textured||s->plume!=1||!isfinite(s->activity)||s->activity<.05f||s->activity>1||
      s->point_count||s->rolling||s->volume||s->rainbow||s->sparkle||s->wake||s->canopy_response)return false;
  const float width=r->x1-r->x0,height=r->y1-r->y0;
  for(unsigned i=0;i<s->strands;++i) {
    const unsigned seed=e->particle_style.seed^(i+1)*0x9e3779b9u;
    const unsigned period=e->particle_lifetime+seed%(e->particle_lifetime/4+1);
    const unsigned tick=e->pulse_ticks+DeterministicHash_Mix32(seed)%period;
    const unsigned cycle=DeterministicHash_Mix32(seed^(tick/period)*0x85ebca6bu);
    const float t=(tick%period)/(period*s->activity);
    if(t>=1||HashUnit(cycle^17)<.12f)continue;
    const float rx=fminf(width*.3f,2+width*(.08f+.14f*t)),ry=rx*(.85f+.4f*t);
    const float drift=fminf(fabsf(s->drift),fmaxf(0,width*.5f-rx));
    const float x=(r->x0+r->x1)*.5f+drift*t*(.3f+.7f*HashUnit(cycle))*(s->drift<0?-1:1)+
        sinf(t*6+HashUnit(cycle^91)*6)*fminf(s->amplitude,rx*.5f)*t;
    const float y=r->y1-ry-fmaxf(0,height-2*ry)*t;
    ArRenderColorF c=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),t);
    const float fade=sinf(t*3.14159265f);c.a=fminf(.8f,e->tuning.intensity*.65f)*fade*fade;
    /* The broad rounded density mask keeps small puffs feathered. The thin
     * wisp sheet loses its narrow edge fade at this scale and reads as a box. */
    if(!MaskPatch(w,e,clip,kActionMaskCloudBodyFirst+cycle%kActionMaskCloudCount,x,y,rx,ry,c,project,context))return false;
  }
  return true;
}
static bool WetContour(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->point_count<2||s->point_count>kActionContourMaxPoints)return false;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const float phase=e->pulse_ticks*6.2831853f/e->particle_lifetime;
  for(unsigned i=1;i<s->point_count;++i) {
    const float ax=s->points[i-1].x,ay=s->points[i-1].y,bx=s->points[i].x,by=s->points[i].y;
    const ActionEffectLocalRect *r=&e->geometry.data.rect;
    if(!isfinite(ax)||!isfinite(ay)||!isfinite(bx)||!isfinite(by)||ax<r->x0||ax>r->x1||ay<r->y0||ay>r->y1||
        bx<r->x0||bx>r->x1||by<r->y0||by>r->y1)return false;
    const float length=hypotf(bx-ax,by-ay);if(length<.001f)return false;
    const float nx=-(by-ay)/length*.7f,ny=(bx-ax)/length*.7f;
    ArRenderColorF tint=FieldColor(e->tuning.color);tint.a=fminf(1,e->tuning.intensity*(.15f+.1f*sinf(phase+i)*sinf(phase+i)));
    ArRenderVertex2D vertices[4]={
      {.position={ax+nx,ay+ny},.color=tint},{.position={bx+nx,by+ny},.color=tint},
      {.position={ax-nx,ay-ny},.color=tint},{.position={bx-nx,by-ny},.color=tint}};
    int mapped[4]={-1,-1,-1,-1};const int indices[]={0,1,2,1,3,2};
    for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,indices+t,clip,project,context))return false;
  }
  return true;
}

/* A footprint-wide grade changes continuously across authored corners. The
 * walking edge itself remains exact; only loose powder uses this tangent. */
static float GroundSlope(const ActionEffectFieldStyle *s,float x) {
  const float a=fmaxf(s->points[0].x,x-8),b=fminf(s->points[s->point_count-1].x,x+8);
  return b>a?(ActionFieldMotion_GroundHeight(s,b)-ActionFieldMotion_GroundHeight(s,a))/(b-a):0;
}
static float GroundSlopeResponse(float slope) {
  return slope*slope/(.16f+slope*slope);
}
/* A crest sheds dust horizontally; settling grows with travel time instead
 * of snapping to the lee slope. Evaluate the upstream path analytically, so
 * reversing a preview or panning never resets a simulation. */
static float CarriedGroundHeight(const ActionEffectFieldStyle *s, float x, float floor) {
  if(s->carry<=0 || s->drift==0)return floor;
  float y=floor;
  for(unsigned i=0;i<s->point_count;++i) {
    const float distance=(x-s->points[i].x)*(s->drift>0?1:-1);
    if(distance<=0)continue;
    const float t=distance/s->carry;
    y=fminf(y,s->points[i].y+(8+2*s->amplitude)*t*t);
  }
  return y;
}
static float GroundPowderRadius(float size,float motion,float swell) {
  return (3.4f+4.2f*size)*(.65f+.5f*swell)*(.65f+.35f*motion);
}
static bool GroundGrains(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,const ActionFieldWakes *wakes,ActionEffectProjectPointFn project,void *context,bool lighting) {
  const ActionEffectFieldStyle *s=&e->field_style;
  const ActionEffectParticleStyle *style=&e->particle_style;
  const float left=s->points[0].x,right=s->points[s->point_count-1].x;
  ActionEffectInstance clock_source=*e;clock_source.pulse_generation=style->seed;
  const float strength=ActionFieldMotion_Wind(e);
  for(unsigned i=0;i<e->particle_count;++i) {
    const SceneParticleClock clock=SceneParticleClockAt(&clock_source,e->pulse_ticks,i,
        (SceneParticleLifetime){e->particle_lifetime,9,127});
    const float t=clock.t,birth=left+(right-left)*HashUnit(clock.seed^71);
    const float wind=s->drift*(.6f+.8f*HashUnit(clock.seed^11));
    float x=birth+wind*t;
    if(x<=left || x>=right)continue;
    const float size=style->size_min+(style->size_max-style->size_min)*HashUnit(clock.seed^27);
    const float lift=(8+2*s->amplitude)*(.35f+.65f*HashUnit(clock.seed^91));
    /* Fine grains are entrained in the gust; heavier grains make short hops.
     * The clamp only lifts grains out of rising terrain, never pulls them down. */
    const float weight=HashUnit(clock.seed^39),flight=weight<.65f?t*(1-.35f*t):4*t*(1-t);
    float y=ActionFieldMotion_GroundHeight(s,birth)-lift*flight+
        sinf(t*9+HashUnit(clock.seed^57)*6)*style->wander*sinf(t*3.1415927f);
    if(s->wake>0)WakeDisplacement(wakes,&x,&y,true);
    if(x<=left||x>=right)continue;
    y=fminf(y,ActionFieldMotion_GroundHeight(s,x)-size*2);
    if(x+size<clip->x0||x-size>clip->x1||y+size<clip->y0||y-size>clip->y1)continue;
    ArRenderColorF tint=MixColor(FieldColor(e->tuning.color),FieldColor(style->color_end),t);
    const float edge=fminf(1,fminf(x-left,right-x)/16);
    const float envelope=fmaxf(0,sinf(t*3.1415927f))*edge*strength;
    tint.a=fminf(.9f,e->tuning.intensity*.32f)*envelope;
    ReflectiveGrainTint(w,e,x,y,clock.seed,t,envelope*fminf(1,e->tuning.intensity),&tint,lighting,true);
    if(!AppendSceneParticle(w,e,x,y,x-wind*.003f,y+lift*.003f,size*.65f,size,tint,project,context))return false;
  }
  /* Dense, rounded clumps carry the mass of kicked-up powder; fine grains
   * catch the sun around them. Motion controls their spread and lift without
   * making an ordinary walking step almost transparent. The last cohort ages
   * out after movement stops, rather than spawning around a stationary body. */
  for(unsigned body=0;body<kActionFieldWakeCount;++body) {
    const ActionFieldWake *wake=&wakes->bodies[body];if(wake->strength<=0)continue;
    const float source_slope=GroundSlope(s,wake->x),source_grade=GroundSlopeResponse(source_slope);
    const unsigned phase=DeterministicHash_Mix32(wake->seed^0x4d31u)%48;
    for(unsigned i=0;i<kActionGroundWakeGrains;++i) {
      const uint32_t seed=DeterministicHash_Mix32(wake->seed^i*0x9e3779b9u);
      const bool puff=i<kActionGroundWakePuffs;
      /* Overlap staggered births instead of allowing several dense clumps
       * to appear together. Each birth fades in over roughly ten ticks. */
      const unsigned offset=puff?phase+i*48/kActionGroundWakePuffs+seed%5:seed;
      const float age=((e->pulse_ticks-(unsigned)wake->age+offset)%48)+(float)wake->age;
      const float t=age/48;if(t>=1)continue;
      const float motion=sqrtf(wake->strength);
      const float side=wake->dx<0?-1.f:wake->dx>0?1.f:HashUnit(seed^3)<.5f?-1.f:1.f;
      /* A short kick follows the foot, then gravity carries loose snow down
       * the grade. Positive Y is down. Flat paths retain the original motion. */
      float x=wake->x-side*(7+34*t)*motion*(1-.4f*source_grade)+
          (HashUnit(seed^13)-.5f)*24*t*motion+
          source_slope/(1+source_slope*source_slope)*56*t*t*motion;
      if(x<=left||x>=right)continue;
      const float size=style->size_min+(style->size_max-style->size_min)*HashUnit(seed^27);
      const float local_slope=GroundSlope(s,x);
      const float grade=fmaxf(source_grade,GroundSlopeResponse(local_slope));
      const float lift=(puff?7+11*HashUnit(seed^71):12+18*HashUnit(seed^71))*motion;
      const float radius=puff?GroundPowderRadius(size,motion,sinf(t*3.1415927f)):size;
      const float ry=puff?radius*(.75f+.2f*HashUnit(seed^11)):size;
      const float floor=puff?fminf(ActionFieldMotion_GroundHeight(s,x-radius),ActionFieldMotion_GroundHeight(s,x+radius)):ActionFieldMotion_GroundHeight(s,x);
      const float old_y=fminf(wake->y-lift*4*t*(1-t),floor)-(puff?ry:size*2);
      const float slope=local_slope*grade,normal=1/sqrtf(1+slope*slope);
      const float hop=lift*(1-.68f*grade)*4*t*(1-t);
      const float height=(puff?ry:size*2)+hop;
      const float surface=ActionFieldMotion_GroundHeight(s,x);
      float y=old_y+(surface-height*normal-old_y)*grade;
      x+=slope*normal*height*grade;
      if(x<=left||x>=right)continue;
      const float extent_x=puff?(radius+fabsf(slope)*ry)*normal:radius;
      const float extent_y=puff?(fabsf(slope)*radius+ry)*normal:ry;
      if(x+extent_x<clip->x0||x-extent_x>clip->x1||y+extent_y<clip->y0||y-extent_y>clip->y1)continue;
      ArRenderColorF tint=MixColor(FieldColor(e->tuning.color),FieldColor(style->color_end),t);
      const float enter=fminf(1,t*5),settle=fminf(1,(1-t)/.4f);
      const float envelope=enter*enter*(3-2*enter)*settle*settle*(3-2*settle)*fminf(1,wake->strength*4)*
          fminf(1,fminf(x-left,right-x)/12);
      tint.a=fminf(1,e->tuning.intensity)*envelope;
      /* Fine ice gets one slow highlight during its short flight, rather
       * than repeating the longer ambient-grain sparkle cycle several times. */
      ReflectiveGrainTint(w,e,x,y,seed,t*.25f,envelope,&tint,lighting,!puff);
      if(puff) {
        const unsigned variant=seed%kActionMaskCloudCount;
        ArRenderColorF body=tint;body.r*=.88f;body.g*=.94f;
        if(!SlopedPowderPatch(w,e,clip,kActionMaskCloudBodyFirst+variant,x,y,radius,ry,slope,body,project,context))return false;
        ArRenderColorF crest=MixColor(tint,(ArRenderColorF){1,1,1,tint.a},.65f);crest.a*=.75f;
        if(!SlopedPowderPatch(w,e,clip,kActionMaskCloudLightFirst+variant,x,y,radius,ry,slope,crest,project,context))return false;
      } else if(!AppendSceneParticle(w,e,x,y,x+side,y+1,size*.85f,size*1.15f,tint,project,context))return false;
    }
  }
  return true;
}
typedef struct GroundPressure {float x,radius,strength;} GroundPressure;
/* A quiet material surface, independent of the gust clock. Current feet also
 * compress while standing still; a captured wake can briefly retain pressure
 * after contact. Select at most one player and one enemy, just like powder. */
static bool GroundCover(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectFieldStyle *path,const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project,void *context,bool lighting) {
  GroundPressure pressure[kActionFieldWakeCount]={0};
  const bool surface=e->render_layer==kActionEffectRenderLayer_Bg1Mist;
  const float left=path->points[0].x,right=path->points[path->point_count-1].x;
  if(w->frame&&w->frame->actor_count<=kActionEffectActorMax) {
    for(unsigned i=0;i<w->frame->actor_count;++i) {
      const ActionEffectActor *a=&w->frame->actors[i];
      if(!a->visible||!a->body_radius)continue;
      float x=a->x-e->world_x;
      float strength=SceneSoftFalloff((a->feet_y-e->world_y-ActionFieldMotion_GroundHeight(path,x))/(path->ground_depth+2));
      if(x<left||x>right)strength=0;
      if(strength<=0&&a->wake.age<kActionActorWakeTicks) {
        x=a->wake.x-e->world_x;
        const float age=a->wake.age/(float)kActionActorWakeTicks;
        strength=.4f*path->wake*a->wake.strength*(1-age)*(1-age)*
            SceneSoftFalloff((a->wake.y-e->world_y-ActionFieldMotion_GroundHeight(path,x))/(path->ground_depth+2));
      }
      if(x<left||x>right||x<clip->x0-16||x>clip->x1+16)continue;
      const unsigned slot=a->player?0:1;
      if(strength>pressure[slot].strength)
        pressure[slot]=(GroundPressure){x,fmaxf(6,fminf(12,a->body_radius)),strength};
    }
  }
  float columns[kActionGroundCoverColumns];unsigned count=path->point_count;
  for(unsigned i=0;i<count;++i)columns[i]=path->points[i].x;
  /* Insert contact samples without moving any terrain corners. The stable
   * base profile is interpolated from the original columns below, so walking
   * cannot resample noise and make the entire bank shimmer. */
  for(unsigned i=0;i<kActionFieldWakeCount;++i)if(pressure[i].strength>0) {
    for(int offset=-1;offset<=1;++offset) {
      const float x=pressure[i].x+offset*pressure[i].radius;
      if(x<=left||x>=right)continue;
      unsigned at=0;while(at<count&&columns[at]<x)++at;
      if((at<count&&columns[at]-x<.01f)||(at&&x-columns[at-1]<.01f))continue;
      for(unsigned j=count;j>at;--j)columns[j]=columns[j-1];
      columns[at]=x;++count;
    }
  }
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  ArRenderVertex2D vertices[kActionGroundCoverColumns*3];int mapped[kActionGroundCoverColumns*3];
  for(unsigned col=0;col<count;++col) {
    const float x=columns[col],floor=ActionFieldMotion_GroundHeight(path,x);
    unsigned segment=1;while(segment+1<path->point_count&&x>path->points[segment].x)++segment;
    const float t=(x-path->points[segment-1].x)/(path->points[segment].x-path->points[segment-1].x);
    const float a=HashUnit(e->particle_style.seed^(segment-1)*71),b=HashUnit(e->particle_style.seed^segment*71);
    const float edge=fminf(1,fminf(x-left,right-x)/12);
    const float settled_depth=path->ground_depth*(.82f+.18f*(a+(b-a)*t))*edge;
    float compression=0;
    for(unsigned i=0;i<kActionFieldWakeCount;++i)if(pressure[i].strength>0)
      compression=fmaxf(compression,pressure[i].strength*SceneSoftFalloff((x-pressure[i].x)/pressure[i].radius));
    const float depth=settled_depth*(1-.35f*fminf(1,compression));
    ArRenderColorF crest=FieldColor(e->particle_style.color_end),body=FieldColor(e->tuning.color);
    ReflectiveGrainTint(w,e,x,floor-depth,0,0,1,&crest,lighting,false);
    ReflectiveGrainTint(w,e,x,floor,0,0,1,&body,lighting,false);
    const float alpha=fminf(.98f,e->tuning.intensity*.75f)*edge;
    /* Surface width and snow thickness are independent. The rear edge stays
     * on the broad painted top, while only the front responds to foot contact.
     * A narrow rear feather retains the transition into the original artwork. */
    const float back=floor-path->ground_width-settled_depth;
    const float feather=fminf(.8f,path->ground_width*.25f);
    for(unsigned row=0;row<3;++row) {
      const unsigned n=col*3+row;
      const float y=surface?(row==0?back:row==1?back+feather:floor-depth):
          row==0?floor-depth:row==1?floor:floor+1.5f;
      const ArRenderColorF tint=surface?(row==2?crest:MixColor(body,crest,.45f)):row==0?crest:body;
      vertices[n]=(ArRenderVertex2D){.position={x,y},.color=tint};
      vertices[n].color.a=surface?(row==0?0:alpha*(row==1?.58f:.78f)):row==2?0:alpha;mapped[n]=-1;
    }
  }
  for(unsigned col=0;col+1<count;++col)for(unsigned row=0;row<2;++row) {
    const int a=col*3+row,b=a+3,triangles[]={a,b,a+1,b,b+1,a+1};
    for(unsigned t=0;t<6;t+=3)
      if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
  }
  return true;
}
/* Overlapping gusts build on windward slopes, carry over crests and dissipate.
 * One bounded mesh is shared by CPU/WASM and GPU projection. */
static bool GroundCloud(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project, void *context,bool lighting) {
  ActionEffectFieldStyle path = e->field_style;
  const ActionEffectFieldStyle *s = &path;
  if (s->point_count < 2 || s->point_count > kActionContourMaxPoints) return false;
  for(unsigned i=0;i<s->point_count;++i)
    if(!isfinite(s->points[i].x)||!isfinite(s->points[i].y)||
       (i&&s->points[i].x<=s->points[i-1].x))return false;
  /* A simple two-point slope still needs interior samples for soft lobes.
   * Preserve every authored corner while subdividing the widest intervals. */
  while(path.point_count<9) {
    unsigned split=1;
    for(unsigned i=2;i<path.point_count;++i)
      if(path.points[i].x-path.points[i-1].x>path.points[split].x-path.points[split-1].x)split=i;
    for(unsigned i=path.point_count;i>split;--i)path.points[i]=path.points[i-1];
    path.points[split].x=(path.points[split-1].x+path.points[split+1].x)*.5f;
    path.points[split].y=(path.points[split-1].y+path.points[split+1].y)*.5f;
    ++path.point_count;
  }
  const float left = s->points[0].x, right = s->points[s->point_count-1].x;
  if (!(right > left)) return false;
  const float wind_density=ActionFieldMotion_Wind(e);
  const ActionFieldWakes wakes=ActionFieldMotion_Gather(w->frame,e,clip,true);
  if(s->ground_depth>0)
    return GroundCover(w,e,s,clip,project,context,lighting)&&
        (e->render_layer==kActionEffectRenderLayer_Bg1Mist||GroundGrains(w,e,clip,&wakes,project,context,lighting));
  struct { float centre, radius, weight; } gusts[24];
  for (unsigned i=0; i<s->strands; ++i) {
    const SceneParticleClock clock = SceneParticleClockAt(e,e->pulse_ticks,i,
        (SceneParticleLifetime){e->particle_lifetime,9,127});
    const float t=clock.t, swell=sinf(t*3.1415927f);
    gusts[i].centre=left+(right-left)*(i+.2f+.6f*HashUnit(clock.seed))/s->strands+s->drift*(t-.5f);
    gusts[i].radius=(right-left)*(.12f+.08f*HashUnit(clock.seed^71))*(.65f+.7f*t);
    gusts[i].weight=swell*swell*wind_density;
  }
  ActionEffectInstance mesh=*e; mesh.flags|=kActionEffectFlag_ClippedMesh;
  ArRenderVertex2D vertices[kActionContourMaxPoints*4]; int mapped[kActionContourMaxPoints*4];
  const ArRenderColorF tint=FieldColor(e->tuning.color);
  const float alpha[]={0,.55f,1,.35f};
  for (unsigned col=0; col<s->point_count; ++col) {
    const float x=s->points[col].x, floor=s->points[col].y;
    if (!isfinite(x)||!isfinite(floor)||(col && x<=s->points[col-1].x)) return false;
    float density=0;
    for (unsigned i=0; i<s->strands; ++i)
      density+=gusts[i].weight*SceneSoftFalloff((x-gusts[i].centre)/gusts[i].radius);
    float stirred_x=x,stirred_y=floor;
    const float stirred=s->wake>0?WakeDisplacement(&wakes,&stirred_x,&stirred_y,true):0;
    density+=stirred*.25f;
    const unsigned prev=col?col-1:col, next=col+1<s->point_count?col+1:col;
    const float slope=(s->points[next].y-s->points[prev].y)/(s->points[next].x-s->points[prev].x);
    const float bank=1+fminf(.5f,fmaxf(0,-slope*(s->drift>0?1:s->drift<0?-1:0)));
    const float edge=fminf(1,fminf(x-left,right-x)/fmaxf(8,(right-left)*.08f));
    const float base=CarriedGroundHeight(s,x,floor);
    const float height=(8+2*s->amplitude)*bank*(.25f+.75f*fminf(1,density))+stirred*5;
    const float airborne=fminf(1,(floor-base)/8);
    for (unsigned row=0; row<4; ++row) {
      const unsigned n=col*4+row;
      vertices[n]=(ArRenderVertex2D){.position={x,base-height*(1-row/3.f)},.color=tint};
      vertices[n].color.a=fminf(.85f,e->tuning.intensity*(.12f+.12f*s->softness)*density)*alpha[row]*edge;
      if(row==3)vertices[n].color.a*=1-airborne;
      mapped[n]=-1;
    }
  }
  for (unsigned col=0; col+1<s->point_count; ++col) for (unsigned row=0; row<3; ++row) {
    const int a=col*4+row,b=a+4,indices[]={a,b,a+1,b,b+1,a+1};
    for (unsigned t=0;t<6;t+=3)
      if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,indices+t,clip,project,context))return false;
  }
  return GroundGrains(w,e,clip,&wakes,project,context,lighting);
}
/* Three clipped quads per plant at most: one leaning stem/leaf strip, one
 * species silhouette and an optional open-flower centre. Roots, depth and
 * species are seeded in world space; viewport changes never reseed a bed. */
static bool FlowerPatch(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectFloorField *floor,const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project,void *context,bool lighting) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!floor||!floor->count)return true;
  if(s->strands<1||s->strands>kActionFlowerMaxPlants||s->flower_style>3||!e->particle_lifetime)return false;
  const ActionFlowerMotion motion=ActionWoodland_FlowerMotion(w->frame,e);
  for(unsigned plant=0;plant<s->strands;++plant) {
    ActionFlowerPose pose;if(!ActionWoodland_Flower(e,floor,plant,&motion,&pose))continue;
    const uint32_t seed=pose.seed;const unsigned species=pose.species;
    const float x=pose.x,root=pose.root,back=pose.back,height=pose.height;
    ArRenderColorF stem=FieldColor(0x718e40),petal=FieldColor(pose.color);
    stem.a=petal.a=fminf(1,e->tuning.intensity);
    const float response=lighting&&!w->atmosphere_unlit?s->canopy_response:0;
    const ArRenderColorF incoming=response>0?FieldIncidentLight(w,e,x,root-height):(ArRenderColorF){0};
    ReflectedPlantTint(incoming,response,&stem);ReflectedPlantTint(incoming,response,&petal);
    const float rx=3.5f-back*.08f;
    /* These stems are only a dozen native pixels tall. One sheared strip
     * preserves the fixed roots and moving tips while keeping dense beds
     * within the existing alpha budget. */
    const ArRenderPointF tip={pose.tip_x,pose.tip_y};
    const ArRenderPointF stem_points[4]={{x-rx,root},{x+rx,root},
        {tip.x+rx,tip.y},{tip.x-rx,tip.y}};
    const ArRenderPointF stem_uv[4]={ActionEffectMasks_Uv(kActionMaskFlowerStem,0,1),
        ActionEffectMasks_Uv(kActionMaskFlowerStem,1,1),
        ActionEffectMasks_Uv(kActionMaskFlowerStem,1,0),ActionEffectMasks_Uv(kActionMaskFlowerStem,0,0)};
    if(!MaskQuad(w,e,clip,stem_points,stem_uv,stem,NULL,project,context))return false;
    const float size=(2.6f+.8f*HashUnit(seed^43))*(1-back*.025f);
    const float sx=species==1?size*.78f:species==2?size*.65f:size;
    const float sy=species==1?size*1.18f:species==2?size*.9f:size;
    if(!MaskPatch(w,e,clip,kActionMaskFlowerFirst+species,tip.x,tip.y,sx,sy,petal,project,context))return false;
    if(!species) {
      ArRenderColorF center=FieldColor(0xd9b565);center.a=petal.a;
      ReflectedPlantTint(incoming,response,&center);
      const float size=.55f;
      const ArRenderPointF p[4]={{tip.x-size,tip.y-size},{tip.x+size,tip.y-size},
        {tip.x+size,tip.y+size},{tip.x-size,tip.y+size}},uv[4]={{0}};
      if(!MaskQuad(w,e,clip,p,uv,center,NULL,project,context))return false;
    }
  }
  const ActionWoodlandEvent *event=s->petals?ActionWoodland_Event(w->frame,e,0):NULL;
  if(event)for(unsigned i=0;i<kActionFlowerPetals;++i) {
    const unsigned age=(uint16_t)(e->pulse_ticks-event->born);
    if(age<i*8||age>=kActionWoodlandPetalTicks+i*8)continue;
    const float ticks=age-i*8,t=fminf(1,ticks/82),ease=t*t*(3-2*t);
    const float arc=sinf(t*3.1415927f),phase=HashUnit(event->seed^i*71u)*6.2831853f;
    const float x=event->x-e->world_x+(event->end_x-event->x)*ease+
        arc*sinf(t*9+phase)*2;
    const float y=event->y-e->world_y+(event->ground_y-event->y)*ease-arc*(5+i*2);
    if(x+2<clip->x0||x-2>clip->x1||y+2<clip->y0||y-2>clip->y1)continue;
    ArRenderColorF color=FieldColor(event->color);
    const float fade=fminf(1,(kActionWoodlandPetalTicks-ticks)/26);
    color.a=fminf(1,e->tuning.intensity)*fminf(1,ticks/3)*fade*fade*(3-2*fade);
    PlantLightTint(w,e,x,y,&color,lighting);
    const float turn=phase+ticks*.065f,width=.65f*(.8f+.2f*cosf(turn));
    if(!AppendSceneParticle(w,e,x,y,x-cosf(turn),y-sinf(turn),width,1.15f,color,project,context))return false;
  }
  return true;
}

/* A soft stencil of hanging vines and broad leaves, or bare woody fibers.
 * The bare variant retains the fixed roots and only bends the slender tips.
 * Positions and wind phase depend only on the saved definition and clock,
 * never on the visible window.
 * This is ordinary alpha geometry behind the playfield, not a light receiver
 * or an additional full-screen shadow pass. Six segments/leaves per stem keep
 * the worst-case clipped cost exactly bounded by the recipe validator. */
static bool FoliageShadow(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->strands<1||s->strands>8||!isfinite(s->angle)||fabsf(s->angle)>180||
      !isfinite(s->softness)||s->softness<0||s->softness>1)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,height=r->y1-r->y0;
  const float angle=s->angle*.01745329252f,cs=cosf(angle),sn=sinf(angle);
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  const float spacing=width/s->strands;
  const float leaf_size=fminf(spacing*.32f,height*.12f);
  const float max_bow=fminf(spacing*.10f,height*.08f);
  const float inset=fminf(width*.48f,leaf_size+max_bow+s->amplitude+2);
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  for(unsigned stem=0;stem<s->strands;++stem) {
    const uint32_t seed=e->particle_style.seed^(stem+1)*0x9e3779b9u;
    const float x=r->x0+inset+(width-2*inset)*(stem+.25f+.5f*HashUnit(seed))/s->strands;
    const float length=height*(.56f+.37f*HashUnit(seed^17));
    const float bow=max_bow*(HashUnit(seed^43)*2-1);
    const ActionEffectInstance *opening=s->bare?ActionTrunk_Opening(w->frame,e->world_x,e->world_y):NULL;
    const float gust=ActionTrunk_Gust(opening,opening?hypotf(e->world_x-opening->world_x,e->world_y-opening->world_y)+20:0);
    const float push=opening?(opening->world_x<512?1.f:-1.f)*w->frame->trunk.gust*gust:0;
    const float sway=(sinf(phase+HashUnit(seed^91)*6.2831853f)+push*1.7f)*s->amplitude;
    ArRenderColorF tint=FieldColor(e->tuning.color);
    const float opacity=fminf(1,e->tuning.intensity)*(.65f+.35f*HashUnit(seed^57));
    ArRenderPointF previous={x,r->y0};
    for(unsigned segment=0;segment<6;++segment) {
      const float t=(segment+1)/6.f;
      const ArRenderPointF tip={x+sinf(t*3.14159265f)*bow+sway*t*t,r->y0+length*t};
      const float half=s->bare?fminf(1.1f,.4f+height*.012f)*(1-.9f*t):
          fminf(1.8f,height*.006f)*(1-.65f*t);
      const float previous_half=s->bare?fminf(1.1f,.4f+height*.012f)*(1-.9f*segment/6.f):half;
      ArRenderVertex2D vine[4]={
        {.position={previous.x-previous_half,previous.y},.color=tint},
        {.position={previous.x+previous_half,previous.y},.color=tint},
        {.position={tip.x+half,tip.y},.color=tint},
        {.position={tip.x-half,tip.y},.color=tint}};
      for(unsigned j=0;j<4;++j) {
        const ArRenderPointF p=vine[j].position;
        vine[j].position=(ArRenderPointF){cs*p.x+sn*p.y,-sn*p.x+cs*p.y};
        vine[j].color.a=opacity*(s->bare?1.f:.55f);
        if(s->bare&&(j==0||j==3)) {
          vine[j].color.r*=.6f;vine[j].color.g*=.6f;vine[j].color.b*=.6f;
        }
      }
      int mapped[4]={-1,-1,-1,-1};const int triangles[]={0,1,2,0,2,3};
      for(unsigned j=0;j<6;j+=3)
        if(!AppendSceneClippedTriangle(w,&mesh,vine,mapped,triangles+j,clip,project,context))return false;

      if(s->bare) {previous=tip;continue;}

      const uint32_t leaf_seed=seed^(segment+1)*71u;
      const float side=(segment&1)?1.f:-1.f;
      const float reach=leaf_size*(.65f+.35f*HashUnit(leaf_seed));
      const float droop=reach*(.12f+.30f*HashUnit(leaf_seed^5));
      /* Pointed leaves with a broad shoulder; feather the outline while the
       * central vein remains legible on the flat olive background. */
      const float shape[7][2]={{.43f,0},{0,0},{.28f,-.30f},{.68f,-.22f},{1,0},{.62f,.27f},{.24f,.25f}};
      ArRenderVertex2D leaf[7];int leaf_mapped[7];
      for(unsigned j=0;j<7;++j) {
        const float u=shape[j][0],v=shape[j][1];
        const float px=tip.x+side*u*reach,py=tip.y+u*droop+v*reach;
        leaf[j]=(ArRenderVertex2D){.position={cs*px+sn*py,-sn*px+cs*py},.color=tint};
        leaf[j].color.a=opacity*(1-.4f*t)*(j?(1-s->softness)*.65f:1);
        leaf_mapped[j]=-1;
      }
      for(int j=1;j<=6;++j) {
        const int triangle[]={0,j,j==6?1:j+1};
        if(!AppendSceneClippedTriangle(w,&mesh,leaf,leaf_mapped,triangle,clip,project,context))return false;
      }
      previous=tip;
    }
  }
  return true;
}

/* Atlas coordinates survive clipping and resident projection. One mask atlas
 * batches with ordinary white geometry; only these triangles sample detail. */
static bool MaskQuad(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,const ArRenderPointF p[4],const ArRenderPointF uv[4],
    ArRenderColorF color,const float opacity[4],ActionEffectProjectPointFn project,void *context) {
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  ArRenderVertex2D vertices[4];
  for(unsigned i=0;i<4;++i) {
    vertices[i]=(ArRenderVertex2D){.position=p[i],.color=color,.tex_coord=uv[i]};
    if(opacity)vertices[i].color.a*=opacity[i];
    if(e->kind==kActionEffect_AuthoredCloud&&e->projection_plane==kActionEffectProjectionPlane_Bg2&&w->frame) {
      if(!w->cloud_light_ready) {
        w->cloud_light=ActionCloudLight_PrepareWithSun(w->frame,0,0,0,0,w->sun_directions);
        w->cloud_light_ready=true;
      }
      for(unsigned j=0;j<(unsigned)w->cloud_light.meta[0];++j) {
        const DioramaCloudLightRegion *r=&w->cloud_light.regions[j];
        const CloudLightSample light=DioramaCloudLight_Sample(r,p[i].x+e->world_x,p[i].y+e->world_y);
        const float upper=light.edge*.25f+light.halo*r->core[1]+light.streak*r->core[2];
        const float echo=light.echo*r->core[3];
        const float weight=1-expf(-(upper+echo)*r->color[3]);
        ArRenderColorF *c=&vertices[i].color;
        if(e->field_style.volume>0) {
          /* Incident sunlight tints the baked scattering. Replacing both
           * colors with the sun color erases every shaded underside. */
          c->r*=1+(r->color[0]*1.1f-1)*weight;
          c->g*=1+(r->color[1]*1.1f-1)*weight;
          c->b*=1+(r->color[2]*1.1f-1)*weight;
          c->r=fminf(1,c->r);c->g=fminf(1,c->g);c->b=fminf(1,c->b);
        } else {
          c->r+=(r->color[0]-c->r)*weight;
          c->g+=(r->color[1]-c->g)*weight;
          c->b+=(r->color[2]-c->b)*weight;
        }
        /* The lit opening also transmits through the added cloud overlay;
         * otherwise that later grey draw erases the sky's warm highlight. */
        c->a*=1-.75f*(1-expf(-upper*r->color[3]));
      }
    }
  }
  return AppendSceneClippedQuad(w,&mesh,vertices,clip,project,context);
}
static bool MaskPatch(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,unsigned cell,float x,float y,float rx,float ry,
    ArRenderColorF color,ActionEffectProjectPointFn project,void *context) {
  const ArRenderPointF p[4]={{x-rx,y-ry},{x+rx,y-ry},{x+rx,y+ry},{x-rx,y+ry}};
  const ArRenderPointF uv[4]={ActionEffectMasks_Uv(cell,0,0),ActionEffectMasks_Uv(cell,1,0),
      ActionEffectMasks_Uv(cell,1,1),ActionEffectMasks_Uv(cell,0,1)};
  return MaskQuad(w,e,clip,p,uv,color,NULL,project,context);
}
static bool SlopedPowderPatch(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,unsigned cell,float x,float y,float rx,float ry,float slope,
    ArRenderColorF color,ActionEffectProjectPointFn project,void *context) {
  if(slope==0)return MaskPatch(w,e,clip,cell,x,y,rx,ry,color,project,context);
  const float n=1/sqrtf(1+slope*slope),tx=rx*n,ty=rx*slope*n,nx=-ry*slope*n,ny=ry*n;
  const ArRenderPointF p[4]={{x-tx-nx,y-ty-ny},{x+tx-nx,y+ty-ny},
      {x+tx+nx,y+ty+ny},{x-tx+nx,y-ty+ny}};
  const ArRenderPointF uv[4]={ActionEffectMasks_Uv(cell,0,0),ActionEffectMasks_Uv(cell,1,0),
      ActionEffectMasks_Uv(cell,1,1),ActionEffectMasks_Uv(cell,0,1)};
  return MaskQuad(w,e,clip,p,uv,color,NULL,project,context);
}
static bool LeafParticle(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,const ActionEffectLocalRect *clip,
    float x,float y,float dx,float dy,float width,float reach,ArRenderColorF color,ActionEffectProjectPointFn project,void *context) {
  const ArRenderPointF p[4]={{x-dx*reach+dy*width,y-dy*reach-dx*width},
      {x+dx*reach+dy*width,y+dy*reach-dx*width},{x+dx*reach-dy*width,y+dy*reach+dx*width},
      {x-dx*reach-dy*width,y-dy*reach+dx*width}};
  const ArRenderPointF uv[4]={ActionEffectMasks_Uv(kActionMaskLeaf,0,0),ActionEffectMasks_Uv(kActionMaskLeaf,1,0),
      ActionEffectMasks_Uv(kActionMaskLeaf,1,1),ActionEffectMasks_Uv(kActionMaskLeaf,0,1)};
  return MaskQuad(w,e,clip,p,uv,color,NULL,project,context);
}

typedef struct GlowPoint { float x,y,size,glow; ArRenderColorF color; } GlowPoint;
typedef struct GlowOrbit {
  uint32_t seed;
  float phase,x,y,wander,travel_x,travel_y;
  bool fixed;
} GlowOrbit;
static GlowOrbit GlowOrbitFor(const ActionEffectInstance *e,unsigned index) {
  const ActionEffectParticleStyle *s=&e->particle_style;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const uint32_t seed=DeterministicHash_Mix32(s->seed^(index+1)*0x9e3779b9u);
  const bool fixed=e->field_style.glow_style==kActionGlow_Moss;
  const float wander=fixed?0:s->wander,travel_x=fixed?0:s->travel_x,travel_y=fixed?0:s->travel_y;
  const float inset=fminf(.45f,(wander+fmaxf(fabsf(travel_x),fabsf(travel_y))*.5f)/fmaxf(4,fminf(r->x1-r->x0,r->y1-r->y0)));
  const float bx=(HashUnit(seed^17)-.5f)*s->spread*(1-2*inset);
  const float by=(HashUnit(seed^31)-.5f)*(1-2*inset);
  return (GlowOrbit){.seed=seed,.phase=HashUnit(seed)*6.2831853f,
      .x=(r->x0+r->x1)*.5f+(r->x1-r->x0)*bx,.y=(r->y0+r->y1)*.5f+(r->y1-r->y0)*by,
      .wander=wander,.travel_x=travel_x,.travel_y=travel_y,.fixed=fixed};
}
static ArRenderPointF GlowPositionAt(const GlowOrbit *orbit,float clock) {
  if(orbit->fixed)return (ArRenderPointF){orbit->x,orbit->y};
  const float motion=clock*6.2831853f;
  return (ArRenderPointF){
      orbit->x+sinf(motion*.71f+orbit->phase)*orbit->wander+sinf(motion*.23f+orbit->phase)*orbit->travel_x*.5f,
      orbit->y+cosf(motion*.53f+orbit->phase)*orbit->wander*.55f+sinf(motion*.19f+orbit->phase)*orbit->travel_y*.5f};
}
static GlowPoint GlowPointAtTime(const ActionEffectInstance *e,const GlowOrbit *orbit,float clock) {
  const ActionEffectParticleStyle *s=&e->particle_style;
  const uint32_t seed=orbit->seed;
  const float phase=orbit->phase;
  /* Two incommensurate pulse rates prevent a cluster blinking in unison.
   * Stable roots and continuous clocks survive camera movement and scrubbing. */
  float pulse;
  if(e->field_style.glow_style!=kActionGlow_Insects) {
    pulse=.55f+.30f*sinf(clock*2.7f+phase)+.15f*sinf(clock*4.1f+phase*2);
  } else {
    pulse=fmaxf(0,sinf(clock*(8+4*HashUnit(seed^71))+phase));pulse*=pulse;
    pulse*=.45f+.55f*(.5f+.5f*sinf(clock*3.7f+phase*2));
  }
  if(e->field_style.glow_style==kActionGlow_Insects&&e->field_style.chorus>0) {
    /* Seeded invitations, short staggered answers, then independent pulses.
     * The envelope is zero at event boundaries, including reverse scrubbing. */
    const float event_clock=clock*.9f+HashUnit(s->seed^317);
    const int event=(int)floorf(event_clock);
    const uint32_t invitation=DeterministicHash_Mix32(s->seed^(uint32_t)event*0x85ebca6bu);
    const float age=event_clock-event;
    if(HashUnit(invitation)>.22f) {
      const float delay=.10f+.23f*HashUnit(seed^invitation);
      const float answer_age=(age-delay)/.15f;
      const float answer=answer_age>0&&answer_age<1?sinf(answer_age*3.1415927f):0;
      const float window=sinf(age*3.1415927f)*sinf(age*3.1415927f);
      pulse=fminf(1,pulse*(1-.65f*e->field_style.chorus*window)+e->field_style.chorus*answer*answer);
    }
  }
  const float glow=.10f+.90f*pulse;
  const ArRenderPointF position=GlowPositionAt(orbit,clock);
  return (GlowPoint){
    .x=position.x,.y=position.y,
    .size=s->size_min+(s->size_max-s->size_min)*HashUnit(seed^43),.glow=glow,
    .color=MixColor(FieldColor(s->color_end),FieldColor(e->tuning.color),.45f+.55f*glow)};
}
static GlowPoint GlowPointAt(const ActionEffectInstance *e,unsigned index) {
  const GlowOrbit orbit=GlowOrbitFor(e,index);
  return GlowPointAtTime(e,&orbit,e->pulse_ticks/(float)e->particle_lifetime);
}
static bool Fireflies(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,bool lighting,bool particles,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(e->particle_count>32||s->glow_style>=kActionGlow_Count||
      !isfinite(s->chorus)||s->chorus<0||s->chorus>1||!isfinite(s->trail)||s->trail<0||s->trail>1)return false;
  for(unsigned i=0;i<e->particle_count;++i) {
    const GlowOrbit orbit=GlowOrbitFor(e,i);
    const float clock=e->pulse_ticks/(float)e->particle_lifetime;
    const GlowPoint p=GlowPointAtTime(e,&orbit,clock);
    ArRenderColorF color=p.color;
    if(lighting) {
      const float radius=8+p.size*12;
      color.a=fminf(1,e->tuning.intensity*p.glow*.13f);
      if(!MaskPatch(w,e,clip,kActionMaskGlow,p.x,p.y,radius,radius,color,project,context))return false;
    }
    if(particles) {
      if(s->glow_style==kActionGlow_Wisps&&s->trail>0) {
        ArRenderPointF path[4]={{p.x,p.y}};
        /* Historical tail samples only need position; their light/color is
         * the current head's. Reuse the orbit without recalculating pulses. */
        for(unsigned j=1;j<4;++j)
          path[j]=GlowPositionAt(&orbit,clock-.18f*j*s->trail);
        for(unsigned j=3;j>0;--j) {
          const float dx=path[j-1].x-path[j].x,dy=path[j-1].y-path[j].y,length=hypotf(dx,dy);
          const float tx=length>.001f?dx/length:1,ty=length>.001f?dy/length:0;
          const float reach=length*.65f+p.size,width=p.size*(1.7f-.3f*j);
          const float x=(path[j-1].x+path[j].x)*.5f,y=(path[j-1].y+path[j].y)*.5f;
          const ArRenderPointF corners[4]={{x-tx*reach-ty*width,y-ty*reach+tx*width},
              {x+tx*reach-ty*width,y+ty*reach+tx*width},{x+tx*reach+ty*width,y+ty*reach-tx*width},
              {x-tx*reach+ty*width,y-ty*reach-tx*width}};
          const unsigned cell=kActionMaskFogFirst+(e->particle_style.seed+i+j)%kActionMaskFogCount;
          const ArRenderPointF uv[4]={ActionEffectMasks_Uv(cell,0,0),ActionEffectMasks_Uv(cell,1,0),
              ActionEffectMasks_Uv(cell,1,1),ActionEffectMasks_Uv(cell,0,1)};
          ArRenderColorF c=p.color;c.a=fminf(1,e->tuning.intensity*(.38f-.07f*j)*s->trail);
          if(!MaskQuad(w,e,clip,corners,uv,c,NULL,project,context))return false;
        }
      }
      if(s->glow_style==kActionGlow_Moss) {
        color.a=fminf(1,e->tuning.intensity*(.35f+.65f*p.glow));
        if(!MaskPatch(w,e,clip,kActionMaskMossFirst+(e->particle_style.seed+i)%kActionMaskMossCount,
            p.x,p.y,p.size*9,p.size*4,color,project,context))return false;
        continue;
      }
      /* A bright core and a feathered halo share one quad. Transparent edges
       * avoid hard diamond flicker under subpixel camera/rotation movement. */
      color=MixColor(color,(ArRenderColorF){1,1,.82f,1},.2f);
      color.a=fminf(1,e->tuning.intensity*(.10f+.90f*p.glow));
      if(!MaskPatch(w,e,clip,kActionMaskFirefly,p.x,p.y,p.size*6,p.size*6,color,project,context))return false;
    }
  }
  return true;
}
static bool TexturedFoliage(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectFloorField *floor,const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!s->strands||s->strands>8||!isfinite(s->softness)||s->softness<0||s->softness>1||(s->shadows&&(!floor||floor->count>kActionCanopyMaxSpans)))return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,height=r->y1-r->y0,spacing=width/s->strands;
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  const float angle=s->angle*.01745329252f,cs=cosf(angle),sn=sinf(angle);
  for(unsigned i=0;i<s->strands;++i) {
    const uint32_t seed=DeterministicHash_Mix32(e->particle_style.seed^(i+1)*71u);
    const float rx=fminf(spacing*.48f,height*(s->crown?.65f:.20f)),length=height*(.67f+.31f*HashUnit(seed^3));
    const float x=r->x0+spacing*(i+.5f),sway=sinf(phase+HashUnit(seed^17)*6.2831853f)*s->amplitude;
    ArRenderPointF p[4]={{x-rx,r->y0},{x+rx,r->y0},{x+rx+sway,r->y0+length},{x-rx+sway,r->y0+length}};
    const unsigned cell=s->crown?kActionMaskCrown:kActionMaskVineFirst+seed%kActionMaskVineCount;
    ArRenderPointF uv[4]={ActionEffectMasks_Uv(cell,0,0),ActionEffectMasks_Uv(cell,1,0),
        ActionEffectMasks_Uv(cell,1,1),ActionEffectMasks_Uv(cell,0,1)};
    for(unsigned j=0;j<4;++j){const ArRenderPointF q=p[j];p[j]=(ArRenderPointF){cs*q.x+sn*q.y,-sn*q.x+cs*q.y};}
    ArRenderColorF color=FieldColor(e->tuning.color);
    color.a=fminf(1,e->tuning.intensity)*(.75f+.25f*HashUnit(seed^91))*(1-s->softness*.3f);
    if(!s->shadows) {if(!MaskQuad(w,e,clip,p,uv,color,NULL,project,context))return false;continue;}
    /* The complete world-anchored collision field clips the stencil before
     * projection. Roofing stays clear regardless of viewport or sway phase. */
    for(unsigned j=0;j<floor->count;++j) {
      const ActionEffectFloorSpan *span=&floor->spans[j];
      const ActionEffectLocalRect safe={fmaxf(clip->x0,span->x0-e->world_x),clip->y0,
          fminf(clip->x1,span->x1-e->world_x),fminf(clip->y1,span->y-e->world_y)};
      if(safe.x1>safe.x0&&safe.y1>safe.y0&&!MaskQuad(w,e,&safe,p,uv,color,NULL,project,context))return false;
    }
  }
  return true;
}

/* Shared geometry for visible shafts and light scattered by mist. Sampling
 * the same tapered strips prevents a fog crest shining below a blocked ray. */
static bool CanopyStrip(const ActionEffectInstance *e,const ActionEffectFloorField *hits,
    unsigned i,float lean,ArRenderPointF p[6],float opacity[2]) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFloorSpan *hit=&hits->spans[i];
  const float x0=hit->x0-e->world_x,x1=hit->x1-e->world_x;
  const float reach=hit->y-e->world_y-r->y0;
  if(reach<=0)return false;
  const float left=i?fminf(reach,hits->spans[i-1].y-e->world_y-r->y0):reach;
  const float right=i+1<hits->count?fminf(reach,hits->spans[i+1].y-e->world_y-r->y0):reach;
  const float margin=(r->x1-r->x0)*(.12f+.24f*e->field_style.softness);
  opacity[0]=SceneSoftFalloff(fmaxf(0,1-fminf(x0-r->x0,r->x1-x0)/margin));
  opacity[1]=SceneSoftFalloff(fmaxf(0,1-fminf(x1-r->x0,r->x1-x1)/margin));
  const float feather=24+120*e->field_style.softness;
  const float a=fmaxf(0,left-fminf(feather,left*.65f));
  const float b=fmaxf(0,right-fminf(feather,right*.65f));
  p[0]=(ArRenderPointF){x0,r->y0};p[1]=(ArRenderPointF){x1,r->y0};
  p[2]=(ArRenderPointF){x1+lean*b,r->y0+b};p[3]=(ArRenderPointF){x0+lean*a,r->y0+a};
  p[4]=(ArRenderPointF){x1+lean*right,r->y0+right};p[5]=(ArRenderPointF){x0+lean*left,r->y0+left};
  return true;
}
static float TriangleOpacity(ArRenderPointF point,const ArRenderPointF p[3],const float alpha[3]) {
  const float ax=p[1].x-p[0].x,ay=p[1].y-p[0].y,bx=p[2].x-p[0].x,by=p[2].y-p[0].y;
  const float det=ax*by-ay*bx;
  if(fabsf(det)<.0001f)return 0;
  const float x=point.x-p[0].x,y=point.y-p[0].y;
  const float u=(x*by-y*bx)/det,v=(ax*y-ay*x)/det;
  return u>=0&&v>=0&&u+v<=1 ? alpha[0]*(1-u-v)+alpha[1]*u+alpha[2]*v : 0;
}
typedef struct CanopyMask {
  unsigned cell,orientation;
  float left,top,inverse_size,crop,offset_u,offset_v;
} CanopyMask;
static CanopyMask CanopyMaskFor(const ActionEffectInstance *e,float lean) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  const float width=r->x1-r->x0,length=r->y1-r->y0;
  const uint32_t seed=DeterministicHash_Mix32(e->particle_style.seed^0x568bc12du);
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  const float sway=s->amplitude*(.8f*sinf(phase+HashUnit(seed^17)*6.2831853f)+
      .2f*sinf(phase*2+HashUnit(seed^71)*6.2831853f));
  const float crop=.70f+.27f*HashUnit(seed^127);
  return (CanopyMask){
    .cell=kActionMaskCanopyFirst+seed%kActionMaskCanopyCount,.orientation=(seed>>3)&7,
    .left=r->x0+fminf(0,lean*length)-s->amplitude-sway,.top=r->y0,
    .inverse_size=1/fmaxf(length,width+fabsf(lean*length)+2*s->amplitude),
    .crop=crop,.offset_u=(1-crop)*HashUnit(seed^31),.offset_v=(1-crop)*HashUnit(seed^97)};
}
static ArRenderPointF CanopyMaskUv(const CanopyMask *mask,float x,float y) {
  float u=(x-mask->left)*mask->inverse_size,v=(y-mask->top)*mask->inverse_size;
  /* Square rotations/reflections and a contained square crop give the full
   * seed a distinct pattern without stretching leaves or crossing atlas cells.
   * Only the shadow stencil turns; light transport keeps its authored angle. */
  if(mask->orientation&1){const float swap=u;u=v;v=swap;}
  if(mask->orientation&2)u=1-u;
  if(mask->orientation&4)v=1-v;
  return ActionEffectMasks_Uv(mask->cell,mask->offset_u+mask->crop*u,mask->offset_v+mask->crop*v);
}
float ActionCanopy_Sample(const ActionEffectInstance *e,const ActionEffectFloorField *hits,float world_x,float world_y) {
  if(!e||!hits||hits->count>kActionAuthoredFloorMaxSpans||!e->particle_lifetime)return 0;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  const float width=r->x1-r->x0,length=r->y1-r->y0,lean=tanf(s->angle*.01745329252f);
  if(width<=0||length<=0)return 0;
  const ArRenderPointF point={world_x-e->world_x,world_y-e->world_y};
  const float source_x=world_x-lean*(point.y-r->y0);
  for(unsigned i=0;i<hits->count;++i) {
    if(source_x<hits->spans[i].x0||source_x>hits->spans[i].x1)continue;
    ArRenderPointF p[6];float edge[2];
    if(!CanopyStrip(e,hits,i,lean,p,edge))continue;
    const float alpha[6]={edge[0],edge[1],edge[1],edge[0],0,0};
    const unsigned triangles[4][3]={{0,1,2},{0,2,3},{3,2,4},{3,4,5}};
    float opacity=0;
    for(unsigned t=0;t<4;++t) {
      const unsigned *v=triangles[t];
      const ArRenderPointF tri[3]={p[v[0]],p[v[1]],p[v[2]]};
      const float a[3]={alpha[v[0]],alpha[v[1]],alpha[v[2]]};
      opacity=fmaxf(opacity,TriangleOpacity(point,tri,a));
    }
    if(opacity<=0)continue;
    const CanopyMask mask=CanopyMaskFor(e,lean);
    const ArRenderPointF uv=CanopyMaskUv(&mask,point.x,point.y);
    const float illumination=e->tuning.illumination_set?e->tuning.illumination:ActionLightKind_Illumination(e->kind);
    return opacity*ActionEffectMasks_Sample(uv.x,uv.y)*fminf(1,e->tuning.intensity*.42f)*illumination;
  }
  return 0;
}
/* One coherent, world-anchored leaf-gap field. Straight parallel transport is
 * clipped at captured collision hits; nearby surfaces get a short warm return.
 * The material and airborne light use the very same mask/occlusion geometry. */
static bool CanopyLight(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectFloorField *hits,const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(!hits||hits->count>kActionAuthoredFloorMaxSpans||!isfinite(s->angle)||fabsf(s->angle)>70||
      !isfinite(s->softness)||s->softness<0||s->softness>1||!isfinite(s->scatter)||s->scatter<0||s->scatter>1||!isfinite(s->bounce)||s->bounce<0||s->bounce>1)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,length=r->y1-r->y0,lean=tanf(s->angle*.01745329252f);
  if(!isfinite(width)||!isfinite(length)||width<=0||length<=0||!e->particle_lifetime)return false;
  /* Sample a square in world space, not a stretched/sheared emitter UV box.
   * The same seeded mapping serves scenery, actors and mist light sampling. */
  const CanopyMask mask=CanopyMaskFor(e,lean);
  ArRenderColorF tint=FieldColor(e->tuning.color);
  tint.a=fminf(1,e->tuning.intensity*.42f)*(w->light_receiver?1:s->scatter);
  for(unsigned i=0;i<hits->count;++i) {
    const ActionEffectFloorSpan *hit=&hits->spans[i];
    const float x0=hit->x0-e->world_x,x1=hit->x1-e->world_x;
    const float bottom=hit->y-e->world_y,reach=bottom-r->y0;
    if(reach<=0)continue;
    ArRenderPointF p[6];float edges[2];
    if(!CanopyStrip(e,hits,i,lean,p,edges))continue;
    const float edge0=edges[0],edge1=edges[1];
    ArRenderPointF uv[4];
    for(unsigned j=0;j<4;++j)uv[j]=CanopyMaskUv(&mask,p[j].x,p[j].y);
    const float opacity[4]={edge0,edge1,edge1,edge0};
    if(!MaskQuad(w,e,clip,p,uv,tint,opacity,project,context))return false;
    {
      const ArRenderPointF edge[4]={p[3],p[2],p[4],p[5]};
      ArRenderPointF edge_uv[4];
      for(unsigned j=0;j<4;++j)edge_uv[j]=CanopyMaskUv(&mask,edge[j].x,edge[j].y);
      const float fade[4]={edge0,edge1,0,0};
      if(!MaskQuad(w,e,clip,edge,edge_uv,tint,fade,project,context))return false;
    }
    if(hit->height>0&&s->bounce>0&&w->light_receiver) {
      const ArRenderPointF sample=CanopyMaskUv(&mask,(x0+x1)*.5f+lean*reach,bottom);
      ArRenderColorF bounce=tint;
      bounce.a*=s->bounce*(edge0+edge1)*.5f*ActionEffectMasks_Sample(sample.x,sample.y);
      if(bounce.a>0&&!MaskPatch(w,e,clip,kActionMaskFogFirst+i%kActionMaskFogCount,
          (x0+x1)*.5f+lean*reach,bottom-3,(x1-x0)*.5f,14,bounce,project,context))return false;
    }
  }
  return true;
}

/* Two masks per validated cloud lobe, plus one player and one enemy wake.
 * This scratch list is local to a bank; no frame cache or invalidation state. */
enum { kRollingMistMaxStrands=24, kRollingMistMaxPatches=kRollingMistMaxStrands*2+2 };
typedef struct MistPatch {
  float x,y,rx,ry,crest;
  unsigned cell;
  uint32_t seed; /* Stable lobe identity, independent of view clipping. */
  ArRenderColorF tint;
} MistPatch;
static void AddMistPatch(MistPatch *patches,unsigned *count,const ActionEffectLocalRect *clip,MistPatch patch) {
  if(patch.x+patch.rx<clip->x0||patch.x-patch.rx>clip->x1||
      patch.y+patch.ry<clip->y0||patch.y-patch.ry>clip->y1)return;
  patches[(*count)++]=patch;
}
/* Evaluate each nearby glow once for all visible curls, rather than repeating
 * its seeded orbit/pulse for every curl. Canopy and sky fields are captured;
 * sampling them needs no light-map pass, readback or new collision query. */
static void TintMistPatches(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    MistPatch *patches,unsigned count) {
  for(unsigned i=0;i<count;++i) {
    const float alpha=patches[i].tint.a;
    const float ambient=.35f*(1-e->field_style.volume);
    patches[i].tint=MixColor(FieldColor(e->particle_style.color_end),FieldColor(e->tuning.color),
        ambient+(1-ambient)*patches[i].crest);
    patches[i].tint.a=alpha;
  }
  if(!count||!w->frame||w->atmosphere_unlit||e->field_style.canopy_response<=0)return;
  const ActionSceneEffectFrame *f=w->frame;
  if(f->authored_count>kActionAuthoredMaxInstances)return;
  ArRenderColorF incoming[kRollingMistMaxPatches]={0};
  for(unsigned i=0;i<f->authored_count;++i) {
    const ActionEffectInstance *light=&f->authored[i];
    if(!(light->flags&kActionEffectFlag_Visible)||(light->flags&kActionEffectFlag_LightingOff)||
        LightingWorldPlane(light->projection_plane)!=LightingWorldPlane(e->projection_plane))continue;
    if(light->kind==kActionEffect_AuthoredFireflies) {
      const ActionEffectLocalRect *r=&light->geometry.data.rect;
      const ActionEffectParticleStyle *style=&light->particle_style;
      const float movement=light->field_style.glow_style==kActionGlow_Moss?0:
          style->wander+fmaxf(fabsf(style->travel_x),fabsf(style->travel_y))*.5f;
      const float pad=movement+16+style->size_max*18;
      struct { unsigned patch; float x,y; } samples[kRollingMistMaxPatches];
      unsigned sample_count=0;
      for(unsigned k=0;k<count;++k) {
        const float x=e->world_x+patches[k].x-light->world_x,y=e->world_y+patches[k].y-light->world_y;
        if(x<r->x0-pad||x>r->x1+pad||y<r->y0-pad||y>r->y1+pad)continue;
        samples[sample_count].patch=k;samples[sample_count].x=x;samples[sample_count++].y=y;
      }
      if(!sample_count||!light->particle_lifetime)continue;
      for(unsigned j=0;j<light->particle_count&&j<32;++j) {
        const GlowPoint p=GlowPointAt(light,j);
        for(unsigned k=0;k<sample_count;++k) {
          const float distance=hypotf(samples[k].x-p.x,samples[k].y-p.y)/(16+p.size*18);
          const float amount=SceneSoftFalloff(distance)*p.glow*light->tuning.intensity*.22f;
          ArRenderColorF *color=&incoming[samples[k].patch];
          color->r+=p.color.r*amount;color->g+=p.color.g*amount;color->b+=p.color.b*amount;
        }
      }
    } else if(light->kind==kActionEffect_AuthoredLight) {
      for(unsigned k=0;k<count;++k) {
        const ArRenderColorF sample=ActionSoftLight_Sample(light,e->world_x+patches[k].x,e->world_y+patches[k].y);
        incoming[k].r+=sample.r;incoming[k].g+=sample.g;incoming[k].b+=sample.b;
      }
    } else if(light->kind==kActionEffect_AuthoredCanopy) {
      const ArRenderColorF color=FieldColor(light->tuning.color);
      for(unsigned k=0;k<count;++k) {
        const float amount=ActionCanopy_Sample(light,&f->authored_floor[i],e->world_x+patches[k].x,e->world_y+patches[k].y);
        incoming[k].r+=color.r*amount;incoming[k].g+=color.g*amount;incoming[k].b+=color.b*amount;
      }
    }
  }
  /* BG1 mist can catch the distant sunlight too. Translate its world samples
   * into BG2 coordinates, just like its spectral response. Sample once per
   * lobe; this only changes existing vertex colours, not geometry or passes. */
  if(!w->cloud_light_ready) {
    w->cloud_light=ActionCloudLight_PrepareWithSun(f,0,0,0,0,w->sun_directions);
    w->cloud_light_ready=true;
  }
  for(unsigned j=0;j<(unsigned)w->cloud_light.meta[0];++j) {
    const DioramaCloudLightRegion *light=&w->cloud_light.regions[j];
    if(light->color[3]==0||
        (light->core[1]==0&&light->core[2]==0&&light->core[3]==0))continue;
    for(unsigned k=0;k<count;++k) {
      const CloudLightSample sample=DioramaCloudLight_Sample(light,
          e->world_x+patches[k].x+f->bg1_to_bg2_x,
          e->world_y+patches[k].y+f->bg1_to_bg2_y);
      const float illumination=sample.halo*light->core[1]+sample.streak*light->core[2]+
          sample.echo*light->core[3];
      const float amount=.6f*(1-expf(-illumination*light->color[3]));
      incoming[k].r+=light->color[0]*amount;
      incoming[k].g+=light->color[1]*amount;
      incoming[k].b+=light->color[2]*amount;
    }
  }
  const float ambient=1-.35f*e->field_style.canopy_response;
  for(unsigned k=0;k<count;++k) {
    const float strength=e->field_style.canopy_response*(.55f+.65f*patches[k].crest);
    ArRenderColorF *tint=&patches[k].tint;
    tint->r=fminf(1,tint->r*ambient+incoming[k].r*strength);
    tint->g=fminf(1,tint->g*ambient+incoming[k].g*strength);
    tint->b=fminf(1,tint->b*ambient+incoming[k].b*strength);
  }
}
/* The camera ray comes from the same projection that places the mist. Native
 * source packets defer this calculation to the GPU, so changing the camera or
 * interpolation phase never invalidates their immutable spray/light samples. */
static RainbowDirection MistViewDirection(const ActionEffectInstance *e,float x,float y,
    const ActionEffectProjectionContext *c) {
  if(c&&c->diorama_projection) {
    const DioramaProjection *p=c->diorama_projection;
    ArRenderPointF pixel;
    if(p->output_width<=0||p->output_height<=0||
        !ActionEffectProjection_ProjectBillboardPoint((void *)c,e,x,y,&pixel))
      return (RainbowDirection){0};
    const float *m=p->matrix;
    return RainbowViewDirection(2*(pixel.x-p->output_x)/p->output_width-1,
        1-2*(pixel.y-p->output_y)/p->output_height,
        (RainbowDirection){m[0],m[4],m[8]},(RainbowDirection){m[1],m[5],m[9]},
        (RainbowDirection){m[3],m[7],m[11]});
  }
  /* Flat/custom projections retain a fixed virtual eye in native pixels;
   * output resolution and emitter bounds do not change the optical scale. */
  const ArRenderPointF origin=c?ActionEffectProjection_CaptureOrigin(c,e):
      (ArRenderPointF){e->world_x,e->world_y};
  const float rx=(origin.x+x-(c?c->ws_extra:0)-128)/512;
  const float ry=(112+(c?c->ws_extra_top:0)-origin.y-y)/512;
  return RainbowViewDirection(rx,ry,(RainbowDirection){1,0,0},
      (RainbowDirection){0,1,0},(RainbowDirection){0,0,-1});
}
static bool MistRainbow(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const MistPatch *patches,unsigned count,const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project,void *context) {
  if(e->field_style.rainbow<=0||!count||!w->frame||w->atmosphere_unlit)return true;
  if(!w->cloud_light_ready) {
    w->cloud_light=ActionCloudLight_PrepareWithSun(w->frame,0,0,0,0,w->sun_directions);
    w->cloud_light_ready=true;
  }
  if(!DioramaCloudLight_Active(&w->cloud_light))return true;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,height=r->y1-r->y0;
  const bool foreground=LightingWorldPlane(e->projection_plane)==kActionEffectProjectionPlane_Bg1;
  const float dx=foreground?w->frame->bg1_to_bg2_x:0;
  const float dy=foreground?w->frame->bg1_to_bg2_y:0;
  enum { columns=kActionMistRainbowColumns+1, rows=kActionMistRainbowRows+1 };
  ArRenderVertex2D vertices[columns*rows];int mapped[columns*rows];
  bool lit[columns*rows];
  const ActionEffectProjectionContext *camera=project==ActionEffectProjection_ProjectPoint?context:NULL;
  ActionEffectProjectionContext output_camera;
  DioramaProjection output_projection;
  if(camera&&camera->diorama_projection&&camera->diorama_projection->bg2_skybox.to_texture) {
    output_camera=*camera;output_projection=*camera->diorama_projection;
    output_projection.bg2_skybox.to_texture=false;
    output_camera.diorama_projection=&output_projection;camera=&output_camera;
  }
  bool illuminated=false;
  for(unsigned row=0;row<rows;++row)for(unsigned col=0;col<columns;++col) {
    const float x=r->x0+width*col/kActionMistRainbowColumns;
    const float y=r->y0+height*row/kActionMistRainbowRows;
    float density=0;
    for(unsigned j=0;j<count;++j) {
      const MistPatch *p=&patches[j];
      /* Paired body/crest masks describe one volume; do not count it twice. */
      if(p->crest>0||p->rx<=0||p->ry<=0)continue;
      const float s=(x-p->x)/(2*p->rx)+.5f,t=(y-p->y)/(2*p->ry)+.5f;
      if(s<=0||s>=1||t<=0||t>=1)continue;
      const ArRenderPointF uv=ActionEffectMasks_Uv(p->cell,s,t);
      density=fmaxf(density,p->tint.a*ActionEffectMasks_Sample(uv.x,uv.y));
    }
    /* Feather the spray volume's boundary, not a separately authored bow. */
    density*=fminf(1,fminf(fminf(x-r->x0,r->x1-x),fminf(y-r->y0,r->y1-y))/8);
    float weights[4]={0};
    bool receives_light=false;
    if(density>0)for(unsigned j=0;j<(unsigned)w->cloud_light.meta[0];++j) {
      const DioramaCloudLightRegion *light=&w->cloud_light.regions[j];
      if(light->core[2]==0||light->color[3]==0)continue;
      const CloudLightSample sample=DioramaCloudLight_Sample(light,x+e->world_x+dx,y+e->world_y+dy);
      /* Direct shafts alone produce spectral scattering. Ambient halo and
       * reflected cloud light must not conjure rainbows in shadowed spray. */
      const float luminance=.2126f*light->color[0]+.7152f*light->color[1]+.0722f*light->color[2];
      weights[j]=e->field_style.rainbow*fminf(1,density*3)*
          sample.streak*light->core[2]*light->color[3]*luminance;
      receives_light|=weights[j]>.0001f;
    }
    ArRenderColorF color;
    if(w->source)color=(ArRenderColorF){weights[0],weights[1],weights[2],weights[3]};
    else {
      const RainbowDirection ray=MistViewDirection(e,x,y,camera);
      float red=0,green=0,blue=0;
      for(unsigned j=0;j<4;++j)if(weights[j]>0) {
        const float alignment=ray.x*w->sun_directions[0][j]+ray.y*w->sun_directions[1][j]+ray.z*w->sun_directions[2][j];
        const RainbowDirection spectrum=RainbowSpectrum(alignment);
        red+=weights[j]*spectrum.x;green+=weights[j]*spectrum.y;blue+=weights[j]*spectrum.z;
      }
      color=RainbowFinish(red,green,blue);
    }
    const unsigned at=row*columns+col;
    lit[at]=receives_light;illuminated|=receives_light;
    vertices[at]=(ArRenderVertex2D){.position={x,y},.color=color};
    mapped[at]=-1;
  }
  if(!illuminated)return true;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  for(unsigned row=0;row<kActionMistRainbowRows;++row)for(unsigned col=0;col<kActionMistRainbowColumns;++col) {
    const int a=(int)(row*columns+col),b=a+columns;
    const int triangles[]={a,a+1,b+1,a,b+1,b};
    for(unsigned t=0;t<6;t+=3) {
      /* This depends only on captured droplets/illumination, never the view.
       * Empty triangles can be omitted from retained packets altogether. */
      if(!lit[triangles[t]]&&!lit[triangles[t+1]]&&!lit[triangles[t+2]])continue;
      if(w->source) {
        if(!ActionEffectSource_RainbowTriangle(w->source,&mesh,vertices,triangles+t,clip,w->sun_directions))return false;
        w->vertex_count=(int)w->source->count;
      } else if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
    }
  }
  return true;
}
/* A few droplets per body lobe, following its translation and density mask.
 * Stable lobe seeds keep a partial view identical to a crop of the whole bank.
 * The paired crest isn't another volume, so it must not double the sparkles. */
static bool MistSparkles(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const MistPatch *patches,unsigned count,const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project,void *context) {
  if(e->field_style.sparkle<=0||!w->frame||w->atmosphere_unlit)return true;
  for(unsigned i=0;i<count;++i) {
    const MistPatch *p=&patches[i];
    if(p->crest>0||p->rx<=0||p->ry<=0||p->tint.a<=0)continue;
    ActionEffectInstance source=*e;source.pulse_generation=p->seed;
    for(unsigned j=0;j<kActionMistSparklesPerStrand;++j) {
      const SceneParticleClock clock=SceneParticleClockAt(&source,e->pulse_ticks,j,
          (SceneParticleLifetime){72,5,63});
      const float t=clock.t;
      const float u=.16f+.68f*HashUnit(clock.seed^17)+.06f*(t-.5f);
      const float v=.16f+.68f*HashUnit(clock.seed^53)-.08f*t;
      const float x=p->x+(u-.5f)*2*p->rx,y=p->y+(v-.5f)*2*p->ry;
      const float size=.55f+.65f*HashUnit(clock.seed^97);
      if(x+size*1.5f<clip->x0||x-size*1.5f>clip->x1||y+size<clip->y0||y-size>clip->y1)continue;
      const ArRenderPointF uv=ActionEffectMasks_Uv(p->cell,u,v);
      const float density=p->tint.a*ActionEffectMasks_Sample(uv.x,uv.y);
      if(density<=.001f)continue;
      const ArRenderColorF light=DirectSkyLight(w,e,x,y);
      if(light.a<=.001f)continue;
      const float pulse=sinf(t*3.14159265f),squared=pulse*pulse;
      const float alpha=e->field_style.sparkle*fminf(1,density*4)*
          fminf(1,light.a*3)*squared*squared;
      if(alpha<=.002f)continue;
      const ArRenderColorF color={.65f+.35f*light.r/light.a,
          .65f+.35f*light.g/light.a,.65f+.35f*light.b/light.a,alpha};
      if(!AppendSceneParticle(w,e,x,y,x,y-1,size*1.3f,size*.7f,color,project,context))return false;
    }
  }
  return true;
}
static bool RollingMist(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context,bool lighting) {
  const ActionEffectFieldStyle *s=&e->field_style;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  const CloudBankMotion motion={.x=r->x0,.y=r->y0,.width=r->x1-r->x0,.height=r->y1-r->y0,
    .drift=s->drift,.amplitude=s->amplitude,.rolling=s->rolling,.intensity=e->tuning.intensity,
    .phase=phase,.wind_phase=sinf(phase),.strands=s->strands,.volume=s->volume>0};
  MistPatch patches[kRollingMistMaxPatches];unsigned count=0;
  for(unsigned i=0;i<s->strands;++i) {
    const uint32_t seed=e->particle_style.seed^(i+1)*0x9e3779b9u;
    CloudBankPatch pair[2];
    CloudMotion_Bank(&motion,i,seed,pair);
    for(unsigned tier=0;tier<2;++tier) {
      const CloudBankPatch *p=&pair[tier];
      const unsigned cell=motion.volume?
          (tier?kActionMaskCloudLightFirst:kActionMaskCloudBodyFirst)+DeterministicHash_Mix32(seed)%kActionMaskCloudCount:
          kActionMaskFogFirst+(seed+tier*3)%kActionMaskFogCount;
      AddMistPatch(patches,&count,clip,(MistPatch){.x=p->x,.y=p->y,.rx=p->rx,.ry=p->ry,
          .crest=tier,.cell=cell,.seed=seed,.tint={.a=p->alpha}});
    }
  }
  /* A fixed allowance per bank; prefer the player and then the strongest
   * nearby body. This never consumes native actor/sprite slots. */
  const ActionEffectActor *actors[2]={0};float weights[2]={0};
  const unsigned actor_count=w->frame&&s->wake>0&&w->frame->actor_count<=kActionEffectActorMax?w->frame->actor_count:0;
  for(unsigned i=0;i<actor_count;++i) {
    const ActionEffectActor *actor=&w->frame->actors[i];const ActionActorWake *wake=&actor->wake;
    if(!actor->body_radius||!actor->visible||wake->strength<=0||wake->age>=kActionActorWakeTicks)continue;
    const float x=wake->x-e->world_x,y=wake->y-e->world_y;
    const float edge=fminf(fminf(x-r->x0,r->x1-x),fminf(y-r->y0,r->y1-y));
    if(edge<=0)continue;
    const float weight=wake->strength*(1-wake->age/(float)kActionActorWakeTicks)*fminf(1,edge/24);
    const unsigned slot=actor->player?0:1;
    if(weight>weights[slot]){actors[slot]=actor;weights[slot]=weight;}
  }
  for(unsigned i=0;i<2;++i)if(actors[i]) {
    const ActionEffectActor *actor=actors[i];const ActionActorWake *wake=&actor->wake;
    const float age=wake->age/(float)kActionActorWakeTicks,side=wake->dx<0?-1.f:1.f;
    const float x=wake->x-e->world_x-side*(12+age*20)+s->drift*age*.2f;
    const float y=wake->y-e->world_y-4-age*12;
    AddMistPatch(patches,&count,clip,(MistPatch){.x=x,.y=y,
        .rx=actor->body_radius+24+age*24,.ry=8+age*10,.crest=1,
        .cell=kActionMaskFogFirst+actor->generation%kActionMaskFogCount,
        .tint={.a=fminf(.65f,e->tuning.intensity*s->wake*weights[i]*.7f)}});
  }
  TintMistPatches(w,e,patches,count);
  const bool lava_light=!w->atmosphere_unlit&&LightingWorldPlane(e->projection_plane)==kActionEffectProjectionPlane_Bg1;
  float bursts[kRollingMistMaxPatches];
  if(lava_light) {
    ArRenderPointF points[kRollingMistMaxPatches];
    for(unsigned i=0;i<count;++i)points[i]=(ArRenderPointF){e->world_x+patches[i].x,e->world_y+patches[i].y};
    ActionLavaActivity_Sample(w->frame,points,count,bursts);
  }
  for(unsigned i=0;i<count;++i) {
    if(lava_light) {
      const float burst=bursts[i];
      ArRenderColorF *tint=&patches[i].tint;
      tint->r=fminf(1,tint->r+burst*.85f);
      tint->g=fminf(1,tint->g+burst*.32f);
      tint->b=fminf(1,tint->b+burst*.06f);
    }
    const MistPatch *p=&patches[i];
    if(!MaskPatch(w,e,clip,p->cell,p->x,p->y,p->rx,p->ry,p->tint,project,context))return false;
  }
  return !lighting||(MistRainbow(w,e,patches,count,clip,project,context)&&
      MistSparkles(w,e,patches,count,clip,project,context));
}

/* Feathered source-space filaments retain their distant softness at every
 * output resolution. All return strokes share this bounded geometry. */
static bool StormRibbon(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, const ArRenderPointF *points, unsigned count,
    float strength, float thickness, ActionEffectProjectPointFn project, void *context) {
  if (strength <= 0) return true;
  ActionEffectInstance mesh = *e;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float stroke_scale = fminf(1, fmaxf(.45f,
      e->field_style.storm.ground_colors ?
      (e->geometry.data.rect.x1 - e->geometry.data.rect.x0) / 320 :
      (e->geometry.data.rect.y1 - e->geometry.data.rect.y0) / 112));
  const float softness = e->field_style.softness;
  const float radius = stroke_scale * thickness * (.75f + softness * 1.8f);
  ArRenderColorF centre = MixColor(FieldColor(e->tuning.color),
      (ArRenderColorF){1, 1, 1, 1}, .35f);
  centre.a = fminf(1, strength * .88f * e->tuning.intensity / (1 + softness * .65f));
  ArRenderColorF edge = FieldColor(e->tuning.color);
  edge.a = 0;
  for (unsigned i = 1; i < count; ++i) {
    const ArRenderPointF a = points[i - 1], b = points[i];
    const float length = hypotf(b.x - a.x, b.y - a.y);
    if (length < .001f) continue;
    const float nx = -(b.y - a.y) * radius / length;
    const float ny = (b.x - a.x) * radius / length;
    const float taper_a = 1 - .72f * (i - 1) / (count - 1.f);
    const float taper_b = 1 - .72f * i / (count - 1.f);
    for (unsigned side = 0; side < 2; ++side) {
      const float sign = side ? 1.f : -1.f;
      ArRenderVertex2D vertices[] = {
        {{a.x, a.y}, centre, {0, 0}},
        {{a.x + nx * sign * taper_a, a.y + ny * sign * taper_a}, edge, {0, 0}},
        {{b.x + nx * sign * taper_b, b.y + ny * sign * taper_b}, edge, {0, 0}},
        {{b.x, b.y}, centre, {0, 0}}
      };
      int mapped[] = {-1, -1, -1, -1};
      const int triangles[] = {0, 1, 2, 0, 2, 3};
      for (unsigned t = 0; t < 6; t += 3)
        if (!AppendSceneClippedTriangle(w, &mesh, vertices, mapped, triangles + t,
                clip, project, context)) return false;
    }
  }
  return true;
}

/* Fixed sky points, independent shimmer phases and a faint haze. Source-art
 * coverage is captured separately so this renderer never reads native tiles. */
static bool Starfield(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(e->particle_count>kActionSkyMaxStars||!ActionAuthoredParticles_Valid(&e->particle_style)||
      !isfinite(s->scatter)||s->scatter<0||s->scatter>1||!isfinite(s->angle)||fabsf(s->angle)>70||
      e->projection_plane!=kActionEffectProjectionPlane_Bg2||s->placement!=0)return false;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  if(s->scatter>0) {
    ArRenderVertex2D vertices[(kActionSkyHazeSegments+1)*3];int mapped[(kActionSkyHazeSegments+1)*3];
    for(unsigned col=0;col<=kActionSkyHazeSegments;++col)for(unsigned row=0;row<3;++row) {
      const unsigned n=col*3+row;
      const float edge=sinf(col*3.1415927f/kActionSkyHazeSegments);
      const float density=.6f+.4f*ActionStarfield_Unit(e->particle_style.seed^(col/3)*137u);
      vertices[n]=(ArRenderVertex2D){.position=ActionStarfield_HazePoint(e,col,row),
          .color=FieldColor(e->particle_style.color_end)};
      vertices[n].color.a=row==1?.12f*e->tuning.intensity*s->scatter*edge*edge*density*
          ActionStarfield_Visibility(e,kActionSkyMaxStars+n):0;
      mapped[n]=-1;
    }
    for(unsigned col=0;col<kActionSkyHazeSegments;++col)for(unsigned row=0;row<2;++row) {
      const int a=col*3+row,b=a+3,triangles[]={a,b,a+1,b,b+1,a+1};
      for(unsigned t=0;t<6;t+=3) {
        if(vertices[triangles[t]].color.a<=0&&vertices[triangles[t+1]].color.a<=0&&vertices[triangles[t+2]].color.a<=0)continue;
        if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
      }
    }
  }
  for(unsigned i=0;i<e->particle_count;++i) {
    const float visibility=ActionStarfield_Visibility(e,i);
    if(visibility<=0)continue;
    const uint32_t seed=e->particle_style.seed^i*0x9e3779b9u;
    const ArRenderPointF p=ActionStarfield_Point(e,i);
    const float weight=ActionStarfield_Unit(seed^29),bright=weight>.82f?1:.4f+.35f*weight;
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*weight*weight;
    /* Most stars stay steady. Independent phases and rates keep the brighter
     * minority gently shimmering without synchronized flashes or respawns. */
    const float shimmer=weight>.72f?1-s->twinkle*(.5f+.5f*sinf(phase*(1+i%3)+ActionStarfield_Unit(seed^53)*6.2831853f)):1;
    ArRenderColorF tint=FieldColor(e->tuning.color);
    tint.a=fminf(1,.65f*e->tuning.intensity)*bright*visibility*shimmer;
    const ArRenderPointF corners[]={{p.x-size,p.y},{p.x,p.y-size},{p.x+size,p.y},{p.x,p.y+size}};
    ArRenderVertex2D vertices[4];int mapped[]={-1,-1,-1,-1};
    for(unsigned j=0;j<4;++j)vertices[j]=(ArRenderVertex2D){.position=corners[j],.color=tint};
    const int triangles[]={0,1,2,0,2,3};
    for(unsigned t=0;t<6;t+=3)
      if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
  }
  return true;
}

/* Soft curtains with traveling folds and sweeping upper veils. Integer time
 * harmonics keep every activity setting continuous at the cycle boundary. */
static bool Aurora(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->strands<1||s->strands>kActionAuroraMaxCurtains||!isfinite(s->angle)||fabsf(s->angle)>70||
      !isfinite(s->softness)||s->softness<0||s->softness>1||
      e->projection_plane!=kActionEffectProjectionPlane_Bg2||s->placement!=0)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float width=r->x1-r->x0,height=r->y1-r->y0;
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
  const float energy=SkyLightEnvelope(e),lean=tanf(s->angle*.01745329252f);
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  for(unsigned strand=0;strand<s->strands;++strand) {
    const uint32_t seed=e->particle_style.seed^strand*0x9e3779b9u;
    const float offset=HashUnit(seed)*6.2831853f;
    ArRenderVertex2D vertices[(kActionAuroraSegments+1)*4];int mapped[(kActionAuroraSegments+1)*4];
    for(unsigned col=0;col<=kActionAuroraSegments;++col) {
      const float u=col/(float)kActionAuroraSegments,spatial=u*6.2831853f;
      const float edge=sinf(u*3.1415927f);
      const float activity=s->gust;
      const float flow=phase*(strand&1?-1:1);
      const float x=r->x0+width*u+s->drift*(sinf(flow+offset)+
          .45f*activity*sinf(spatial*1.6f-flow*2+offset))*edge;
      const float broad=.7f*sinf(spatial*1.1f+flow+offset)+.3f*sinf(spatial*2.4f-flow+offset*.7f);
      const float fold=broad*(1-.45f*activity)+activity*(.28f*sinf(spatial*3.2f-flow*2+offset)+
          .17f*sinf(spatial*5.1f+flow*3+offset*.6f));
      const float bottom=r->y0+height*(.53f+.22f*strand/fmaxf(1,s->strands-1))+s->amplitude*fold;
      const float veil=height*(.18f+.2f*s->softness+.05f*cosf(spatial*.7f+flow+offset))*
          (1+.25f*activity*sinf(spatial*2.2f-flow*2+offset));
      const float calm=cosf(spatial*3+flow+offset);
      const float rays=cosf(spatial*3+flow*3+offset+activity*sinf(spatial*1.7f+flow));
      const float gathered=(1-activity)*(.65f+.35f*calm*calm)+activity*(.4f+.6f*rays*rays);
      const float rows[]={-1,-.42f,-.06f,.10f},alpha[]={0,.18f+.2f*activity,1,0};
      for(unsigned row=0;row<4;++row) {
        const unsigned n=col*4+row;
        const float y=bottom+veil*rows[row];
        const float tint_mix=fminf(1,.16f+.2f*strand+.35f*(1-row/3.f));
        const float sweep=lean+.1f*activity*sinf(spatial+flow*2+offset);
        vertices[n]=(ArRenderVertex2D){.position={x+sweep*(bottom-y),y},
            .color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),tint_mix)};
        vertices[n].color.a=fminf(.75f,e->tuning.intensity*.22f)*alpha[row]*edge*edge*gathered*energy;
        mapped[n]=-1;
      }
    }
    for(unsigned col=0;col<kActionAuroraSegments;++col)for(unsigned row=0;row<3;++row) {
      const int a=col*4+row,b=a+4,triangles[]={a,b,a+1,b,b+1,a+1};
      for(unsigned t=0;t<6;t+=3)
        if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
    }
  }
  return true;
}

/* Clip the cloud flash in source space like the channel. A screen billboard
 * would be submitted whole for every intersecting sky band in the editor,
 * while the resident weather path draws the union once. This compact fan also
 * replaces 160 billboard primitives with 16 clipped source triangles. */
static bool StormGlow(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, ArRenderPointF origin, float strength,
    ActionEffectProjectPointFn project, void *context) {
  ActionEffectInstance mesh = *e;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const ActionEffectLocalRect *r = &e->geometry.data.rect;
  const float rx = (r->x1 - r->x0) * .48f, ry = ActionStorm_GlowHeight(e);
  /* Illuminate the emitting cloud around the channel root. Offsetting this
   * below the root made the distant strikes look detached from their cloud. */
  ArRenderColorF tint = FieldColor(e->tuning.color);
  tint.a = fminf(1, .12f * strength * e->tuning.intensity);
  ArRenderVertex2D vertices[kActionStormGlowSegments + 1] = {{origin, tint, {0, 0}}};
  int mapped[kActionStormGlowSegments + 1];
  mapped[0] = -1;
  tint.a = 0;
  for (unsigned i = 0; i < kActionStormGlowSegments; ++i) {
    vertices[i + 1] = (ArRenderVertex2D){
      {origin.x + kCircle32[i * 2][0] * rx, origin.y + kCircle32[i * 2][1] * ry},
      tint, {0, 0}
    };
    mapped[i + 1] = -1;
  }
  for (unsigned i = 0; i < kActionStormGlowSegments; ++i) {
    const int triangle[] = {0, (int)i + 1, (int)((i + 1) % kActionStormGlowSegments) + 1};
    if (!AppendSceneClippedTriangle(w, &mesh, vertices, mapped, triangle,
            clip, project, context)) return false;
  }
  return true;
}

static bool StormLightning(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project, void *context) {
  const ActionEffectFieldStyle *s = &e->field_style;
  if (e->particle_lifetime < 120 || s->strands < 1 || s->strands > kActionStormMaxForks + 1 ||
      !isfinite(s->activity) || s->activity < .01f || s->activity > .2f) return false;
  ActionStormFlash flash;
  if (!ActionStorm_Flash(e, &flash)) return true;
  ActionStormChannel channel;
  ActionStorm_Path(e, flash.seed, &channel);
  if (!StormGlow(w, e, clip, channel.main[0], flash.cloud, project, context) ||
      !StormRibbon(w, e, clip, channel.main, kActionStormMainSegments + 1,
          flash.channel, 1, project, context)) return false;
  for (unsigned i = 0; i + 1 < s->strands; ++i)
    if (!StormRibbon(w, e, clip, channel.forks[i], kActionStormForkSegments + 1,
            flash.forks[i], .75f, project, context)) return false;
  return true;
}

bool AppendAuthoredField(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,const ActionEffectFloorField *floor,bool lighting,bool particles,
    ActionEffectProjectPointFn project,ActionEffectClipBoundsFn clip_fn,void *context) {
  ActionEffectInstance anchored;
  if (e->kind == kActionEffect_AuthoredStorm &&
      e->projection_plane == kActionEffectProjectionPlane_Bg2 && (e->flags & kActionEffectFlag_StaticAnchor)) {
    anchored = *e;
    const int row = (int)floorf(ActionStorm_SourceY(e));
    anchored.world_y += row;
    anchored.geometry.data.rect.y0 -= row;
    anchored.geometry.data.rect.y1 -= row;
    anchored.clip_rect.y0 -= row;
    anchored.clip_rect.y1 -= row;
    e = &anchored;
  }
  ActionEffectInstance motion=*e;const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  if((e->kind==kActionEffect_AuthoredFireflies || e->kind==kActionEffect_AuthoredParticlePath || e->kind==kActionEffect_AuthoredParticleArea || e->kind==kActionEffect_AuthoredWater ||
      e->kind==kActionEffect_AuthoredDrips || e->kind==kActionEffect_AuthoredSpray ||
      e->kind==kActionEffect_AuthoredCloud) && !ActionAuthoredParticles_Valid(&e->particle_style))return false;
  if(s->placement>3 || !isfinite(s->amplitude)||s->amplitude<0||s->amplitude>32 ||
      !isfinite(s->drift)||fabsf(s->drift)>128||!isfinite(s->gust)||s->gust<0||s->gust>1||
      !isfinite(s->wake)||s->wake<0||s->wake>1||
      !isfinite(s->twinkle)||s->twinkle<0||s->twinkle>1)return false;
  if(e->kind==kActionEffect_AuthoredHeat||e->kind==kActionEffect_AuthoredCloudLight)return true; /* Background texture sampling owns this effect. */
  if(e->kind==kActionEffect_AuthoredStorm) {
    /* The cloud halo extends past the strike region. Use its whole envelope
     * for admission; an offscreen bolt may still illuminate visible cloud. */
    motion.geometry.data.rect = ActionStorm_Bounds(e);
  }
  if(e->kind==kActionEffect_AuthoredAurora) {
    if(!isfinite(s->angle)||fabsf(s->angle)>70)return false;
    const float pad=fabsf(s->drift)*(1+.45f*s->gust)+
        (fabsf(tanf(s->angle*.01745329252f))+.1f*s->gust)*(r->y1-r->y0);
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-s->amplitude-(r->y1-r->y0)*.01f,r->x1+pad,r->y1+s->amplitude};
  }
  if(e->kind==kActionEffect_AuthoredStarfield) {
    const float pad=e->particle_style.size_max;
    const float vertical=fabsf(tanf(s->angle*.01745329252f))*(r->x1-r->x0)*.1f+pad;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-vertical,r->x1+pad,r->y1+vertical};
  }
  if(e->kind==kActionEffect_AuthoredParticlePath) {
    const float pad=fmaxf(e->particle_style.wander,s->pattern==kActionParticle_Butterflies&&s->wake>0?36:0)+e->particle_style.size_max*(s->pattern==kActionParticle_IceCrystals?2.25f:2);
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-pad,r->x1+pad,r->y1+pad};
  }
  if(e->kind==kActionEffect_AuthoredCanopy) {
    const float lean=tanf(s->angle*.01745329252f)*(r->y1-r->y0);
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0+fminf(0,lean),r->y0-16,r->x1+fmaxf(0,lean),r->y1+16};
  } else if(e->kind==kActionEffect_AuthoredFoliage) {
    if(!isfinite(s->angle)||fabsf(s->angle)>180)return false;
    const float angle=s->angle*.01745329252f;
    const float pad=(r->y1-r->y0)*.25f+s->amplitude+(s->flowers?(s->petals?28:10):0);
    const float x=(r->x1-r->x0)*.5f+pad,y=(r->y1-r->y0)*.5f+pad;
    const float ex=fabsf(cosf(angle))*x+fabsf(sinf(angle))*y;
    const float ey=fabsf(sinf(angle))*x+fabsf(cosf(angle))*y;
    motion.geometry.data.rect=(ActionEffectLocalRect){-ex,-ey,ex,ey};
  } else if(e->kind==kActionEffect_AuthoredFireflies) {
    const float pad=e->particle_style.wander+fmaxf(fabsf(e->particle_style.travel_x),fabsf(e->particle_style.travel_y))*.5f+8+e->particle_style.size_max*12;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-pad,r->x1+pad,r->y1+pad};
  } else if(e->kind==kActionEffect_AuthoredParticleArea) {
    const float pad=600+44*s->wake;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-pad,r->x1+pad,r->y1+pad};
  } else if(e->kind==kActionEffect_AuthoredWater) {
    const float pad=e->particle_style.size_max*3;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-s->amplitude-pad,r->x1+pad,fmaxf(r->y1,r->y0+s->amplitude)+pad};
  } else if(e->kind==kActionEffect_AuthoredDrips) {
    const float pad=fmaxf(s->ripple||s->landing?12:0,e->particle_style.size_max*2);
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-pad,r->x1+pad,r->y1+pad};
  } else if(e->kind==kActionEffect_AuthoredCloud) {
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-fabsf(s->drift),r->y0-s->amplitude,r->x1+fabsf(s->drift),r->y1+s->amplitude};
    if(s->point_count) {
      float rise=(8+2*s->amplitude)*1.5f+e->particle_style.wander+e->particle_style.size_max*2+36*s->wake;
      if(s->wake>0) {
        const float motion_strength=sqrtf(s->wake);
        const float radius=GroundPowderRadius(e->particle_style.size_max,motion_strength,1);
        const float pad=fmaxf(fabsf(s->drift),2*radius+18*motion_strength);
        motion.geometry.data.rect.x0=r->x0-pad;motion.geometry.data.rect.x1=r->x1+pad;
        rise=fmaxf(rise,18*motion_strength+2*radius*.95f);
      }
      motion.geometry.data.rect.y0=r->y0-fmaxf(rise,s->ground_depth+s->ground_width);
      if(s->ground_depth>0)motion.geometry.data.rect.y1=fmaxf(motion.geometry.data.rect.y1,r->y1+1.5f);
    }
  } else if(e->kind==kActionEffect_AuthoredFan) {
    const float extent=(r->x1-r->x0)*.5f+r->y1-r->y0;
    motion.geometry.data.rect=(ActionEffectLocalRect){-extent,-extent,extent,extent};
  }
  ActionEffectLocalRect visible=motion.geometry.data.rect;
  /* Free puffs test their animated footprint in AppendBillboard. */
  if(!((e->kind==kActionEffect_AuthoredCloud&&!s->point_count&&!s->textured) &&
      (w->source || project==ActionEffectProjection_ProjectPoint)) &&
      clip_fn&&!clip_fn(context,&motion,&visible))return true;
  switch(e->kind) {
    case kActionEffect_AuthoredStorm: return !lighting || StormLightning(w,e,&visible,project,context);
    case kActionEffect_AuthoredAurora: return !lighting || Aurora(w,e,&visible,project,context);
    case kActionEffect_AuthoredStarfield: return !lighting || Starfield(w,e,&visible,project,context);
    case kActionEffect_AuthoredCanopy: return !lighting || CanopyLight(w,e,floor,&visible,project,context);
    case kActionEffect_AuthoredFoliage: return !particles || (s->flowers?FlowerPatch(w,e,floor,&visible,project,context,lighting):s->textured?TexturedFoliage(w,e,floor,&visible,project,context):FoliageShadow(w,e,&visible,project,context));
    case kActionEffect_AuthoredFireflies: return Fireflies(w,e,&visible,lighting,particles,project,context);
    case kActionEffect_AuthoredExposure: {
      if(!lighting&&!particles)return true;
      if(!(e->tuning.dim_receivers&kActionReceiver_Scenery))return true;
      const ActionEffectInstance *boxes[]={e};
      return AppendExposureGroup(w,boxes,1,project,clip_fn,context);
    }
    case kActionEffect_AuthoredHalo: return !lighting || Halo(w,e,&visible,project,context);
    case kActionEffect_AuthoredGradient: return !lighting || LightGradient(w,e,&visible,project,context);
    case kActionEffect_AuthoredContour: return !particles || WetContour(w,e,&visible,project,context);
    case kActionEffect_AuthoredParticleArea: return !particles || ParticleArea(w,e,&visible,project,context,lighting);
    case kActionEffect_AuthoredParticlePath: return !particles || ParticlePath(w,e,&visible,project,context,lighting);
    case kActionEffect_AuthoredFan: {
      if (!lighting) return true;
      const int first = w->vertex_count;
      if (!LightFan(w, e, &visible, project, context)) return false;
      /* Fan vertices grow from the editor's anchor (local 0,0), not the
       * top of its symmetric bounds. A ceiling behind the source must not
       * shadow light already entering the chamber. */
      if(s->shadows)ActionSceneryShadow_Apply(w, first, e, 0, 0, false, 1, project, context);
      return true;
    }
    case kActionEffect_AuthoredWater: return !particles || WaterField(w,e,&visible,project,context);
    case kActionEffect_AuthoredDrips: case kActionEffect_AuthoredSpray:
      return !particles || FallingWater(w,e,&visible,project,context);
    case kActionEffect_AuthoredCloud: {
      if(!particles)return true;
      if(!isfinite(s->ground_depth)||s->ground_depth<0||s->ground_depth>12||
          (s->ground_depth>0&&(!s->point_count||s->placement!=2||e->projection_plane!=kActionEffectProjectionPlane_Bg1)))return false;
      if(!isfinite(s->ground_width)||s->ground_width<0||s->ground_width>24||
          (s->ground_width>0&&s->ground_depth<=0))return false;
      if(s->strands<1||s->strands>kRollingMistMaxStrands||!isfinite(s->softness)||s->softness<0||s->softness>1)return false;
      if(!isfinite(s->carry)||s->carry<0||s->carry>512||(!s->point_count&&(s->carry>0||e->particle_count)))return false;
      if(!isfinite(s->rolling)||s->rolling<0||s->rolling>1||!isfinite(s->wake)||s->wake<0||s->wake>1||
          !isfinite(s->canopy_response)||s->canopy_response<0||s->canopy_response>1)return false;
      if(!isfinite(s->volume)||s->volume<0||s->volume>1||
          (s->volume>0&&(!s->textured||s->point_count||s->rolling<=0)))return false;
      if(!isfinite(s->rainbow)||s->rainbow<0||s->rainbow>1||
          (s->rainbow>0&&(!s->textured||s->point_count||s->rolling<=0)))return false;
      if(!isfinite(s->sparkle)||s->sparkle<0||s->sparkle>1||
          (s->sparkle>0&&(!s->textured||s->point_count||s->rolling<=0)))return false;
      if(s->rolling>0&&(!s->textured||s->point_count))return false;
      if((s->wake>0||s->canopy_response>0)&&!s->point_count&&(!s->textured||s->rolling<=0))return false;
      if(s->plume)return RisingPlume(w,e,&visible,project,context);
      if(s->point_count)return GroundCloud(w,e,&visible,project,context,lighting);
      if(s->rolling>0)return RollingMist(w,e,&visible,project,context,lighting);
      const float width=r->x1-r->x0,height=r->y1-r->y0;
      const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
      for(unsigned i=0;i<s->strands;++i) {
        const uint32_t seed=(s->textured?e->particle_style.seed:e->generation)^i*0x9e3779b9u;
        const float x=r->x0+width*(.2f+.6f*HashUnit(seed))+sinf(phase+HashUnit(seed^7)*6)*s->drift;
        const float y=r->y0+height*(.25f+.5f*HashUnit(seed^91))+cosf(phase+HashUnit(seed^23)*6)*s->amplitude;
        if(s->textured) {
          /* Keep the whole feathered mask inside the authored field. Cutting
           * a drifting puff at that rectangle leaves a horizontal/vertical
           * seam in hanging haze, even at very low opacity. */
          const float rx=width*(.20f+.12f*HashUnit(seed^11)),ry=height*(.24f+.10f*HashUnit(seed^3));
          const float space_x=width*.5f-rx,space_y=height*.5f-ry;
          const float drift=fminf(fabsf(s->drift),space_x*.7f)*(s->drift<0?-1:1);
          const float billow=fminf(s->amplitude,space_y*.7f);
          const float px=(r->x0+r->x1)*.5f+(space_x-fabsf(drift))*(2*HashUnit(seed)-1)+sinf(phase+HashUnit(seed^7)*6)*drift;
          const float py=(r->y0+r->y1)*.5f+(space_y-billow)*(2*HashUnit(seed^91)-1)+cosf(phase+HashUnit(seed^23)*6)*billow;
          ArRenderColorF tint=FieldColor(e->tuning.color);tint.a=fminf(1,e->tuning.intensity*.7f);
          if(!MaskPatch(w,e,&visible,kActionMaskFogFirst+(seed%kActionMaskFogCount),px,py,rx,ry,tint,project,context))return false;
        } else if(!AppendSceneSoftCloud(w,e,x,y,width*(.10f+.08f*HashUnit(seed^11)),height*.25f,
            FieldColor(e->tuning.color),fminf(1,e->tuning.intensity*(.06f+.12f*s->softness)),seed,project,context))return false;
      }
      return true;
    }
    default:return false;
  }
}
