/* Actual scenery fragment shader versus an independent receiver-space oracle.
 * Repacking the same source ink cannot change the world phase. Magenta outside
 * the owned slot catches wrap/clamp leakage, including linear-filter edges. */
#include "diorama_owned_heat_oracle.h"
#include <stdlib.h>
enum { kOwnedHeatAtlas=256 };
static uint32_t s_owned_heat_atlas[kOwnedHeatAtlas*kOwnedHeatAtlas];

static ArRenderTexture OwnedHeatAtlas(ArRenderDevice *device,unsigned scale,bool linear) {
  for(unsigned i=0;i<kOwnedHeatAtlas*kOwnedHeatAtlas;++i)s_owned_heat_atlas[i]=0xffff00ff;
  const unsigned origins[2][2]={{0,0},{128,128}};
  for(unsigned slot=0;slot<2;++slot)for(unsigned y=0;y<16*scale;++y)for(unsigned x=0;x<16*scale;++x) {
    const unsigned nx=x/scale,ny=y/scale;
    const unsigned a=(nx==3||ny==5)?0:(nx==8||ny==11)?128:255;
    const uint32_t color=a?((a<<24)|((24+nx*11)<<16)|((32+ny*10)<<8)|(12+(nx+ny)*5)):0;
    s_owned_heat_atlas[(y+origins[slot][1])*kOwnedHeatAtlas+x+origins[slot][0]]=color;
  }
  const ArRenderTextureDesc desc={.width=kOwnedHeatAtlas,.height=kOwnedHeatAtlas,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Static,
      .filter=linear?kArRenderFilter_Linear:kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  ArRenderTexture texture;assert(ArRenderDevice_CreateTexture(device,&desc,&texture));
  assert(ArRenderDevice_UpdateTexture(device,texture,NULL,s_owned_heat_atlas,kOwnedHeatAtlas*4));return texture;
}
static double OwnedHeatSample(OwnedHeatOraclePoint uv,unsigned channel,bool linear) {
  const unsigned shift[4]={16,8,0,24};
  const double px=uv.x*kOwnedHeatAtlas-(linear?.5:0),py=uv.y*kOwnedHeatAtlas-(linear?.5:0);
  const int x=(int)floor(px),y=(int)floor(py);const double fx=px-x,fy=py-y;
  double value=0;
  for(unsigned oy=0;oy<(linear?2u:1u);++oy)for(unsigned ox=0;ox<(linear?2u:1u);++ox) {
    const int sx=x+(int)ox,sy=y+(int)oy;assert(sx>=0&&sx<kOwnedHeatAtlas&&sy>=0&&sy<kOwnedHeatAtlas);
    const double weight=linear?(ox?fx:1-fx)*(oy?fy:1-fy):1;
    value+=((s_owned_heat_atlas[sy*kOwnedHeatAtlas+sx]>>shift[channel])&255)*weight;
  }
  return value;
}
static OwnedHeatOraclePoint OwnedHeatInterpolate(const DioramaDepthVertex v[4],double tx,double ty,bool world) {
  unsigned ids[3];double bary[3];
  if(tx>=ty){ids[0]=0;ids[1]=1;ids[2]=2;bary[0]=1-tx;bary[1]=tx-ty;bary[2]=ty;}
  else {ids[0]=0;ids[1]=2;ids[2]=3;bary[0]=1-ty;bary[1]=tx;bary[2]=ty-tx;}
  OwnedHeatOraclePoint out={0};double denominator=0;
  for(unsigned i=0;i<3;++i){const unsigned n=ids[i];const double w=bary[i]/v[n].clip[3];
    const ArRenderPointF p=world?v[n].heat_world_xy:v[n].uv;out.x+=p.x*w;out.y+=p.y*w;denominator+=w;}
  out.x/=denominator;out.y/=denominator;return out;
}
static void TestOwnedHeatMaterials(ArRenderDevice *device,ArGpuDioramaDepthPass *pass,
    ArRenderTexture target,ArRenderTexture black,bool hybrid_mode) {
  unsigned checked=0,transparent=0,fractional=0,displaced=0,ambiguous=0;
  uint8_t actual[W*H*4],off[W*H*4],heated[W*H*4];
  for(unsigned mode=1;mode<=2;++mode)for(unsigned linear=0;linear<2;++linear) {
    const unsigned scale=mode==1?1:4;ArRenderTexture texture=OwnedHeatAtlas(device,scale,linear);
    for(unsigned scenario=0;scenario<8;++scenario) {
      const unsigned origin=scenario==2?128:0;
      const float first=scenario==5?4:.5f/scale,last=scenario==5?12:16-.5f/scale;
      const float alpha=scenario==6?.65f:1;
      DioramaSceneryLightEffectParams p={.illumination=black,.shade={1,1,1,1},
          .heat={.source={400,800,1024,2048},.bounds={{80,120,180,240}},.motion={{scenario?3.75f:0,.71f,0,0}}},
          .heat_owned={scale/(float)kOwnedHeatAtlas,scale/(float)kOwnedHeatAtlas,(float)mode,(float)scale}};
      if(scenario==4){p.heat.source[0]=-7000;p.heat.source[1]=9000;p.heat.source[2]=64;p.heat.source[3]=64;}
      if(scenario==7){p.heat_owned=(DioramaHeatOwned){0};p.heat.motion[0][0]=0;}
      DioramaDepthVertex v[4];
      for(unsigned i=0;i<4;++i) {
        const bool right=i==1||i==2,bottom=i>=2;const float x=right?120:8,y=bottom?88:8,w=right?1.25f:1;
        v[i]=(DioramaDepthVertex){.clip={(2*x/W-1)*w,(1-2*y/H)*w,.35f*w,w},
            .color={1,1,1,alpha},.uv={(origin+(right?last:first)*scale)/kOwnedHeatAtlas,
                (origin+(bottom?last:first)*scale)/kOwnedHeatAtlas},
            .heat_world_xy={100+(right?32:0)+(scenario==3?11:0),160+(bottom?32:0)+(scenario==3?7:0)}};
      }
      const DioramaDepthDraw draw={.vertices=v,.indices=indices,.vertex_count=4,.index_count=6,
          .depth_mode=kDioramaDepth_Write,.blend=kArRenderBlendMode_Alpha,.textured=true,.nearest=!linear};
      SDL_GPUTexture *source=GpuTexture(texture);const ArGpuDioramaDepthMaterial material={.scenery=&p,.illumination=GpuTexture(black)};
      assert(ArSdlRenderBackend_SubmitPending(device));SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(pass->device);assert(cmd);
      assert((hybrid_mode?ArGpuDioramaDepthPass_EncodeHybridMaterials:ArGpuDioramaDepthPass_EncodeMaterials)
          (pass,cmd,GpuTexture(target),W,H,&draw,&source,&material,1,SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
      assert(SDL_SubmitGPUCommandBuffer(cmd));Read(device,target,actual);
      for(unsigned y=8;y<88;++y)for(unsigned x=8;x<120;++x) {
        const double tx=(x+.5-8)/112,ty=(y+.5-8)/80;
        const OwnedHeatOraclePoint uv=OwnedHeatInterpolate(v,tx,ty,false),world=OwnedHeatInterpolate(v,tx,ty,true);
        const OwnedHeatOraclePoint sample=OwnedHeatOracleUv(&p.heat,p.heat_owned,world,uv);
        /* The shader computes float waves; do not confuse a texel-boundary
         * rounding tie with a displacement or ownership mismatch. */
        if(!linear&&(fabs(sample.x*kOwnedHeatAtlas-round(sample.x*kOwnedHeatAtlas))<.001 ||
            fabs(sample.y*kOwnedHeatAtlas-round(sample.y*kOwnedHeatAtlas))<.001)){++ambiguous;continue;}
        const double a=OwnedHeatSample(sample,3,linear)*alpha;
        const unsigned at=(y*W+x)*4;
        for(unsigned c=0;c<4;++c) {
          const double expected=a<=.5?0:c==3?a:OwnedHeatSample(sample,c,linear)*a/255;
          const double error=fabs(actual[at+c]-expected);
          if(error>2.1)fprintf(stderr,"owned heat GPU mode%u linear%u scenario%u hybrid%u pixel%u,%u channel%u actual%u expected%.4f uv%.8f,%.8f\n",
              mode,linear,scenario,hybrid_mode,x,y,c,actual[at+c],expected,sample.x,sample.y);
          assert(error<=2.1);
        }
        if(a<.5)++transparent;else if(a<254.5)++fractional;++checked;
      }
      if(!scenario)memcpy(off,actual,sizeof(off));
      if(scenario==1){memcpy(heated,actual,sizeof(heated));for(unsigned i=0;i<sizeof(off);++i)if(abs(off[i]-actual[i])>3)++displaced;}
      if(scenario==4)assert(!memcmp(heated,actual,sizeof(heated)));
      if(scenario==7)assert(!memcmp(off,actual,sizeof(off)));
    }
    ArRenderDevice_DestroyTexture(device,texture);
  }
  assert(checked>240000&&transparent>10000&&fractional>10000&&displaced>10000&&ambiguous<2000);
  printf("Owned heat GPU%s: %u analytic pixels, %u holes, %u fractional, %u displaced channels;16/64 slots, repacking, perspective, crops and capture invariance passed\n",
      hybrid_mode?" hybrid":"",checked,transparent,fractional,displaced);
}
