#include "action_effect_masks.h"
#include "render/cloud_material.h"
#include "render/cloud_noise.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>

/* Atlas ownership stays here; cloud materials have no action/editor state. */
static void BakeCloud(unsigned variant,uint8_t *atlas) {
  /* Prepare publishes one immutable atlas. Its single writer owns scratch,
   * kept off the small WASM stack and reused for every material variant. */
  static CloudVolumeScratch scratch;
  CloudVolumeMaterial material;
  CloudMaterial_Billows(variant,&material);
  const unsigned column=variant%kActionMaskAtlasColumns*kActionMaskCellSize;
  const unsigned body=(kActionMaskCloudBodyFirst+variant)/kActionMaskAtlasColumns*kActionMaskCellSize;
  const unsigned light=(kActionMaskCloudLightFirst+variant)/kActionMaskAtlasColumns*kActionMaskCellSize;
  const bool baked=CloudMaterial_BakeMasks(&material,&kCloudBillowLight,&scratch,
      atlas+body*kActionMaskAtlasWidth+column,atlas+light*kActionMaskAtlasWidth+column,
      kActionMaskAtlasWidth,kActionMaskCellSize,kActionMaskCellSize);
  assert(baked); (void)baked;
}
static float Density(unsigned cell,float u,float v) {
  if(!cell)return 1;
  const uint32_t seed=cell*0x91e10da5u;
  if(cell==kActionMaskRipple) {
    const float r=hypotf(u*2-1,v*2-1);
    return CloudNoise_Ramp(.62f,.76f,r)*(1-CloudNoise_Ramp(.80f,.96f,r));
  }
  if(cell>=kActionMaskMossFirst&&cell<kActionMaskMossFirst+kActionMaskMossCount) {
    /* Small branching patches with holes, not circular light blobs. The four
     * variants occupy spare atlas cells; texture size and draw batching stay fixed. */
    const float edge=CloudNoise_Ramp(0,.24f,u)*CloudNoise_Ramp(0,.24f,1-u)*CloudNoise_Ramp(0,.26f,v)*CloudNoise_Ramp(0,.26f,1-v);
    const float coarse=CloudNoise_Seeded2(u*5,v*3,seed),fine=CloudNoise_Seeded2(u*19,v*11,seed^71);
    return edge*CloudNoise_Ramp(.37f,.67f,coarse)*CloudNoise_Ramp(.25f,.68f,fine);
  }
  if(cell==kActionMaskLeaf) {
    const float x=u*2-1,y=v*2-1;
    const float half=.70f*fmaxf(0,1-x*x);
    return (1-CloudNoise_Ramp(half*.65f,half+.05f,fabsf(y-.12f*sinf(x*3.1415927f))))*CloudNoise_Ramp(0,.12f,1-fabsf(x));
  }
  if(cell>=kActionMaskGlow) {
    const float r=hypotf(u*2-1,v*2-1);
    const float halo=(1-CloudNoise_Ramp(0,1,r))*(1-CloudNoise_Ramp(0,1,r));
    const float core=(1-CloudNoise_Ramp(0,.20f,r))*(1-CloudNoise_Ramp(0,.20f,r));
    return cell==kActionMaskFirefly?.18f*halo+.82f*core:halo;
  }
  if(cell>=kActionMaskVineFirst) {
    /* Bake narrow, pointed leaves and their curved stem once. No new texture
     * per emitter, noise shader, or per-frame tessellation is needed. */
    const float phase=(seed&255)*.02464f;
    const float stem=.5f+.11f*sinf(v*4.5f+phase);
    float alpha=(1-CloudNoise_Ramp(.006f,.017f,fabsf(u-stem)))*(1-CloudNoise_Ramp(.78f,.99f,v));
    for(unsigned i=0;i<7;++i) {
      const float y=.10f+i*.12f,x=.5f+.11f*sinf(y*4.5f+phase);
      const float side=(i&1)?-1.f:1.f,reach=.24f+.10f*((seed>>(i*3))&7)/7.f;
      const float t=(u-x)*side/reach;
      if(t<0||t>1)continue;
      const float half=.052f*sinf(t*3.1415927f);
      const float d=fabsf(v-y-t*.06f);
      alpha=fmaxf(alpha,(1-CloudNoise_Ramp(half*.5f,half+.007f,d))*.92f);
    }
    return alpha*CloudNoise_Ramp(0,.04f,u)*CloudNoise_Ramp(0,.04f,1-u)*CloudNoise_Ramp(0,.025f,v)*CloudNoise_Ramp(0,.025f,1-v);
  }
  const float x=u*2-1,y=v*2-1;
  const float edge=CloudNoise_Ramp(0,.22f,1-x*x)*CloudNoise_Ramp(0,.32f,1-y*y);
  if(edge==0)return 0;
  /* Broad bodies, curled filaments and small holes share one precomputed
   * density field. Animation moves these immutable masks, not their pixels. */
  if(cell<kActionMaskCanopyFirst)return CloudMaterial_Wisps(u,v,seed);
  /* Leaf gaps vary along the light as well as across it. Nearly constant
   * vertical noise reads as evenly spaced stripes rather than dappled light. */
  const float gaps=.65f*CloudNoise_Seeded2(u*32,v*32,seed^31)+.35f*CloudNoise_Seeded2(u*57,v*57,seed^97);
  /* Larger crowns leave irregular shaded pockets between the fine leaf gaps.
   * This hierarchy is baked once into the atlas, not evaluated per frame. */
  const float crowns=.65f*CloudNoise_Seeded2(u*3.2f,v*3.2f,seed^151)+.35f*CloudNoise_Seeded2(u*7,v*7,seed^317);
  const float leaves=CloudNoise_Ramp(.32f,.70f,gaps),opening=CloudNoise_Ramp(.61f,.82f,crowns);
  return edge*(leaves*(.18f+.82f*CloudNoise_Ramp(.24f,.72f,crowns))+.18f*opening*(1-leaves));
}
ArRenderPointF ActionEffectMasks_Uv(unsigned cell,float u,float v) {
  cell%=kActionMaskAtlasColumns*(kActionMaskAtlasHeight/kActionMaskCellSize);
  return (ArRenderPointF){((cell%kActionMaskAtlasColumns)*kActionMaskCellSize+.5f+u*(kActionMaskCellSize-1))/kActionMaskAtlasWidth,
      ((cell/kActionMaskAtlasColumns)*kActionMaskCellSize+.5f+v*(kActionMaskCellSize-1))/kActionMaskAtlasHeight};
}
static uint32_t GeneratePixel(unsigned x,unsigned y) {
  const unsigned cell=(y/kActionMaskCellSize)*kActionMaskAtlasColumns+x/kActionMaskCellSize;
  const float alpha=Density(cell,(x%kActionMaskCellSize)/(float)(kActionMaskCellSize-1),
      (y%kActionMaskCellSize)/(float)(kActionMaskCellSize-1));
  return ((uint32_t)floorf(alpha*255+.5f)<<24)|0xffffffu;
}
/* Immutable after first use. Both game-thread receiver sampling and renderer
 * upload use the quantized GPU values; repeated CPU samples never run noise. */
