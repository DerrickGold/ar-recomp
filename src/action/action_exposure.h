#ifndef AR_ACTION_EXPOSURE_H
#define AR_ACTION_EXPOSURE_H
#include "action_effects.h"
#include <math.h>

/* Fixed-size tessellation, independent of box size and viewport resolution.
 * Four intervals per feather bound the corner interpolation error to 1/64
 * of the brightness change. Opposing fades shrink together on small boxes. */
enum { kActionExposureFadeSteps=4, kActionExposureAxisPoints=10 };
static inline float ActionExposure_Brightness(const ActionEffectInstance *e) {
  return e->field_style.brightness_set ? e->field_style.brightness : 1-e->tuning.intensity;
}
static inline void ActionExposure_Fades(float size,float *near,float *far) {
  const float sum=*near+*far;
  if(sum>size){*near*=size/sum;*far*=size/sum;}
}
static inline float ActionExposure_Axis(float p,float size,float near,float far) {
  if(p<0||p>size)return 0;
  ActionExposure_Fades(size,&near,&far);
  return fmaxf(0,fminf(1,fminf(near>0?p/near:1,far>0?(size-p)/far:1)));
}
static inline float ActionExposure_Factor(const ActionEffectInstance *e,float x,float y) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float *fade=e->field_style.fade;
  const float weight=ActionExposure_Axis(x-r->x0,r->x1-r->x0,fade[0],fade[2])*
      ActionExposure_Axis(y-r->y0,r->y1-r->y0,fade[1],fade[3]);
  return 1+(ActionExposure_Brightness(e)-1)*weight;
}
static inline unsigned ActionExposure_AxisPoints(float low,float high,float near,float far,float *points) {
  ActionExposure_Fades(high-low,&near,&far);
  unsigned n=0;points[n++]=low;
  if(near>0)for(unsigned i=1;i<=kActionExposureFadeSteps;++i)points[n++]=low+near*i/kActionExposureFadeSteps;
  if(high-far>points[n-1])points[n++]=high-far;
  if(far>0)for(unsigned i=1;i<=kActionExposureFadeSteps;++i)points[n++]=high-far+far*i/kActionExposureFadeSteps;
  return n;
}
static inline bool ActionExposure_Matches(const ActionEffectInstance *a,const ActionEffectInstance *b,unsigned receiver) {
  return b->kind==kActionEffect_AuthoredExposure && (b->flags&kActionEffectFlag_Visible) &&
      (b->tuning.dim_receivers&receiver) && roundf(ActionExposure_Brightness(a)*1000000)==roundf(ActionExposure_Brightness(b)*1000000);
}
static inline float ActionExposure_Union(const ActionEffectInstance *const *boxes,unsigned count,float x,float y) {
  const bool dark=ActionExposure_Brightness(boxes[0])<1;float factor=1;
  for(unsigned i=0;i<count;++i) {
    const float f=ActionExposure_Factor(boxes[i],x-boxes[i]->world_x,y-boxes[i]->world_y);
    factor=dark?fminf(factor,f):fmaxf(factor,f);
  }
  return factor;
}
/* Prepared once per composite; per-vertex sampling visits each box once.
 * The native scenery mesh carries exposure on all BG1 depth bands, preserving
 * texture alpha and leaving objects/other backgrounds untouched. */
typedef struct ActionExposureLighting {
  const ActionEffectInstance *groups[kActionAuthoredMaxInstances][kActionAuthoredMaxInstances];
  unsigned counts[kActionAuthoredMaxInstances],count;
  float origin_x,origin_y,width,height;
} ActionExposureLighting;
static inline void ActionExposure_Prepare(ActionExposureLighting *out,const ActionSceneEffectFrame *frame) {
  out->count=0;
  if(!frame||frame->authored_count>kActionAuthoredMaxInstances)return;
  for(unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    if(!ActionExposure_Matches(e,e,kActionReceiver_Scenery)||ActionExposure_Brightness(e)==1)continue;
    unsigned g=0;while(g<out->count&&!ActionExposure_Matches(e,out->groups[g][0],kActionReceiver_Scenery))++g;
    if(g==out->count){out->counts[g]=0;++out->count;}
    out->groups[g][out->counts[g]++]=e;
  }
}
static inline float ActionExposure_Scenery(void *context,float u,float v) {
  const ActionExposureLighting *light=context;float factor=1;
  const float x=light->origin_x+u*light->width,y=light->origin_y+v*light->height;
  for(unsigned i=0;i<light->count;++i)factor*=ActionExposure_Union(light->groups[i],light->counts[i],x,y);
  return factor;
}
typedef bool (*ActionExposureQuadFn)(void *context,const float xs[2],const float ys[2],const float factors[4]);
static inline unsigned ActionExposure_Unique(float *values,unsigned count) {
  // At most 160 coordinates; insertion sort needs no library scratch storage.
  for(unsigned i=1;i<count;++i) {
    const float v=values[i];unsigned j=i;
    while(j&&values[j-1]>v){values[j]=values[j-1];--j;}values[j]=v;
  }
  unsigned n=0;for(unsigned i=0;i<count;++i)if(!n||values[i]>values[n-1])values[n++]=values[i];return n;
}
/* Disjoint cells represent the union once, rather than layering alpha quads.
 * Box and feather boundaries are world-anchored, so scrolling cannot move the
 * tessellation. No allocations, render targets, pixel scans or readbacks. */
