/* Pure authoring kernels. All storage belongs to the caller's retained batch;
 * no native actor allocation, simulation state, texture or extra GPU resolve. */
#include "action_effect_render_internal.h"
#include "action_authored_particles.h"
#include "action_exposure.h"

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
/* A shared, smooth wind envelope. Positions retain their analytic clocks, so
 * scrubbing and camera movement cannot restart or teleport a gust. */
static float WindEnvelope(const ActionEffectInstance *e) {
  const float phase=(e->pulse_ticks%4096)*6.2831853f/4096;
  const float gust=.5f+.3f*sinf(phase*5)+.2f*sinf(phase*11+.7f);
  return 1-e->field_style.gust*(1-gust);
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
/* Ordered paths support crest streamers, surface slides, falling grit and
 * sparse stationary glints. All sources and trajectories are editable; no
 * collision queries, actors or persistent particle simulations are needed. */
static bool ParticlePath(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->point_count<2||s->point_count>kActionContourMaxPoints||!isfinite(s->activity)||s->activity<.05f||s->activity>1||s->pattern>=kActionParticle_Count)return false;
  float lengths[kActionContourMaxPoints]={0},total=0;
  for(unsigned i=1;i<s->point_count;++i) {
    const float length=hypotf(s->points[i].x-s->points[i-1].x,s->points[i].y-s->points[i-1].y);
    if(!isfinite(length)||length<.01f)return false;
    lengths[i]=total+=length;
  }
  ActionEffectInstance source=*e;source.pulse_generation=e->particle_style.seed;
  const bool glint=s->pattern==kActionParticle_Glints;
  const float wind=WindEnvelope(e);
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
    const float x=s->points[segment-1].x+dx*u-dy/length*sway;
    const float y=s->points[segment-1].y+dy*u+dx/length*sway;
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(clock.seed^27);
    if(x+size*2<clip->x0||x-size*2>clip->x1||y+size*2<clip->y0||y-size*2>clip->y1)continue;
    ArRenderColorF color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),age);
    float fade=fmaxf(0,sinf(age*3.1415927f));if(glint)fade=fade*fade*fade*fade;
    color.a=fminf(1,e->tuning.intensity*.65f*fade*wind);
    Twinkle(s->twinkle,clock.seed,age,fade*wind*fminf(1,e->tuning.intensity),&color);
    if(!AppendSceneParticle(w,e,x,y,x-dx/length,y-dy/length,size*.65f,size*(glint?1.6f:1),color,project,context))return false;
  }
  return true;
}
static bool FieldParticle(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectInstance *clock_source, unsigned index, uint32_t seed, float birth_x, float birth_y, const ActionEffectLocalRect *clip,
    ActionEffectProjectPointFn project, void *context) {
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
    default: break;
  }
  if (x+reach<clip->x0 || x-reach>clip->x1 || y+reach<clip->y0 || y-reach>clip->y1) return true;
  ArRenderColorF color=MixColor(FieldColor(e->tuning.color),FieldColor(s->color_end),t);
  const float fade=fmaxf(0,sinf(t*3.1415927f));
  color.a=fminf(1,e->tuning.intensity*.7f*fade);
  if(e->field_style.pattern==kActionParticle_Glints) {
    color.a=fminf(1,e->tuning.intensity*.7f*fade*fade*fade*fade);
  }
  if(e->field_style.pattern==kActionParticle_Insects)color.a*=.5f+.5f*sinf(t*25+phase)*sinf(t*25+phase);
  Twinkle(e->field_style.twinkle,clock.seed,t,fade*fminf(1,e->tuning.intensity),&color);
  return AppendSceneParticle(w,e,x,y,x-dx*.003f,y-dy*.003f,width,reach,color,project,context);
}
static bool ParticleArea(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  if(!ActionAuthoredParticles_Valid(&e->particle_style) || e->particle_count>16 ||
      e->field_style.pattern>=kActionParticle_Count)return false;
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectParticleStyle *s=&e->particle_style;
  /* Cells are anchored in world space. Panning changes the visited set, never
   * seeds, ages or positions. Backtrack from the visible motion envelope to
   * birth cells; do not traverse or rasterize the complete saved region. */
  const float pad=s->wander*1.6f+s->size_max*3+25;
  const float x0=fmaxf(r->x0,clip->x0-fmaxf(0,s->travel_x)-pad)+e->world_x;
  const float x1=fminf(r->x1,clip->x1-fminf(0,s->travel_x)+pad)+e->world_x;
  const float y0=fmaxf(r->y0,clip->y0-fmaxf(0,s->travel_y)-pad)+e->world_y;
  const float y1=fminf(r->y1,clip->y1-fminf(0,s->travel_y)+pad)+e->world_y;
  if(x1<=x0 || y1<=y0)return true;
  const int first_x=(int)floorf(x0/kActionParticleCellSize), last_x=(int)floorf(x1/kActionParticleCellSize);
  const int first_y=(int)floorf(y0/kActionParticleCellSize), last_y=(int)floorf(y1/kActionParticleCellSize);
  if((last_x-first_x+1)*(last_y-first_y+1)>kActionParticleMaxCells)return false;
  ActionEffectInstance clock_source=*e;
  for(int cy=first_y;cy<=last_y;++cy)for(int cx=first_x;cx<=last_x;++cx) {
    const uint32_t seed=DeterministicHash_Mix32(s->seed^(uint32_t)cx*0x9e3779b9u^(uint32_t)cy*0x85ebca6bu);
    clock_source.pulse_generation=seed;
    for(unsigned i=0;i<e->particle_count;++i) {
      const float x=(cx+.5f+(HashUnit(seed^i*71u)-.5f)*s->spread)*kActionParticleCellSize-e->world_x;
      const float y=(cy+HashUnit(seed^i*91u^51u))*kActionParticleCellSize-e->world_y;
      if(x<r->x0 || x>r->x1 || y<r->y0 || y>r->y1)continue;
      if(!FieldParticle(w,e,&clock_source,i,seed,x,y,clip,project,context))return false;
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
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float softness=e->field_style.softness,angle=e->field_style.angle;
  if(!isfinite(softness)||softness<0||softness>1||!isfinite(angle)||fabsf(angle)>180)return false;
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const float dx=sinf(angle*.0174532925f),dy=cosf(angle*.0174532925f);
  const float width=r->x1-r->x0,height=r->y1-r->y0,span=fabsf(dx)*width+fabsf(dy)*height;
  ArRenderVertex2D vertices[25];int mapped[25];
  for(unsigned row=0;row<5;++row)for(unsigned col=0;col<5;++col){
    const unsigned n=row*5+col;const float x=r->x0+width*col*.25f,y=r->y0+height*row*.25f;
    const float t=fmaxf(0,fminf(1,.5f+(x*dx+y*dy)/span));
    const float edge=fminf(fminf(col,4-col),fminf(row,4-row))*.25f;
    const float fade=softness>0?fminf(1,edge/(softness*.5f)):1;
    vertices[n]=(ArRenderVertex2D){.position={x,y},.color=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),t)};
    vertices[n].color.a=fminf(1,e->tuning.intensity*.18f)*fade;mapped[n]=-1;
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
  ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
  const ArRenderColorF tint=FieldColor(e->tuning.color);
  for(unsigned i=0;i<s->strands;++i) {
    const float u=(i+.5f)/s->strands;
    const uint32_t seed=e->generation^i*0x9e3779b9u;
    const float angle=(s->angle+(u-.5f)*s->fan)*.0174532925f;
    const float dx=sinf(angle),dy=cosf(angle),rx=cosf(angle),ry=-sinf(angle);
    const float origin=(u-.5f)*width, reach=length*(.8f+.2f*HashUnit(seed));
    const float radius=fmaxf(.4f,width/s->strands*(.2f+.2f*HashUnit(seed^71)));
    ArRenderVertex2D vertices[9];int mapped[9];
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col) {
      const unsigned n=row*3+col;const float t=row*.5f,side=(float)col-1;
      vertices[n]=(ArRenderVertex2D){.position={origin+dx*reach*t+rx*side*radius*(1+2*t),
        dy*reach*t+ry*side*radius*(1+2*t)},.color=tint};
      vertices[n].color.a=col==1?fminf(1,e->tuning.intensity*(.10f+.06f*HashUnit(seed^27))*(row==2?0:row==0?.65f:1)):0;
      mapped[n]=-1;
    }
    for(unsigned row=0;row<2;++row)for(unsigned col=0;col<2;++col) {
      const int a=row*3+col,b=a+3,triangles[]={a,a+1,b,a+1,b+1,b};
      for(unsigned t=0;t<6;t+=3)if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,triangles+t,clip,project,context))return false;
    }
  }
  ArRenderColorF backdrop=tint;backdrop.a=fminf(1,s->softness*e->tuning.intensity*.10f);
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
  const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
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
static bool FallingWater(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  ActionEffectInstance source=*e;source.pulse_generation=e->particle_style.seed;
  for(unsigned i=0;i<e->particle_count;++i) {
    const SceneParticleClock clock=SceneParticleClockAt(&source,e->pulse_ticks,i,(SceneParticleLifetime){e->particle_lifetime,0,0});
    const float t=clock.t,birth=r->x0+(r->x1-r->x0)*HashUnit(clock.seed),height=r->y1-r->y0;
    float x=birth,y=r->y0+height*t*t,dy=1,reach=2;
    if(e->kind==kActionEffect_AuthoredSpray) {
      x+=(HashUnit(clock.seed^71)-.5f)*height*t;
      y=r->y1-height*3*t*(1-t);dy=t-.5f;reach=1;
    }
    if(x<clip->x0||x>clip->x1||y<clip->y0||y>clip->y1)continue;
    ArRenderColorF c=MixColor(FieldColor(e->tuning.color),FieldColor(e->particle_style.color_end),t);
    c.a=fminf(1,e->tuning.intensity*.65f*sinf(t*3.1415927f));
    const float size=e->particle_style.size_min+(e->particle_style.size_max-e->particle_style.size_min)*HashUnit(clock.seed^27);
    if(!AppendSceneParticle(w,e,x,y,x,y-dy,size,size*reach,c,project,context))return false;
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
static float GroundHeight(const ActionEffectFieldStyle *s, float x) {
  unsigned i=1;
  while(i+1<s->point_count && x>s->points[i].x)++i;
  const float t=fmaxf(0,fminf(1,(x-s->points[i-1].x)/(s->points[i].x-s->points[i-1].x)));
  return s->points[i-1].y+t*(s->points[i].y-s->points[i-1].y);
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
static bool GroundGrains(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip,ActionEffectProjectPointFn project,void *context) {
  const ActionEffectFieldStyle *s=&e->field_style;
  const ActionEffectParticleStyle *style=&e->particle_style;
  const float left=s->points[0].x,right=s->points[s->point_count-1].x;
  ActionEffectInstance clock_source=*e;clock_source.pulse_generation=style->seed;
  const float strength=WindEnvelope(e);
  for(unsigned i=0;i<e->particle_count;++i) {
    const SceneParticleClock clock=SceneParticleClockAt(&clock_source,e->pulse_ticks,i,
        (SceneParticleLifetime){e->particle_lifetime,9,127});
    const float t=clock.t,birth=left+(right-left)*HashUnit(clock.seed^71);
    const float wind=s->drift*(.6f+.8f*HashUnit(clock.seed^11));
    const float x=birth+wind*t;
    if(x<=left || x>=right)continue;
    const float size=style->size_min+(style->size_max-style->size_min)*HashUnit(clock.seed^27);
    const float lift=(8+2*s->amplitude)*(.35f+.65f*HashUnit(clock.seed^91));
    /* Fine grains are entrained in the gust; heavier grains make short hops.
     * The clamp only lifts grains out of rising terrain, never pulls them down. */
    const float weight=HashUnit(clock.seed^39),flight=weight<.65f?t*(1-.35f*t):4*t*(1-t);
    float y=GroundHeight(s,birth)-lift*flight+
        sinf(t*9+HashUnit(clock.seed^57)*6)*style->wander*sinf(t*3.1415927f);
    y=fminf(y,GroundHeight(s,x)-size*2);
    if(x+size<clip->x0||x-size>clip->x1||y+size<clip->y0||y-size>clip->y1)continue;
    ArRenderColorF tint=MixColor(FieldColor(e->tuning.color),FieldColor(style->color_end),t);
    const float edge=fminf(1,fminf(x-left,right-x)/16);
    const float envelope=fmaxf(0,sinf(t*3.1415927f))*edge*strength;
    tint.a=fminf(.9f,e->tuning.intensity*.32f)*envelope;
    Twinkle(s->twinkle,clock.seed,t,envelope*fminf(1,e->tuning.intensity),&tint);
    if(!AppendSceneParticle(w,e,x,y,x-wind*.003f,y+lift*.003f,size*.65f,size,tint,project,context))return false;
  }
  return true;
}
/* Overlapping gusts build on windward slopes, carry over crests and dissipate.
 * One bounded mesh is shared by CPU/WASM and GPU projection. */
static bool GroundCloud(ActionEffectGeometryWriter *w, const ActionEffectInstance *e,
    const ActionEffectLocalRect *clip, ActionEffectProjectPointFn project, void *context) {
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
  const float wind_density=WindEnvelope(e);
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
    const unsigned prev=col?col-1:col, next=col+1<s->point_count?col+1:col;
    const float slope=(s->points[next].y-s->points[prev].y)/(s->points[next].x-s->points[prev].x);
    const float bank=1+fminf(.5f,fmaxf(0,-slope*(s->drift>0?1:s->drift<0?-1:0)));
    const float edge=fminf(1,fminf(x-left,right-x)/fmaxf(8,(right-left)*.08f));
    const float base=CarriedGroundHeight(s,x,floor);
    const float height=(8+2*s->amplitude)*bank*(.25f+.75f*fminf(1,density));
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
  return GroundGrains(w,e,clip,project,context);
}
bool AppendAuthoredField(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,bool lighting,bool particles,
    ActionEffectProjectPointFn project,ActionEffectClipBoundsFn clip_fn,void *context) {
  ActionEffectInstance motion=*e;const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  if((e->kind==kActionEffect_AuthoredParticlePath || e->kind==kActionEffect_AuthoredParticleArea || e->kind==kActionEffect_AuthoredWater ||
      e->kind==kActionEffect_AuthoredDrips || e->kind==kActionEffect_AuthoredSpray ||
      e->kind==kActionEffect_AuthoredCloud) && !ActionAuthoredParticles_Valid(&e->particle_style))return false;
  if(s->placement>2 || !isfinite(s->amplitude)||s->amplitude<0||s->amplitude>32 ||
      !isfinite(s->drift)||fabsf(s->drift)>128||!isfinite(s->gust)||s->gust<0||s->gust>1||
      !isfinite(s->twinkle)||s->twinkle<0||s->twinkle>1)return false;
  if(e->kind==kActionEffect_AuthoredHeat)return true; /* Background texture sampling owns this effect. */
  if(e->kind==kActionEffect_AuthoredParticlePath) {
    const float pad=e->particle_style.wander+e->particle_style.size_max*2;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-pad,r->x1+pad,r->y1+pad};
  }
  if(e->kind==kActionEffect_AuthoredParticleArea) {
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-600,r->y0-600,r->x1+600,r->y1+600};
  } else if(e->kind==kActionEffect_AuthoredWater) {
    const float pad=e->particle_style.size_max*3;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-s->amplitude-pad,r->x1+pad,fmaxf(r->y1,r->y0+s->amplitude)+pad};
  } else if(e->kind==kActionEffect_AuthoredCloud) {
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-fabsf(s->drift),r->y0-s->amplitude,r->x1+fabsf(s->drift),r->y1+s->amplitude};
    if(s->point_count) motion.geometry.data.rect.y0=r->y0-(8+2*s->amplitude)*1.5f-e->particle_style.wander-e->particle_style.size_max*2;
  } else if(e->kind==kActionEffect_AuthoredFan) {
    const float extent=(r->x1-r->x0)*.5f+r->y1-r->y0;
    motion.geometry.data.rect=(ActionEffectLocalRect){-extent,-extent,extent,extent};
  }
  ActionEffectLocalRect visible=motion.geometry.data.rect;
  /* Free puffs test their animated footprint in AppendBillboard. */
  if(!(e->kind==kActionEffect_AuthoredCloud&&!s->point_count &&
      (w->source || project==ActionEffectProjection_ProjectPoint)) &&
      clip_fn&&!clip_fn(context,&motion,&visible))return true;
  switch(e->kind) {
    case kActionEffect_AuthoredExposure: {
      if(!lighting&&!particles)return true;
      if(!(e->tuning.dim_receivers&kActionReceiver_Scenery))return true;
      const ActionEffectInstance *boxes[]={e};
      return AppendExposureGroup(w,boxes,1,project,clip_fn,context);
    }
    case kActionEffect_AuthoredHalo: return !lighting || Halo(w,e,&visible,project,context);
    case kActionEffect_AuthoredGradient: return !lighting || LightGradient(w,e,&visible,project,context);
    case kActionEffect_AuthoredContour: return !particles || WetContour(w,e,&visible,project,context);
    case kActionEffect_AuthoredParticleArea: return !particles || ParticleArea(w,e,&visible,project,context);
    case kActionEffect_AuthoredParticlePath: return !particles || ParticlePath(w,e,&visible,project,context);
    case kActionEffect_AuthoredFan: {
      if (!lighting) return true;
      const int first = w->vertex_count;
      if (!LightFan(w, e, &visible, project, context)) return false;
      /* Fan vertices grow from the editor's anchor (local 0,0), not the
       * top of its symmetric bounds. A ceiling behind the source must not
       * shadow light already entering the chamber. */
      ActionSceneryShadow_Apply(w, first, e, 0, 0, false, 1, project, context);
      return true;
    }
    case kActionEffect_AuthoredWater: return !particles || WaterField(w,e,&visible,project,context);
    case kActionEffect_AuthoredDrips: case kActionEffect_AuthoredSpray:
      return !particles || FallingWater(w,e,&visible,project,context);
    case kActionEffect_AuthoredCloud: {
      if(!particles)return true;
      if(s->strands<1||s->strands>24||!isfinite(s->softness)||s->softness<0||s->softness>1)return false;
      if(!isfinite(s->carry)||s->carry<0||s->carry>512||(!s->point_count&&(s->carry>0||e->particle_count)))return false;
      if(s->point_count)return GroundCloud(w,e,&visible,project,context);
      const float width=r->x1-r->x0,height=r->y1-r->y0;
      const float phase=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
      for(unsigned i=0;i<s->strands;++i) {
        const uint32_t seed=e->generation^i*0x9e3779b9u;
        const float x=r->x0+width*(.2f+.6f*HashUnit(seed))+sinf(phase+HashUnit(seed^7)*6)*s->drift;
        const float y=r->y0+height*(.25f+.5f*HashUnit(seed^91))+cosf(phase+HashUnit(seed^23)*6)*s->amplitude;
        if(!AppendSceneSoftCloud(w,e,x,y,width*(.10f+.08f*HashUnit(seed^11)),height*.25f,
            FieldColor(e->tuning.color),fminf(1,e->tuning.intensity*(.06f+.12f*s->softness)),seed,project,context))return false;
      }
      return true;
    }
    default:return false;
  }
}
