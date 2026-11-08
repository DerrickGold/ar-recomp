#include "action_ground_cover.h"
#include "deterministic_hash.h"
#include <math.h>
#include <string.h>

static float Falloff(float distance) {
  const float t=fmaxf(0,1-distance*distance);
  return t*t;
}
static ArRenderColorF Color(uint32_t color) {
  return (ArRenderColorF){((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,1};
}
static bool BankValid(const ActionGroundCoverBank *b) {
  return b&&b->point_count>=2&&b->point_count<=kActionContourMaxPoints&&
      b->depth>0&&b->depth<=12&&b->painted_width>0&&b->painted_width<=24;
}
static unsigned Segment(const ActionGroundCoverBank *b,float x) {
  unsigned i=1;
  while(i+1<b->point_count&&x>b->points[i].x)++i;
  return i;
}
static float Height(const ActionGroundCoverBank *b,float x) {
  const unsigned i=Segment(b,x);
  const ActionGroundCoverPoint a=b->points[i-1],z=b->points[i];
  const float t=fmaxf(0,fminf(1,(x-a.x)/(z.x-a.x)));
  return a.y+(z.y-a.y)*t;
}
static uint64_t KeyBytes(uint64_t key,const void *data,size_t length) {
  const unsigned char *bytes=data;
  for(size_t i=0;i<length;++i){key^=bytes[i];key*=UINT64_C(1099511628211);}
  return key;
}

bool ActionGroundCover_Prepare(const ActionEffectInstance *e,ActionGroundCoverBank *out) {
  if(!out)return false;
  memset(out,0,sizeof(*out));
  if(!e||e->kind!=kActionEffect_AuthoredCloud||!(e->flags&kActionEffectFlag_Visible)||
      e->render_layer!=kActionEffectRenderLayer_WorldDust||
      e->projection_plane!=kActionEffectProjectionPlane_Bg1||e->field_style.placement!=2)return false;
  const ActionEffectFieldStyle *s=&e->field_style;
  if(s->point_count<2||s->point_count>kActionContourMaxPoints||
      !isfinite(s->ground_depth)||s->ground_depth<=0||s->ground_depth>12||
      !isfinite(s->ground_width)||s->ground_width<=0||s->ground_width>24||
      !isfinite(s->wake)||s->wake<0||s->wake>1||
      !isfinite(e->tuning.intensity)||e->tuning.intensity<0)return false;
  ActionGroundCoverBank b={.point_count=s->point_count,.depth=s->ground_depth,
      .painted_width=s->ground_width,.intensity=e->tuning.intensity,.wake=s->wake,
      .body=Color(e->tuning.color),.crest=Color(e->particle_style.color_end)};
  /* Subdivide in local coordinates in the same order as GroundCloud. Adding
   * the native origin afterwards preserves its exact seeded knot profile. */
  for(unsigned i=0;i<s->point_count;++i) {
    if(!isfinite(s->points[i].x)||!isfinite(s->points[i].y)||
        (i&&s->points[i].x<=s->points[i-1].x))return false;
    b.points[i]=(ActionGroundCoverPoint){s->points[i].x,s->points[i].y};
  }
  while(b.point_count<9) {
    unsigned split=1;
    for(unsigned i=2;i<b.point_count;++i)
      if(b.points[i].x-b.points[i-1].x>b.points[split].x-b.points[split-1].x)split=i;
    for(unsigned i=b.point_count;i>split;--i)b.points[i]=b.points[i-1];
    b.points[split]=(ActionGroundCoverPoint){(b.points[split-1].x+b.points[split+1].x)*.5f,
        (b.points[split-1].y+b.points[split+1].y)*.5f};
    ++b.point_count;
  }
  for(unsigned i=0;i<b.point_count;++i) {
    b.points[i].x+=e->world_x;b.points[i].y+=e->world_y;
    if(!isfinite(b.points[i].x)||!isfinite(b.points[i].y)||
        (i&&b.points[i].x<=b.points[i-1].x))return false;
    b.height_noise[i]=DeterministicHash_Unit32(e->particle_style.seed^i*71);
  }
  /* Explicit fields, never padding or borrowed pointers, form this value key. */
  uint64_t key=UINT64_C(14695981039346656037);
#define KEY(field) key=KeyBytes(key,&b.field,sizeof(b.field))
  KEY(point_count);KEY(points);KEY(height_noise);KEY(depth);KEY(painted_width);
  KEY(intensity);KEY(wake);KEY(body);KEY(crest);
#undef KEY
  b.content_key=key;*out=b;return true;
}

bool ActionGroundCover_SameSupport(const ActionGroundCoverBank *a,const ActionGroundCoverBank *b) {
  return BankValid(a)&&BankValid(b)&&a->point_count==b->point_count&&
      !memcmp(a->points,b->points,a->point_count*sizeof(a->points[0]));
}

static float PressureFloor(const ActionGroundCoverBank *b,float x,
    const ActionGroundCoverPoint *from,const ActionGroundCoverPoint *to) {
  if(!from)return Height(b,x);
  const float t=fmaxf(0,fminf(1,(x-from->x)/(to->x-from->x)));
  return from->y+(to->y-from->y)*t;
}
static bool PreparePressure(const ActionGroundCoverBank *b,
    const ActionSceneEffectFrame *f,const ActionGroundCoverPoint *from,
    const ActionGroundCoverPoint *to,ActionGroundCoverPressure *out) {
  if(!out)return false;
  *out=(ActionGroundCoverPressure){0};
  if(!BankValid(b))return false;
  if(!f)return true;
  if(f->actor_count>kActionEffectActorMax)return false;
  const float left=b->points[0].x,right=b->points[b->point_count-1].x;
  for(unsigned i=0;i<f->actor_count;++i) {
    const ActionEffectActor *a=&f->actors[i];
    if(!a->visible||!a->body_radius)continue;
    const float radius=fmaxf(6,fminf(12,a->body_radius));
    if(from&&a->x+radius<fminf(from->x,to->x)&&a->wake.x+radius<fminf(from->x,to->x))continue;
    if(from&&a->x-radius>fmaxf(from->x,to->x)&&a->wake.x-radius>fmaxf(from->x,to->x))continue;
    float x=a->x,strength=Falloff((a->feet_y-PressureFloor(b,x,from,to))/(b->depth+2));
    if(x<left||x>right)strength=0;
    if(strength<=0&&a->wake.age<kActionActorWakeTicks) {
      x=a->wake.x;
      const float age=a->wake.age/(float)kActionActorWakeTicks;
      strength=.4f*b->wake*a->wake.strength*(1-age)*(1-age)*
          Falloff((a->wake.y-PressureFloor(b,x,from,to))/(b->depth+2));
    }
    if(x<left||x>right||!isfinite(strength)||strength<=0)continue;
    const unsigned slot=a->player?0:1;
    if(strength>out->feet[slot].strength)
      out->feet[slot]=(ActionGroundCoverFoot){x,radius,strength};
  }
  return true;
}
bool ActionGroundCover_PreparePressure(const ActionGroundCoverBank *b,
    const ActionSceneEffectFrame *f,ActionGroundCoverPressure *out) {
  return PreparePressure(b,f,NULL,NULL,out);
}
bool ActionGroundCover_PrepareSupportPressure(const ActionGroundCoverBank *b,
    const ActionSceneEffectFrame *f,ActionGroundCoverPoint from,ActionGroundCoverPoint to,
    ActionGroundCoverPressure *out) {
  if(!isfinite(from.x)||!isfinite(from.y)||!isfinite(to.x)||!isfinite(to.y)||fabsf(to.x-from.x)<.00001f) {
    if(out)*out=(ActionGroundCoverPressure){0};return false;
  }
  return PreparePressure(b,f,&from,&to,out);
}
static ActionGroundCoverFoot MappedFoot(const ActionGroundCoverBank *b,
    const ActionGroundCoverContactTrace *traces,unsigned count,float x,float y,float radius) {
  ActionGroundCoverFoot out={0};
  for(unsigned i=0;i<count;++i) {
    const ActionGroundCoverContactTrace *t=&traces[i];
    const float dx=t->to.x-t->from.x,dy=t->to.y-t->from.y,ds=t->source_to_x-t->source_from_x;
    const float length2=dx*dx+dy*dy;
    if(length2<.000001f||fabsf(ds)<.000001f)continue;
    const float u=((x-t->from.x)*dx+(y-t->from.y)*dy)/length2;
    const float nearest=fmaxf(0,fminf(1,u));
    const float ex=x-(t->from.x+nearest*dx),ey=y-(t->from.y+nearest*dy);
    if(ex*ex+ey*ey>(radius+b->depth+2)*(radius+b->depth+2))continue;
    const float px=t->from.x+u*dx,py=t->from.y+u*dy;
    const float sx=t->source_from_x+u*ds;
    if(sx<b->points[0].x||sx>b->points[b->point_count-1].x)continue;
    const float distance2=(x-px)*(x-px)+(y-py)*(y-py);
    const float falloff=fmaxf(0,1-distance2/((b->depth+2)*(b->depth+2)));
    const float strength=falloff*falloff;
    if(strength>out.strength)out=(ActionGroundCoverFoot){sx,
        fminf(4096,radius*fabsf(ds)/sqrtf(length2)),strength};
  }
  return out;
}
bool ActionGroundCover_PrepareMappedPressure(const ActionGroundCoverBank *b,
    const ActionSceneEffectFrame *f,const ActionGroundCoverContactTrace *traces,unsigned count,
    ActionGroundCoverPressure *out) {
  if(!out)return false;*out=(ActionGroundCoverPressure){0};
  if(!BankValid(b)||count>2||(count&&!traces))return false;
  for(unsigned i=0;i<count;++i) {
    const ActionGroundCoverContactTrace t=traces[i];
    if(!isfinite(t.from.x)||!isfinite(t.from.y)||!isfinite(t.to.x)||!isfinite(t.to.y)||
        !isfinite(t.source_from_x)||!isfinite(t.source_to_x))return false;
  }
  if(!f||!count)return true;
  if(f->actor_count>kActionEffectActorMax)return false;
  for(unsigned i=0;i<f->actor_count;++i) {
    const ActionEffectActor *actor=&f->actors[i];if(!actor->visible||!actor->body_radius)continue;
    const float radius=fmaxf(6,fminf(12,actor->body_radius));
    ActionGroundCoverFoot foot=MappedFoot(b,traces,count,actor->x,actor->feet_y,radius);
    if(foot.strength<=0&&actor->wake.age<kActionActorWakeTicks) {
      foot=MappedFoot(b,traces,count,actor->wake.x,actor->wake.y,radius);
      const float age=actor->wake.age/(float)kActionActorWakeTicks;
      foot.strength*=.4f*b->wake*actor->wake.strength*(1-age)*(1-age);
    }
    const unsigned slot=actor->player?0:1;
    if(isfinite(foot.strength)&&foot.strength>out->feet[slot].strength)out->feet[slot]=foot;
  }
  return true;
}

bool ActionGroundCover_ClipSupport(const ActionGroundCoverBank *b,
    ActionGroundCoverPoint from,ActionGroundCoverPoint to,float tolerance,
    ActionGroundCoverInterval *out,unsigned capacity,unsigned *count) {
  if(!count)return false;
  *count=0;
  if(!BankValid(b)||!isfinite(from.x)||!isfinite(from.y)||!isfinite(to.x)||!isfinite(to.y)||
      !isfinite(tolerance)||tolerance<0||tolerance>ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE||
      fabsf(to.x-from.x)<.00001f)return false;
  ActionGroundCoverInterval intervals[kActionGroundCoverMaxIntervals];unsigned n=0;
  const float dx=to.x-from.x,dy=to.y-from.y;
  for(unsigned i=1;i<b->point_count;++i) {
    const ActionGroundCoverPoint a=b->points[i-1],z=b->points[i];
    float ta=(a.x-from.x)/dx,tb=(z.x-from.x)/dx;
    if(ta>tb){const float tmp=ta;ta=tb;tb=tmp;}
    float lo=fmaxf(0,ta),hi=fminf(1,tb);
    if(hi<=lo)continue;
    const float slope=(z.y-a.y)/(z.x-a.x);
    const float error=from.y-a.y-(from.x-a.x)*slope,rate=dy-dx*slope;
    if(fabsf(rate)<.000001f) {if(fabsf(error)>tolerance)continue;}
    else {
      float ea=(-tolerance-error)/rate,eb=(tolerance-error)/rate;
      if(ea>eb){const float tmp=ea;ea=eb;eb=tmp;}
      lo=fmaxf(lo,ea);hi=fminf(hi,eb);
      if(hi<=lo)continue;
    }
    intervals[n++]=(ActionGroundCoverInterval){lo,hi};
  }
  if(n>capacity||(n&&!out))return false;
  for(unsigned i=0;i<n;++i)out[i]=intervals[dx>0?i:n-i-1];
  *count=n;return true;
}

bool ActionGroundCover_ContactCuts(const ActionGroundCoverPressure *pressure,
    float from_x,float to_x,ActionGroundCoverInterval support,
    float *out,unsigned capacity,unsigned *count) {
  if(!count)return false;
  *count=0;
  if(!isfinite(from_x)||!isfinite(to_x)||fabsf(to_x-from_x)<.00001f||
      !isfinite(support.t0)||!isfinite(support.t1)||support.t0<0||support.t1>1||
      support.t1<=support.t0)return false;
  float cuts[kActionGroundCoverMaxContactCuts]={support.t0,support.t1};unsigned n=2;
  if(pressure)for(unsigned i=0;i<kActionFieldWakeCount;++i) {
    const ActionGroundCoverFoot p=pressure->feet[i];
    if(!isfinite(p.x)||!isfinite(p.radius)||!isfinite(p.strength)||p.radius<=0||p.strength<=0)continue;
    for(int offset=-1;offset<=1;++offset) {
      const float t=(p.x+offset*p.radius-from_x)/(to_x-from_x);
      if(t<=support.t0||t>=support.t1)continue;
      unsigned at=0;while(at<n&&cuts[at]<t)++at;
      if((at<n&&fabsf(cuts[at]-t)<.00001f)||(at&&fabsf(cuts[at-1]-t)<.00001f))continue;
      for(unsigned j=n;j>at;--j)cuts[j]=cuts[j-1];
      cuts[at]=t;++n;
    }
  }
  if(n>capacity||!out)return false;
  memcpy(out,cuts,n*sizeof(*out));*count=n;return true;
}

bool ActionGroundCover_Sample(const ActionGroundCoverBank *b,const ActionGroundCoverPressure *pressure,
    float x,float depth_fraction,float contact_weight,ActionGroundCoverSample *out) {
  if(!out)return false;
  *out=(ActionGroundCoverSample){0};
  if(!BankValid(b)||!isfinite(x)||!isfinite(depth_fraction)||depth_fraction<0||depth_fraction>1||
      !isfinite(contact_weight)||contact_weight<0||contact_weight>1||
      x<b->points[0].x||x>b->points[b->point_count-1].x)return false;
  const unsigned i=Segment(b,x);
  const float t=(x-b->points[i-1].x)/(b->points[i].x-b->points[i-1].x);
  const float edge=fminf(1,fminf(x-b->points[0].x,b->points[b->point_count-1].x-x)/12);
  const float noise=b->height_noise[i-1]+(b->height_noise[i]-b->height_noise[i-1])*t;
  const float settled=b->depth*(.82f+.18f*noise)*edge;
  float compression=0;
  if(pressure)for(unsigned j=0;j<kActionFieldWakeCount;++j) {
    const ActionGroundCoverFoot p=pressure->feet[j];
    if(!isfinite(p.x)||!isfinite(p.radius)||!isfinite(p.strength)||p.radius<=0||p.strength<=0)continue;
    compression=fmaxf(compression,p.strength*Falloff((x-p.x)/p.radius));
  }
  compression=fminf(1,compression)*contact_weight;
  const float middle=1-fminf(.8f,b->painted_width*.25f)/b->painted_width;
  const float base_alpha=fminf(.98f,b->intensity*.75f)*edge;
  const float weight=depth_fraction<=middle?depth_fraction/middle:1;
  const float alpha=depth_fraction<=middle?.78f+(.58f-.78f)*weight:
      .58f*(1-depth_fraction)/(1-middle);
  *out=(ActionGroundCoverSample){.native_floor_y=Height(b,x),.settled_height=settled,
      .height=settled*(1-.35f*compression),.compression=compression,.edge=edge,
      .body=b->body,.crest=b->crest,.crest_mix=1+(.45f-1)*weight,.alpha=base_alpha*alpha};
  return true;
}

ActionGroundCoverClaim ActionGroundCover_ResolveClaim(bool ready,bool prepared,bool ok,unsigned count) {
  if(!ready||!prepared)return kActionGroundCoverUnsupported;
  if(!ok)return kActionGroundCoverFailure;
  return count?kActionGroundCoverClaimedGeometry:kActionGroundCoverClaimedEmpty;
}
bool ActionGroundCover_SuppressesFlatTop(ActionGroundCoverClaim claim) {
  return claim==kActionGroundCoverClaimedEmpty||claim==kActionGroundCoverClaimedGeometry||
      claim==kActionGroundCoverFailure;
}
