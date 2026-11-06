#ifndef AR_DIORAMA_DEPTH_ALPHA_UTIL_H
#define AR_DIORAMA_DEPTH_ALPHA_UTIL_H

#include "diorama/diorama_depth_pass.h"
#include <SDL3/SDL_gpu.h>
#include <limits.h>
#include <math.h>

typedef struct DioramaDepthAlphaTargetFormats {
  SDL_GPUTextureFormat order, key;
} DioramaDepthAlphaTargetFormats;

static inline DioramaDepthAlphaTargetFormats DioramaDepthAlpha_SelectTargetFormats(
    bool supports_r32, bool supports_rg32) {
  /* The shaders read only order.r and previous_key.xy. Each retained channel
   * keeps the original 32-bit float precision and negative sentinel value. */
  return (DioramaDepthAlphaTargetFormats){
    supports_r32 ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
    supports_rg32 ? SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT};
}

static inline bool DioramaDepthAlpha_NeedsInitialPass(SDL_GPULoadOp color_load, bool clear_depth) {
  return color_load != SDL_GPU_LOADOP_LOAD || clear_depth;
}

/* Positive homogeneous W makes the projected clipped triangle a subset of
 * its projected vertex bounds. Materials cannot emit outside that coverage.
 * One output pixel guards floating-point and fixed-point raster rounding.
 * Peel layers are cleared per band, so outside this union all resolve samples
 * are zero. Unsupported inputs conservatively retain the full-target resolve. */
static inline ArRenderRectI DioramaDepthAlpha_ResolveBounds(const DioramaDepthDraw *draws,
    unsigned first,unsigned last,unsigned width,unsigned height) {
  const ArRenderRectI full={0,0,(int)width,(int)height};
  if(!draws || width>INT_MAX || height>INT_MAX || !width || !height || first>last)
    return full;
  int x0=(int)width,y0=(int)height,x1=0,y1=0;
  for(unsigned i=first;i<last;++i) {
    const DioramaDepthDraw *draw=&draws[i];
    if(!draw->vertices || !draw->indices || !draw->vertex_count || !draw->index_count)
      return full;
    ArRenderRectI clip=draw->clip_enabled?draw->clip:full;
    if(clip.x<0 || clip.y<0 || clip.w<=0 || clip.h<=0 ||
        (int64_t)clip.x+clip.w>width || (int64_t)clip.y+clip.h>height)return full;
    double left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
    for(unsigned j=0;j<draw->index_count;++j) {
      const int index=draw->indices[j];
      if(index<0 || (unsigned)index>=draw->vertex_count)return full;
      const float *v=draw->vertices[index].clip;
      if(!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) ||
          !isfinite(v[3]) || v[3]<=0)return full;
      /* Use double through clipping, then clamp before integer conversion;
       * finite float coordinates divided by a tiny positive W remain safe. */
      const double x=(1+(double)v[0]/v[3])*.5*width;
      const double y=(1-(double)v[1]/v[3])*.5*height;
      left=fmin(left,x);right=fmax(right,x);top=fmin(top,y);bottom=fmax(bottom,y);
    }
    left=fmax(clip.x,fmin((double)clip.x+clip.w,floor(left)-1));
    right=fmax(clip.x,fmin((double)clip.x+clip.w,ceil(right)+1));
    top=fmax(clip.y,fmin((double)clip.y+clip.h,floor(top)-1));
    bottom=fmax(clip.y,fmin((double)clip.y+clip.h,ceil(bottom)+1));
    if(right<=left || bottom<=top)continue;
    if((int)left<x0)x0=(int)left;if((int)top<y0)y0=(int)top;
    if((int)right>x1)x1=(int)right;if((int)bottom>y1)y1=(int)bottom;
  }
  return x1>x0 && y1>y0?(ArRenderRectI){x0,y0,x1-x0,y1-y0}:(ArRenderRectI){0};
}

#endif
