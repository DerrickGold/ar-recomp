#ifndef AR_SCENERY_SHADING_H
#define AR_SCENERY_SHADING_H

#include "render_types.h"
#include <math.h>
#include <string.h>

/* A piecewise brightness field in source UVs. Sorted cuts mark authored edges
 * and feather intervals. The interior sample selects the side of a hard edge;
 * vertices on that edge may legitimately have different colors in each cell. */
typedef struct SceneryShadingField {
  const float *x, *y;
  unsigned nx, ny;
  float (*sample)(void *, float u, float v, float inside_u, float inside_v);
  void *userdata;
  /* Optional exact bound: omit cuts obscured by a stronger overlapping zone. */
  bool (*uniform)(void *, float u0, float v0, float u1, float v1, float *shade);
} SceneryShadingField;
typedef bool (*SceneryShadingSubmit)(void *, const ArRenderVertex2D *, int,
    const int32_t *, int);
/* Optional parallel normalized GPU depths. Both clipping and uniform-region
 * fast paths preserve their correspondence to the submitted vertices. */
typedef bool (*SceneryShadingDepthSubmit)(void *, const ArRenderVertex2D *,
    const float *, int, const int32_t *, int);

enum { kSceneryShadingBatchVertices = 4096, kSceneryShadingBatchIndices = 12288 };
typedef struct SceneryShadingBatch {
  ArRenderVertex2D vertices[kSceneryShadingBatchVertices];
  float depths[kSceneryShadingBatchVertices];
  int32_t indices[kSceneryShadingBatchIndices];
  int nv, ni;
  SceneryShadingDepthSubmit submit;
  bool has_depth;
  void *context;
} SceneryShadingBatch;

static inline bool SceneryShading_Flush(SceneryShadingBatch *b) {
  if (b->ni && !b->submit(b->context,b->vertices,b->has_depth?b->depths:NULL,
      b->nv,b->indices,b->ni)) return false;
  b->nv=b->ni=0;
  return true;
}
static inline float SceneryShading_Axis(ArRenderVertex2D v, unsigned axis) {
  return axis ? v.tex_coord.y : v.tex_coord.x;
}
static inline ArRenderVertex2D SceneryShading_Intersect(
    ArRenderVertex2D a, ArRenderVertex2D b, unsigned axis, float edge,
    float za,float zb,float *depth) {
  /* Canonical edge order makes shared intersections identical. */
  if (a.tex_coord.x>b.tex_coord.x ||
      (a.tex_coord.x==b.tex_coord.x && a.tex_coord.y>b.tex_coord.y)) {
    const ArRenderVertex2D swap=a;a=b;b=swap;
    const float swap_z=za;za=zb;zb=swap_z;
  }
  const float t=(edge-SceneryShading_Axis(a,axis))/
      (SceneryShading_Axis(b,axis)-SceneryShading_Axis(a,axis));
  ArRenderVertex2D v={
    .position={a.position.x+(b.position.x-a.position.x)*t,a.position.y+(b.position.y-a.position.y)*t},
    .tex_coord={a.tex_coord.x+(b.tex_coord.x-a.tex_coord.x)*t,a.tex_coord.y+(b.tex_coord.y-a.tex_coord.y)*t},
    .color={a.color.r+(b.color.r-a.color.r)*t,a.color.g+(b.color.g-a.color.g)*t,
      a.color.b+(b.color.b-a.color.b)*t,a.color.a+(b.color.a-a.color.a)*t}};
  if(axis)v.tex_coord.y=edge;else v.tex_coord.x=edge;
  if(depth)*depth=za+(zb-za)*t;
  return v;
}
static inline unsigned SceneryShading_Clip(const ArRenderVertex2D *in,const float *depths,unsigned n,
    ArRenderVertex2D out[8],float *out_depths,unsigned axis,float edge,bool lower) {
  unsigned count=0;
  for(unsigned i=0;i<n;++i) {
    const ArRenderVertex2D a=in[i],b=in[(i+1)%n];
    const float av=SceneryShading_Axis(a,axis),bv=SceneryShading_Axis(b,axis);
    const bool ai=lower?av>=edge:av<=edge,bi=lower?bv>=edge:bv<=edge;
    /* Endpoints on the cut are already emitted by the adjacent edge. */
    if(ai!=bi && av!=edge && bv!=edge) {
      out[count]=SceneryShading_Intersect(a,b,axis,edge,
          depths?depths[i]:0,depths?depths[(i+1)%n]:0,out_depths?out_depths+count:NULL);
      ++count;
    }
    if(bi) {
      out[count]=b;if(out_depths)out_depths[count]=depths[(i+1)%n];++count;
    }
  }
  return count;
}
static inline bool SceneryShading_Polygon(SceneryShadingBatch *b,
    const SceneryShadingField *field,ArRenderRectF uv,ArRenderVertex2D *v,const float *depths,unsigned n,
    float inside_x,float inside_y,float uniform) {
  if(n<3)return true;
  if(b->nv+(int)n>kSceneryShadingBatchVertices ||
      b->ni+(int)(n-2)*3>kSceneryShadingBatchIndices)
    if(!SceneryShading_Flush(b))return false;
  const int base=b->nv;
  for(unsigned i=0;i<n;++i) {
    const float shade=uniform>=0?uniform:field->sample(field->userdata,
        uv.x+v[i].tex_coord.x*uv.w,uv.y+v[i].tex_coord.y*uv.h,
        uv.x+inside_x*uv.w,uv.y+inside_y*uv.h);
    v[i].color.r*=shade;v[i].color.g*=shade;v[i].color.b*=shade;
    if(b->has_depth)b->depths[b->nv]=depths[i];
    b->vertices[b->nv++]=v[i];
  }
  for(unsigned i=1;i+1<n;++i) {
    b->indices[b->ni++]=base;b->indices[b->ni++]=base+(int)i;b->indices[b->ni++]=base+(int)i+1;
  }
  return true;
}
static inline unsigned SceneryShading_First(const float *cuts,unsigned n,float value,
    float offset,float scale) {
  unsigned low=0,high=n;
  while(low<high){const unsigned mid=low+(high-low)/2;if((cuts[mid]-offset)/scale<=value)low=mid+1;else high=mid;}
  return low;
}
/* Split only triangles crossed by authored boundaries. Positions, source UVs,
 * alpha, painter order and active material remain intact. Fixed scratch batches
 * stream arbitrarily complex zones without allocations or extra render targets. */