static inline bool ActionExposure_Visit(const ActionEffectInstance *const *boxes,unsigned count,
    const ActionEffectLocalRect *clip,ActionExposureQuadFn emit,void *context) {
  float xs[kActionAuthoredMaxInstances*kActionExposureAxisPoints],ys[kActionAuthoredMaxInstances*kActionExposureAxisPoints];
  unsigned nx=0,ny=0;
  if(!count)return true;
  if(count>kActionAuthoredMaxInstances)return false;
  for(unsigned i=0;i<count;++i) {
    const ActionEffectInstance *e=boxes[i];const ActionEffectLocalRect *r=&e->geometry.data.rect;const float *f=e->field_style.fade;
    nx+=ActionExposure_AxisPoints(e->world_x+r->x0,e->world_x+r->x1,f[0],f[2],xs+nx);
    ny+=ActionExposure_AxisPoints(e->world_y+r->y0,e->world_y+r->y1,f[1],f[3],ys+ny);
  }
  nx=ActionExposure_Unique(xs,nx);ny=ActionExposure_Unique(ys,ny);
  for(unsigned y=0;y+1<ny;++y)for(unsigned x=0;x+1<nx;++x) {
    if(clip&&(xs[x+1]<=clip->x0||xs[x]>=clip->x1||ys[y+1]<=clip->y0||ys[y]>=clip->y1))continue;
    const float mx=(xs[x]+xs[x+1])*.5f,my=(ys[y]+ys[y+1])*.5f;
    const ActionEffectInstance *active[kActionAuthoredMaxInstances];unsigned n=0;
    for(unsigned i=0;i<count;++i) {
      const ActionEffectInstance *e=boxes[i];const ActionEffectLocalRect *r=&e->geometry.data.rect;
      if(mx>e->world_x+r->x0&&mx<e->world_x+r->x1&&my>e->world_y+r->y0&&my<e->world_y+r->y1)active[n++]=e;
    }
    if(!n)continue;
    float f[4]={ActionExposure_Union(active,n,xs[x],ys[y]),ActionExposure_Union(active,n,xs[x+1],ys[y]),
      ActionExposure_Union(active,n,xs[x+1],ys[y+1]),ActionExposure_Union(active,n,xs[x],ys[y+1])};
    // Refine a crossover between two feathers; otherwise keep the small
    // original mesh. The centre test also detects opposing linear ramps.
    const float middle=ActionExposure_Union(active,n,mx,my);
    const unsigned steps=n>1&&fabsf(middle-(f[0]+f[1]+f[2]+f[3])*.25f)>.008f?2:1;
    for(unsigned v=0;v<steps;++v)for(unsigned u=0;u<steps;++u) {
      const float xx[2]={xs[x]+(xs[x+1]-xs[x])*u/steps,xs[x]+(xs[x+1]-xs[x])*(u+1)/steps};
      const float yy[2]={ys[y]+(ys[y+1]-ys[y])*v/steps,ys[y]+(ys[y+1]-ys[y])*(v+1)/steps};
      if(steps>1) {f[0]=ActionExposure_Union(active,n,xx[0],yy[0]);f[1]=ActionExposure_Union(active,n,xx[1],yy[0]);
        f[2]=ActionExposure_Union(active,n,xx[1],yy[1]);f[3]=ActionExposure_Union(active,n,xx[0],yy[1]);}
      if(!emit(context,xx,yy,f))return false;
    }
  }
  return true;
}

#endif
