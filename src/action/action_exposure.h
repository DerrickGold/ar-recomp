#ifndef AR_ACTION_EXPOSURE_H
#define AR_ACTION_EXPOSURE_H
#include "action_effects.h"
#include "render/scenery_shading.h"
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
  /* Every box belongs to one brightness group; pack groups contiguously rather
   * than reserving a full room's capacity for each group. */
  const ActionEffectInstance *boxes[kActionAuthoredMaxInstances];
  unsigned offsets[kActionAuthoredMaxInstances],counts[kActionAuthoredMaxInstances],count;
  float origin_x,origin_y,width,height;
  float xs[kActionAuthoredMaxInstances*kActionExposureAxisPoints];
  float ys[kActionAuthoredMaxInstances*kActionExposureAxisPoints];
  SceneryShadingField field;
} ActionExposureLighting;
static inline unsigned ActionExposure_Unique(float *values,unsigned count);
static inline void ActionExposure_AxisRange(float low,float high,float size,float near,float far,float *min,float *max) {
  if(high<0||low>size){*min=*max=0;return;}
  ActionExposure_Fades(size,&near,&far);
  const float a=ActionExposure_Axis(fmaxf(0,low),size,near,far);
  const float b=ActionExposure_Axis(fminf(size,high),size,near,far);
  *min=low<0||high>size?0:fminf(a,b);
  *max=high>=near&&low<=size-far?1:fmaxf(a,b);
}
static inline bool ActionExposure_Uniform(void *context,float u0,float v0,float u1,float v1,float *shade) {
  const ActionExposureLighting *light=context;
  const float x0=light->origin_x+u0*light->width,y0=light->origin_y+v0*light->height;
  const float x1=light->origin_x+u1*light->width,y1=light->origin_y+v1*light->height;
  float min=1,max=1;
  for(unsigned g=0;g<light->count;++g) {
    const ActionEffectInstance *const *boxes=light->boxes+light->offsets[g];
    const bool dark=ActionExposure_Brightness(boxes[0])<1;
    float group_min=1,group_max=1;
    for(unsigned i=0;i<light->counts[g];++i) {
      const ActionEffectInstance *e=boxes[i];
      const ActionEffectLocalRect *r=&e->geometry.data.rect;const float *fade=e->field_style.fade;
      float ax,bx,ay,by;
      ActionExposure_AxisRange(x0-e->world_x-r->x0,x1-e->world_x-r->x0,r->x1-r->x0,fade[0],fade[2],&ax,&bx);
      ActionExposure_AxisRange(y0-e->world_y-r->y0,y1-e->world_y-r->y0,r->y1-r->y0,fade[1],fade[3],&ay,&by);
      const float delta=ActionExposure_Brightness(e)-1;
      const float a=1+delta*ax*ay,b=1+delta*bx*by;
      group_min=dark?fminf(group_min,b):fmaxf(group_min,a);
      group_max=dark?fminf(group_max,a):fmaxf(group_max,b);
    }
    min*=group_min;max*=group_max;
  }
  *shade=min;
  return min==max;
}
static inline float ActionExposure_Cell(void *context,float u,float v,float inside_u,float inside_v) {
  const ActionExposureLighting *light=context;
  const float x=light->origin_x+u*light->width,y=light->origin_y+v*light->height;
  const float mx=light->origin_x+inside_u*light->width,my=light->origin_y+inside_v*light->height;
  float factor=1;
  for(unsigned g=0;g<light->count;++g) {
    const ActionEffectInstance *const *boxes=light->boxes+light->offsets[g];
    const bool dark=ActionExposure_Brightness(boxes[0])<1;
    float group=1;
    for(unsigned i=0;i<light->counts[g];++i) {
      const ActionEffectInstance *e=boxes[i];
      const ActionEffectLocalRect *r=&e->geometry.data.rect;
      if(mx<e->world_x+r->x0||mx>e->world_x+r->x1||
          my<e->world_y+r->y0||my>e->world_y+r->y1)continue;
      /* The cell selects the inside of a hard edge. Clamp vertex roundoff at
       * that edge instead of accidentally sampling the outside brightness. */
      const float f=ActionExposure_Factor(e,
          fmaxf(r->x0,fminf(r->x1,x-e->world_x)),
          fmaxf(r->y0,fminf(r->y1,y-e->world_y)));
      group=dark?fminf(group,f):fmaxf(group,f);
    }
    factor*=group;
  }
  return factor;
}
static inline void ActionExposure_Prepare(ActionExposureLighting *out,const ActionSceneEffectFrame *frame) {
  out->count=0;
  out->field=(SceneryShadingField){.x=out->xs,.y=out->ys,.sample=ActionExposure_Cell,
      .userdata=out,.uniform=ActionExposure_Uniform};
  if(!frame||frame->authored_count>kActionAuthoredMaxInstances||out->width<=0||out->height<=0)return;
  unsigned membership[kActionAuthoredMaxInstances];
  for(unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    membership[i]=kActionAuthoredMaxInstances;
    if(!ActionExposure_Matches(e,e,kActionReceiver_Scenery)||ActionExposure_Brightness(e)==1)continue;
    unsigned g=0;while(g<out->count&&!ActionExposure_Matches(e,out->boxes[g],kActionReceiver_Scenery))++g;
    if(g==out->count){out->boxes[g]=e;out->counts[g]=0;++out->count;}
    membership[i]=g;++out->counts[g];
    const ActionEffectLocalRect *r=&e->geometry.data.rect;
    const float *f=e->field_style.fade;
    out->field.nx+=ActionExposure_AxisPoints(e->world_x+r->x0,e->world_x+r->x1,f[0],f[2],out->xs+out->field.nx);
    out->field.ny+=ActionExposure_AxisPoints(e->world_y+r->y0,e->world_y+r->y1,f[1],f[3],out->ys+out->field.ny);
  }
  unsigned total=0;
  for(unsigned g=0;g<out->count;++g) {
    out->offsets[g]=total;total+=out->counts[g];out->counts[g]=0;
  }
  for(unsigned i=0;i<frame->authored_count;++i) {
    const unsigned g=membership[i];
    if(g<out->count)out->boxes[out->offsets[g]+out->counts[g]++]=&frame->authored[i];
  }
  out->field.nx=ActionExposure_Unique(out->xs,out->field.nx);
  out->field.ny=ActionExposure_Unique(out->ys,out->field.ny);
  for(unsigned i=0;i<out->field.nx;++i)out->xs[i]=(out->xs[i]-out->origin_x)/out->width;
  for(unsigned i=0;i<out->field.ny;++i)out->ys[i]=(out->ys[i]-out->origin_y)/out->height;
}
static inline float ActionExposure_Scenery(void *context,float u,float v) {
  const ActionExposureLighting *light=context;float factor=1;
  const float x=light->origin_x+u*light->width,y=light->origin_y+v*light->height;
  for(unsigned i=0;i<light->count;++i)factor*=ActionExposure_Union(light->boxes+light->offsets[i],light->counts[i],x,y);
  return factor;
}
typedef bool (*ActionExposureQuadFn)(void *context,const float xs[2],const float ys[2],const float factors[4]);
static inline unsigned ActionExposure_Unique(float *values,unsigned count) {
  // The bounded coordinate list needs no library scratch storage.
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
