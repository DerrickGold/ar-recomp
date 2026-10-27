#ifndef AR_ACTION_EFFECT_MASK_TEXTURE_H
#define AR_ACTION_EFFECT_MASK_TEXTURE_H
#include "action_effect_masks.h"
#include "render/render_device.h"
#include <stdlib.h>

static inline bool ActionEffectMasks_UsesTexture(const ArRenderVertex2D *vertices,int count) {
  for(int i=0;i<count;++i)
    if(vertices[i].tex_coord.x!=0||vertices[i].tex_coord.y!=0)return true;
  return false;
}

/* Render-thread owner supplies and releases the handle with its other cached
 * textures. Creation/upload happens once, never per frame or per emitter. */
static inline bool ActionEffectMasks_EnsureTexture(ArRenderDevice *device,ArRenderTexture *out) {
  if(ArRenderTexture_IsValid(*out))return true;
  const unsigned width=kActionMaskAtlasWidth,height=kActionMaskAtlasHeight;
  uint32_t *pixels=malloc(width*height*sizeof(*pixels));
  if(!pixels)return false;
  ActionEffectMasks_Build(pixels);
  const ArRenderTextureDesc desc={.width=width,.height=height,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Static,.filter=kArRenderFilter_Linear,.blend=kArRenderBlendMode_Alpha};
  const ArRenderRectI full={0,0,(int)width,(int)height};
  bool ok=ArRenderDevice_CreateTexture(device,&desc,out)&&
      ArRenderDevice_UpdateTexture(device,*out,&full,pixels,width*sizeof(*pixels));
  free(pixels);
  if(!ok){ArRenderDevice_DestroyTexture(device,*out);*out=ArRenderTexture_Invalid();}
  return ok;
}
#endif
