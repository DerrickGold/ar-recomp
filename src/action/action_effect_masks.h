#ifndef AR_ACTION_EFFECT_MASKS_H
#define AR_ACTION_EFFECT_MASKS_H
#include "render/render_types.h"

/* One immutable atlas shared by fog, canopy light, foliage and small glows. Cell zero is
 * white, so existing untextured vertices keep their original appearance. */
enum { kActionMaskCellSize=128, kActionMaskAtlasColumns=8,
       kActionMaskAtlasWidth=1024, kActionMaskAtlasHeight=768,
       kActionMaskFogFirst=1, kActionMaskFogCount=7,
       kActionMaskCanopyFirst=8, kActionMaskCanopyCount=8,
       kActionMaskVineFirst=16, kActionMaskVineCount=8, kActionMaskGlow=24, kActionMaskFirefly=25, kActionMaskLeaf=26,
       kActionMaskMossFirst=27, kActionMaskMossCount=4, kActionMaskRipple=31,
       /* Matched density/illumination masks keep cloud shading in the same
        * ordinary alpha batch, without another shader or render target. */
       kActionMaskCloudBodyFirst=32, kActionMaskCloudLightFirst=40, kActionMaskCloudCount=8 };
ArRenderPointF ActionEffectMasks_Uv(unsigned cell,float u,float v);
uint32_t ActionEffectMasks_Pixel(unsigned x,unsigned y);
void ActionEffectMasks_Build(uint32_t *pixels);
/* Quantized bilinear alpha, identical to the GPU sampler. Used by fog/reflection
 * sampling and the flat renderer's object-light fallback; the data is immutable. */
float ActionEffectMasks_Sample(float u,float v);
#endif
