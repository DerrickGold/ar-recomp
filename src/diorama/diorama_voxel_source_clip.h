#ifndef AR_DIORAMA_VOXEL_SOURCE_CLIP_H
#define AR_DIORAMA_VOXEL_SOURCE_CLIP_H

#include <math.h>
#include <stdbool.h>
#include "render/render_device.h"

/* A source boundary is a texture-storage boundary, never a physical wall.
 * Clip each original triangle in source UV, retaining its original diagonal.
 * Native cap UV may cross this boundary at a different point from geometry Y.
 * The depth path carries homogeneous W; the painter path preserves its
 * existing screen-linear interpolation instead. */
typedef struct DioramaVoxelSourceVertex {
  ArRenderVertex2D vertex;
  float depth, clip_w;
} DioramaVoxelSourceVertex;

static inline DioramaVoxelSourceVertex DioramaVoxelSource_Lerp(
    const DioramaVoxelSourceVertex *a,const DioramaVoxelSourceVertex *b,
    float t,bool perspective) {
  DioramaVoxelSourceVertex p=*a;
  const float w=perspective?a->clip_w+(b->clip_w-a->clip_w)*t:1;
  const float sa=perspective?(1-t)*a->clip_w/w:1-t;
  const float sb=perspective?t*b->clip_w/w:t;
  p.vertex.position.x=sa*a->vertex.position.x+sb*b->vertex.position.x;
  p.vertex.position.y=sa*a->vertex.position.y+sb*b->vertex.position.y;
  p.depth=sa*a->depth+sb*b->depth;
  p.clip_w=w;
  p.vertex.tex_coord.x=a->vertex.tex_coord.x+(b->vertex.tex_coord.x-a->vertex.tex_coord.x)*t;
  p.vertex.tex_coord.y=a->vertex.tex_coord.y+(b->vertex.tex_coord.y-a->vertex.tex_coord.y)*t;
  p.vertex.color.r=a->vertex.color.r+(b->vertex.color.r-a->vertex.color.r)*t;
  p.vertex.color.g=a->vertex.color.g+(b->vertex.color.g-a->vertex.color.g)*t;
  p.vertex.color.b=a->vertex.color.b+(b->vertex.color.b-a->vertex.color.b)*t;
  p.vertex.color.a=a->vertex.color.a+(b->vertex.color.a-a->vertex.color.a)*t;
  return p;
}

/* At most four polygon vertices. The primary owns v<seam and the guard owns
 * v>=seam, including a constant-UV cap exactly at the boundary. Shared edge
 * vertices do not duplicate any area. Returns zero for empty/invalid input. */
static inline unsigned DioramaVoxelSource_ClipTriangle(
    const DioramaVoxelSourceVertex in[3],float seam,bool guard,bool perspective,
    DioramaVoxelSourceVertex out[4]) {
  if(!in||!out||!isfinite(seam))return 0;
  for(unsigned i=0;i<3;++i)
    if(!isfinite(in[i].vertex.tex_coord.y)||
        (perspective&&(!isfinite(in[i].clip_w)||in[i].clip_w<=0)))return 0;
  unsigned n=0;
  for(unsigned i=0;i<3;++i) {
    const DioramaVoxelSourceVertex *a=&in[i],*b=&in[(i+1)%3];
    const float av=a->vertex.tex_coord.y,bv=b->vertex.tex_coord.y;
    const bool ai=guard?av>=seam:av<seam,bi=guard?bv>=seam:bv<seam;
    if(ai)out[n++]=*a;
    if(ai!=bi) {
      const float t=(seam-av)/(bv-av);
      out[n]=DioramaVoxelSource_Lerp(a,b,t,perspective);
      out[n++].vertex.tex_coord.y=seam;
    }
  }
  return n;
}

/* Clip an already split convex source polygon to a finite texture extent.
 * Inclusive geometric boundaries share vertices; no nonzero area exists
 * outside the finite captured source. Eight slots cover a clipped triangle. */
static inline unsigned DioramaVoxelSource_ClipLimit(
    const DioramaVoxelSourceVertex *in,unsigned count,unsigned axis,float limit,
    bool greater,bool perspective,DioramaVoxelSourceVertex out[8]) {
  if(!in||!out||count>8||axis>1||!isfinite(limit))return 0;
  unsigned n=0;
  for(unsigned i=0;i<count;++i) {
    const DioramaVoxelSourceVertex *a=&in[i],*b=&in[(i+1)%count];
    const float av=axis?a->vertex.tex_coord.y:a->vertex.tex_coord.x;
    const float bv=axis?b->vertex.tex_coord.y:b->vertex.tex_coord.x;
    const bool ai=greater?av>=limit:av<=limit,bi=greater?bv>=limit:bv<=limit;
    if(ai){if(n==8)return 0;out[n++]=*a;}
    if(ai!=bi) {
      if(n==8)return 0;
      out[n]=DioramaVoxelSource_Lerp(a,b,(limit-av)/(bv-av),perspective);
      if(axis)out[n].vertex.tex_coord.y=limit;else out[n].vertex.tex_coord.x=limit;
      ++n;
    }
  }
  return n;
}
#endif