static inline bool SceneryShading_DrawDepth(const SceneryShadingField *field,ArRenderRectF uv,
    const ArRenderVertex2D *vertices,const float *depths,int nv,const int32_t *indices,int ni,
    SceneryShadingDepthSubmit submit,void *context) {
  if(!field || !field->sample)return submit(context,vertices,depths,nv,indices,ni);
  if(uv.w<=0 || uv.h<=0)return false;
  SceneryShadingBatch batch;
  batch.nv=batch.ni=0;batch.submit=submit;batch.context=context;batch.has_depth=depths!=NULL;
  /* Most draws are entirely inside/outside a room-wide box. Keep their indexed
   * mesh intact and sample each vertex once; no clipping or extra draw calls. */
  if(nv>0 && nv<=kSceneryShadingBatchVertices) {
    float x0=vertices[0].tex_coord.x,x1=x0,y0=vertices[0].tex_coord.y,y1=y0;
    for(int i=1;i<nv;++i) {
      x0=fminf(x0,vertices[i].tex_coord.x);x1=fmaxf(x1,vertices[i].tex_coord.x);
      y0=fminf(y0,vertices[i].tex_coord.y);y1=fmaxf(y1,vertices[i].tex_coord.y);
    }
    const unsigned x=SceneryShading_First(field->x,field->nx,x0,uv.x,uv.w);
    const unsigned y=SceneryShading_First(field->y,field->ny,y0,uv.y,uv.h);
    float shade=-1;
    if(field->uniform && !field->uniform(field->userdata,
        uv.x+x0*uv.w,uv.y+y0*uv.h,uv.x+x1*uv.w,uv.y+y1*uv.h,&shade))shade=-1;
    if(shade>=0 || ((x==field->nx || (field->x[x]-uv.x)/uv.w>=x1) &&
        (y==field->ny || (field->y[y]-uv.y)/uv.h>=y1))) {
      const float mx=uv.x+(x0+x1)*.5f*uv.w,my=uv.y+(y0+y1)*.5f*uv.h;
      memcpy(batch.vertices,vertices,(size_t)nv*sizeof(*vertices));
      for(int i=0;i<nv;++i) {
        ArRenderVertex2D *v=&batch.vertices[i];
        const float factor=shade>=0?shade:field->sample(field->userdata,
            uv.x+v->tex_coord.x*uv.w,uv.y+v->tex_coord.y*uv.h,mx,my);
        v->color.r*=factor;v->color.g*=factor;v->color.b*=factor;
      }
      return submit(context,batch.vertices,depths,nv,indices,ni);
    }
  }
  for(int i=0;i<ni;i+=3) {
    ArRenderVertex2D triangle[3]={vertices[indices[i]],vertices[indices[i+1]],vertices[indices[i+2]]};
    float triangle_z[3]={0};
    if(depths)for(unsigned j=0;j<3;++j)triangle_z[j]=depths[indices[i+j]];
    float x0=triangle[0].tex_coord.x,x1=x0,y0=triangle[0].tex_coord.y,y1=y0;
    for(unsigned j=1;j<3;++j){x0=fminf(x0,triangle[j].tex_coord.x);x1=fmaxf(x1,triangle[j].tex_coord.x);
      y0=fminf(y0,triangle[j].tex_coord.y);y1=fmaxf(y1,triangle[j].tex_coord.y);}
    float shade;
    if(field->uniform && field->uniform(field->userdata,
        uv.x+x0*uv.w,uv.y+y0*uv.h,uv.x+x1*uv.w,uv.y+y1*uv.h,&shade)) {
      if(!SceneryShading_Polygon(&batch,field,uv,triangle,triangle_z,3,0,0,shade))return false;
      continue;
    }
    const unsigned first_x=SceneryShading_First(field->x,field->nx,x0,uv.x,uv.w);
    const unsigned first_y=SceneryShading_First(field->y,field->ny,y0,uv.y,uv.h);
    float bottom=y0;
    for(unsigned row=first_y;;++row) {
      const float top=bottom;
      bottom=row<field->ny?fminf(y1,(field->y[row]-uv.y)/uv.h):y1;
      if(bottom<=top && y0<y1)continue;
      float right=x0;
      for(unsigned col=first_x;;++col) {
        const float left=right;
        right=col<field->nx?fminf(x1,(field->x[col]-uv.x)/uv.w):x1;
        if(right<=left && x0<x1)continue;
        ArRenderVertex2D a[8],b[8];float za[8],zb[8];
        for(unsigned j=0;j<3;++j){a[j]=triangle[j];za[j]=triangle_z[j];}unsigned n=3;
        const bool cut[]={left>x0,right<x1,top>y0,bottom<y1};
        const float edges[]={left,right,top,bottom};
        for(unsigned c=0;c<4;++c)if(cut[c]) {
          n=SceneryShading_Clip(a,depths?za:NULL,n,b,depths?zb:NULL,c/2,edges[c],(c&1)==0);
          for(unsigned j=0;j<n;++j){a[j]=b[j];if(depths)za[j]=zb[j];}
        }
        if(!SceneryShading_Polygon(&batch,field,uv,a,za,n,(left+right)*.5f,(top+bottom)*.5f,-1))return false;
        if(right>=x1)break;
      }
      if(bottom>=y1)break;
    }
  }
  return SceneryShading_Flush(&batch);
}

typedef struct SceneryShadingLegacySubmit {
  SceneryShadingSubmit submit;
  void *context;
} SceneryShadingLegacySubmit;
static inline bool SceneryShading_Submit2D(void *context,const ArRenderVertex2D *v,
    const float *depths,int nv,const int32_t *indices,int ni) {
  (void)depths;
  SceneryShadingLegacySubmit *legacy=context;
  return legacy->submit(legacy->context,v,nv,indices,ni);
}
static inline bool SceneryShading_Draw(const SceneryShadingField *field,ArRenderRectF uv,
    const ArRenderVertex2D *vertices,int nv,const int32_t *indices,int ni,
    SceneryShadingSubmit submit,void *context) {
  if(!field || !field->sample)return submit(context,vertices,nv,indices,ni);
  SceneryShadingLegacySubmit legacy={submit,context};
  return SceneryShading_DrawDepth(field,uv,vertices,NULL,nv,indices,ni,SceneryShading_Submit2D,&legacy);
}
#endif
