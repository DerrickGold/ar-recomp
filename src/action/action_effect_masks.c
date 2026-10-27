#include "action_effect_masks.h"
#include "deterministic_hash.h"
#include <math.h>
#include <stdatomic.h>

static float Smooth(float a,float b,float x) {
  const float t=fmaxf(0,fminf(1,(x-a)/(b-a)));return t*t*(3-2*t);
}
static float Noise(float x,float y,uint32_t seed) {
  const int ix=(int)floorf(x),iy=(int)floorf(y);
  const float u=Smooth(0,1,x-ix),v=Smooth(0,1,y-iy);
  float n[4];
  for(unsigned j=0;j<4;++j) {
    const uint32_t h=DeterministicHash_Mix32(seed^(uint32_t)(ix+(j&1))*0x9e3779b9u^
        (uint32_t)(iy+(j>>1))*0x85ebca6bu);
    n[j]=(h&65535)/65535.f;
  }
  return (n[0]+(n[1]-n[0])*u)*(1-v)+(n[2]+(n[3]-n[2])*u)*v;
}
static float Density(unsigned cell,float u,float v) {
  if(!cell)return 1;
  const uint32_t seed=cell*0x91e10da5u;
  if(cell==kActionMaskRipple) {
    const float r=hypotf(u*2-1,v*2-1);
    return Smooth(.62f,.76f,r)*(1-Smooth(.80f,.96f,r));
  }
  if(cell>=kActionMaskMossFirst&&cell<kActionMaskMossFirst+kActionMaskMossCount) {
    /* Small branching patches with holes, not circular light blobs. The four
     * variants occupy spare atlas cells; texture size and draw batching stay fixed. */
    const float edge=Smooth(0,.24f,u)*Smooth(0,.24f,1-u)*Smooth(0,.26f,v)*Smooth(0,.26f,1-v);
    const float coarse=Noise(u*5,v*3,seed),fine=Noise(u*19,v*11,seed^71);
    return edge*Smooth(.37f,.67f,coarse)*Smooth(.25f,.68f,fine);
  }
  if(cell==kActionMaskLeaf) {
    const float x=u*2-1,y=v*2-1;
    const float half=.70f*fmaxf(0,1-x*x);
    return (1-Smooth(half*.65f,half+.05f,fabsf(y-.12f*sinf(x*3.1415927f))))*Smooth(0,.12f,1-fabsf(x));
  }
  if(cell>=kActionMaskGlow) {
    const float r=hypotf(u*2-1,v*2-1);
    const float halo=(1-Smooth(0,1,r))*(1-Smooth(0,1,r));
    const float core=(1-Smooth(0,.20f,r))*(1-Smooth(0,.20f,r));
    return cell==kActionMaskFirefly?.18f*halo+.82f*core:halo;
  }
  if(cell>=kActionMaskVineFirst) {
    /* Bake narrow, pointed leaves and their curved stem once. No new texture
     * per emitter, noise shader, or per-frame tessellation is needed. */
    const float phase=(seed&255)*.02464f;
    const float stem=.5f+.11f*sinf(v*4.5f+phase);
    float alpha=(1-Smooth(.006f,.017f,fabsf(u-stem)))*(1-Smooth(.78f,.99f,v));
    for(unsigned i=0;i<7;++i) {
      const float y=.10f+i*.12f,x=.5f+.11f*sinf(y*4.5f+phase);
      const float side=(i&1)?-1.f:1.f,reach=.24f+.10f*((seed>>(i*3))&7)/7.f;
      const float t=(u-x)*side/reach;
      if(t<0||t>1)continue;
      const float half=.052f*sinf(t*3.1415927f);
      const float d=fabsf(v-y-t*.06f);
      alpha=fmaxf(alpha,(1-Smooth(half*.5f,half+.007f,d))*.92f);
    }
    return alpha*Smooth(0,.04f,u)*Smooth(0,.04f,1-u)*Smooth(0,.025f,v)*Smooth(0,.025f,1-v);
  }
  const float x=u*2-1,y=v*2-1;
  const float edge=Smooth(0,.22f,1-x*x)*Smooth(0,.32f,1-y*y);
  if(edge==0)return 0;
  /* Broad bodies, curled filaments and small holes share one precomputed
   * density field. Animation moves these immutable masks, not their pixels. */
  if(cell<kActionMaskCanopyFirst) {
    const float wx=u*4+Noise(u*3,v*3,seed)*1.5f;
    const float wy=v*5+Noise(u*3+9,v*3,seed^71)*1.3f;
    const float n=.53f*Noise(wx,wy,seed)+.29f*Noise(wx*2.1f,wy*2.1f,seed^31)+
        .18f*Noise(wx*4.3f,wy*4.3f,seed^97);
    return edge*Smooth(.25f,.71f,n)*(.65f+.35f*Noise(wx*7,wy*7,seed^57));
  }
  /* Leaf gaps vary along the light as well as across it. Nearly constant
   * vertical noise reads as evenly spaced stripes rather than dappled light. */
  const float gaps=.65f*Noise(u*32,v*32,seed^31)+.35f*Noise(u*57,v*57,seed^97);
  /* Larger crowns leave irregular shaded pockets between the fine leaf gaps.
   * This hierarchy is baked once into the atlas, not evaluated per frame. */
  const float crowns=.65f*Noise(u*3.2f,v*3.2f,seed^151)+.35f*Noise(u*7,v*7,seed^317);
  const float leaves=Smooth(.32f,.70f,gaps),opening=Smooth(.61f,.82f,crowns);
  return edge*(leaves*(.18f+.82f*Smooth(.24f,.72f,crowns))+.18f*opening*(1-leaves));
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
    for(unsigned y=0;y<kActionMaskAtlasHeight;++y)for(unsigned x=0;x<kActionMaskAtlasWidth;++x)
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
