#ifndef AR_DIORAMA_SAND_SURFACE_H
#define AR_DIORAMA_SAND_SURFACE_H
#include <stdbool.h>
#include <math.h>
/* Pixel bounds are end-exclusive. An optional finite guard is relative to
 * the primary capture; it may be narrower or start after an unavailable gap. */
static inline bool DioramaSandSurface_Covered(float x,float y,float width,
    int primary_width,int primary_height,const float guard[4],unsigned rows) {
  if(!rows || rows>16 || !isfinite(x)||!isfinite(y)||!isfinite(width)||width<=0 ||
      x<0 || y<0 || x+width>primary_width)return false;
  const float bottom=y+rows;
  if(bottom<=primary_height)return true;
  if(!guard || guard[2]<=0 || guard[3]<=0 || x<guard[0] ||
      x+width>guard[0]+guard[2])return false;
  const float lower=fmaxf(y,(float)primary_height);
  return lower>=guard[1] && bottom<=guard[1]+guard[3];
}
static inline float DioramaSandSurface_SourceY(float anchor,float depth,
    float near_depth,float far_depth,unsigned rows) {
  const float span=far_depth-near_depth;
  const float fraction=span>.00001f?fmaxf(0,fminf(1,(depth-near_depth)/span)):0;
  return anchor+.5f+(rows?rows-1:0)*fraction;
}
#endif
