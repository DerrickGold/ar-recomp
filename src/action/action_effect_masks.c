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
  if(cell>=kActionMaskFlowerStem) {
    /* Small pixel-shaped silhouettes are baked once. One stem strip and a
     * bloom replace many tiny clipped petal/leaf triangles. */
    const float x=u*2-1,y=v*2-1;
    if(cell==kActionMaskFlowerStem) {
      float a=fabsf(x)<.075f&&v>.03f?1:0;
      const float roots[2]={.45f,.72f};
      for(unsigned i=0;i<2;++i) {
        const float t=x*(i?1:-1);
        if(t>0&&t<.91f&&fabsf(v-roots[i]+t*.18f)<.10f*sinf(t*3.45f))a=1;
      }
      return a;
    }
    if(cell>=kActionMaskFlowerFirst&&cell<kActionMaskFlowerFirst+kActionMaskFlowerCount) {
      const int px=(int)floorf(u*12),py=(int)floorf(v*12);
      static const uint16_t silhouettes[3][12]={
        {0x000,0x060,0x0f0,0x1f8,0x7fe,0x7fe,0x7fe,0x3fc,0x0f0,0x060,0x000,0x000},
        {0x000,0x060,0x0f0,0x0f0,0x1f8,0x1f8,0x3fc,0x3fc,0x7fe,0x5aa,0x000,0x000},
        {0x000,0x060,0x0f0,0x1f8,0x1f8,0x1f8,0x0f0,0x060,0x060,0x000,0x000,0x000}};
      return px<12&&py<12&&(silhouettes[cell-kActionMaskFlowerFirst][py]&(1u<<px))?1:0;
    }
    if(cell==kActionMaskButterflyWing) {
      const float upper=(x-.12f)*(x-.12f)/.61f+(y+.30f)*(y+.30f)/.37f;
      const float lower=(x+.16f)*(x+.16f)/.44f+(y-.42f)*(y-.42f)/.22f;
      return 1-CloudNoise_Ramp(.78f,1,fminf(upper,lower));
    }
    if(cell==kActionMaskCrown) {
      /* Overlapping broad leaves form a broken crown, not hanging vines. */
      float a=0;
      for(unsigned i=0;i<13;++i) {
        const float cx=CloudNoise_Seed(i*71+39)*1.8f-.9f,cy=CloudNoise_Seed(i*97+21)*1.7f-.85f;
        const float turn=CloudNoise_Seed(i*53+11)*6.2831853f,cs=cosf(turn),sn=sinf(turn);
        const float dx=(x-cx)*cs+(y-cy)*sn,dy=-(x-cx)*sn+(y-cy)*cs;
        const float half=.14f*fmaxf(0,1-dx*dx/.16f);
        a=fmaxf(a,(1-CloudNoise_Ramp(half*.65f,half+.025f,fabsf(dy)))*(1-CloudNoise_Ramp(.32f,.42f,fabsf(dx))));
      }
      return a*CloudNoise_Ramp(0,.16f,1-fabsf(x))*CloudNoise_Ramp(0,.16f,1-fabsf(y));
    }
    return 0;
  }
  if(cell>=kActionMaskCausticFirst) {
    /* Warped cellular boundaries form rounded, interlocking light arcs.
     * All eight cells share the pattern with progressively softer ridges.
     * Bake once; animation only deforms UVs on the existing light mesh. */
    const float edge=CloudNoise_Ramp(0,.16f,u)*CloudNoise_Ramp(0,.16f,1-u)*
        CloudNoise_Ramp(0,.16f,v)*CloudNoise_Ramp(0,.16f,1-v);
    if(edge==0)return 0;
    const float x=u*4.6f+.32f*sinf(v*12+.6f*sinf(u*15)),
                y=v*4.6f+.28f*sinf(u*13-.5f*cosf(v*17));
    const int ix=(int)floorf(x),iy=(int)floorf(y);
    float nearest=100,second=100;
    for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i) {
      const uint32_t id=(uint32_t)(ix+i)*0x9e3779b9u^(uint32_t)(iy+j)*0x85ebca6bu^713;
      const float dx=x-(ix+i+.16f+.68f*CloudNoise_Seed(id)),
                  dy=y-(iy+j+.16f+.68f*CloudNoise_Seed(id^173));
      const float d=dx*dx+dy*dy;
      if(d<nearest){second=nearest;nearest=d;}else second=fminf(second,d);
    }
    const float distance=sqrtf(second)-sqrtf(nearest);
    const float softness=(float)(cell-kActionMaskCausticFirst)/(kActionMaskCausticCount-1);
    const float ridge=1-CloudNoise_Ramp(.018f+.045f*softness,.065f+.12f*softness,distance);
    const float broken=.18f+.82f*CloudNoise_Ramp(.23f,.70f,CloudNoise_Seeded2(x*.8f,y*.8f,991));
    return edge*ridge*broken;
  }
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
    for(unsigned y=kActionMaskCausticFirst/kActionMaskAtlasColumns*kActionMaskCellSize;
        y<kActionMaskAtlasHeight;++y)for(unsigned x=0;x<kActionMaskAtlasWidth;++x)
      s_alpha[y*kActionMaskAtlasWidth+x]=(uint8_t)(GeneratePixel(x,y)>>24);
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