static uint8_t s_alpha[kActionMaskAtlasWidth*kActionMaskAtlasHeight];
static atomic_uint s_ready=ATOMIC_VAR_INIT(0);
static void Prepare(void) {
  if(atomic_load_explicit(&s_ready,memory_order_acquire)==2)return;
  unsigned empty=0;
  if(atomic_compare_exchange_strong_explicit(&s_ready,&empty,1,memory_order_acq_rel,memory_order_acquire)) {
    for(unsigned y=0;y<kActionMaskCloudBodyFirst/kActionMaskAtlasColumns*kActionMaskCellSize;++y)
      for(unsigned x=0;x<kActionMaskAtlasWidth;++x)
      s_alpha[y*kActionMaskAtlasWidth+x]=(uint8_t)(GeneratePixel(x,y)>>24);
    for (unsigned variant=0; variant<kActionMaskCloudCount; ++variant)
      BakeCloud(variant,s_alpha);
    atomic_store_explicit(&s_ready,2,memory_order_release);
  } else while(atomic_load_explicit(&s_ready,memory_order_acquire)!=2) {}
}
uint32_t ActionEffectMasks_Pixel(unsigned x,unsigned y) {
  Prepare();
  if(x>=kActionMaskAtlasWidth)x=kActionMaskAtlasWidth-1;
  if(y>=kActionMaskAtlasHeight)y=kActionMaskAtlasHeight-1;
  return ((uint32_t)s_alpha[y*kActionMaskAtlasWidth+x]<<24)|0xffffffu;
}
void ActionEffectMasks_Build(uint32_t *pixels) {
  if(!pixels)return;
  Prepare();
  for(unsigned i=0;i<kActionMaskAtlasWidth*kActionMaskAtlasHeight;++i)
    pixels[i]=((uint32_t)s_alpha[i]<<24)|0xffffffu;
}
float ActionEffectMasks_Sample(float u,float v) {
  if(u==0&&v==0)return 1;
  Prepare();
  const float x=fmaxf(0,fminf(kActionMaskAtlasWidth-1,u*kActionMaskAtlasWidth-.5f)),y=fmaxf(0,fminf(kActionMaskAtlasHeight-1,v*kActionMaskAtlasHeight-.5f));
  const unsigned ix=(unsigned)x,iy=(unsigned)y;
  const unsigned next_x=ix+1<kActionMaskAtlasWidth?ix+1:ix;
  const unsigned next_y=iy+1<kActionMaskAtlasHeight?iy+1:iy;
  const float a=x-ix,b=y-iy;
  /* One acquire publishes the entire immutable atlas, including all four
   * taps. Avoid repeating its atomic readiness check for each texel. */
  const float p0=s_alpha[iy*kActionMaskAtlasWidth+ix]/255.f;
  const float p1=s_alpha[iy*kActionMaskAtlasWidth+next_x]/255.f;
  const float p2=s_alpha[next_y*kActionMaskAtlasWidth+ix]/255.f;
  const float p3=s_alpha[next_y*kActionMaskAtlasWidth+next_x]/255.f;
  return (p0+(p1-p0)*a)*(1-b)+(p2+(p3-p2)*a)*b;
}
