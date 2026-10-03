/* Pure authoring kernels. All storage belongs to the caller's retained batch;
 * no native actor allocation, simulation state, texture or extra GPU resolve. */
#include "action_effect_render_internal.h"
#include "action_authored_particles.h"

static ArRenderColorF FieldColor(uint32_t color) {
  return (ArRenderColorF){((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
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
    default: break;
  }
  if (x+reach<clip->x0 || x-reach>clip->x1 || y+reach<clip->y0 || y-reach>clip->y1) return true;
  ArRenderColorF color=MixColor(FieldColor(e->tuning.color),FieldColor(s->color_end),t);
  color.a=fminf(1,e->tuning.intensity*.7f*sinf(t*3.1415927f));
  if(e->field_style.pattern==kActionParticle_Insects)color.a*=.5f+.5f*sinf(t*25+phase)*sinf(t*25+phase);
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
bool AppendAuthoredField(ActionEffectGeometryWriter *w,const ActionEffectInstance *e,bool lighting,bool particles,
    ActionEffectProjectPointFn project,ActionEffectClipBoundsFn clip_fn,void *context) {
  ActionEffectInstance motion=*e;const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const ActionEffectFieldStyle *s=&e->field_style;
  if((e->kind==kActionEffect_AuthoredParticleArea || e->kind==kActionEffect_AuthoredWater ||
      e->kind==kActionEffect_AuthoredDrips || e->kind==kActionEffect_AuthoredSpray) && !ActionAuthoredParticles_Valid(&e->particle_style))return false;
  if(s->placement>2 || !isfinite(s->amplitude)||s->amplitude<0||s->amplitude>32 ||
      !isfinite(s->drift)||fabsf(s->drift)>128)return false;
  if(e->kind==kActionEffect_AuthoredParticleArea) {
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-600,r->y0-600,r->x1+600,r->y1+600};
  } else if(e->kind==kActionEffect_AuthoredWater) {
    const float pad=e->particle_style.size_max*3;
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-pad,r->y0-s->amplitude-pad,r->x1+pad,fmaxf(r->y1,r->y0+s->amplitude)+pad};
  } else if(e->kind==kActionEffect_AuthoredCloud) {
    motion.geometry.data.rect=(ActionEffectLocalRect){r->x0-fabsf(s->drift),r->y0-s->amplitude,r->x1+fabsf(s->drift),r->y1+s->amplitude};
  } else if(e->kind==kActionEffect_AuthoredFan) {
    const float extent=(r->x1-r->x0)*.5f+r->y1-r->y0;
    motion.geometry.data.rect=(ActionEffectLocalRect){-extent,-extent,extent,extent};
  }
  ActionEffectLocalRect visible=motion.geometry.data.rect;
  if(clip_fn&&!clip_fn(context,&motion,&visible))return true;
  switch(e->kind) {
    case kActionEffect_AuthoredExposure: {
      if(!lighting&&!particles)return true;
      if(!(e->tuning.dim_receivers&kActionReceiver_Scenery))return true;
      if(e->tuning.intensity>1)return false;
      ActionEffectInstance mesh=*e;mesh.flags|=kActionEffectFlag_ClippedMesh;
      const float points[4][2]={{r->x0,r->y0},{r->x1,r->y0},{r->x1,r->y1},{r->x0,r->y1}};
      ArRenderVertex2D vertices[4];int mapped[4]={-1,-1,-1,-1};
      for(unsigned i=0;i<4;++i) {
        vertices[i]=(ArRenderVertex2D){.position={points[i][0],points[i][1]},
            .color={0,0,0,e->tuning.intensity}};
      }
      const int indices[]={0,1,2,0,2,3};
      for(unsigned t=0;t<6;t+=3)
        if(!AppendSceneClippedTriangle(w,&mesh,vertices,mapped,indices+t,&visible,project,context))return false;
      return true;
    }
    case kActionEffect_AuthoredHalo: return !lighting || Halo(w,e,&visible,project,context);
    case kActionEffect_AuthoredGradient: return !lighting || LightGradient(w,e,&visible,project,context);
    case kActionEffect_AuthoredContour: return !particles || WetContour(w,e,&visible,project,context);
    case kActionEffect_AuthoredParticleArea: return !particles || ParticleArea(w,e,&visible,project,context);
    case kActionEffect_AuthoredFan: {
      if (!lighting) return true;
      const int first = w->vertex_count;
      if (!LightFan(w, e, &visible, project, context)) return false;
      ActionSceneryShadow_Apply(w, first, e, 0, r->y0, false, project, context);
      return true;
    }
    case kActionEffect_AuthoredWater: return !particles || WaterField(w,e,&visible,project,context);
    case kActionEffect_AuthoredDrips: case kActionEffect_AuthoredSpray:
      return !particles || FallingWater(w,e,&visible,project,context);
    case kActionEffect_AuthoredCloud: {
      if(!particles)return true;
      if(s->strands<1||s->strands>24||!isfinite(s->softness)||s->softness<0||s->softness>1)return false;
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
