/* Reuse the depth fixture's upload/readback geometry and independent native
 * priority oracle. Its original entry point is intentionally not executed. */
#define main DioramaDepthLegacyFixtureMain
#include "diorama_depth_pass_gpu_test.c"
#undef main
#include "platform/sdl/diorama_depth_alpha_sdl.h"
#include "diorama/diorama_effect_backend.h"
#include <stdlib.h>

static void SortedReference(uint8_t *out,const DioramaDepthDraw *d,
    const uint8_t *const *textures,unsigned count) {
  for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x) {
    const float u=(x+.5f-8)/112,v=(y+.5f-8)/48;
    float color[4]={0};
    if(u>0&&u<1&&v>0&&v<1) {
      struct Fragment {float z,c[4];unsigned order;bool add;} f[16];unsigned n=0;
      for(unsigned j=0;j<count;++j) {
        if(d[j].clip_enabled&&(x<(unsigned)d[j].clip.x||y<(unsigned)d[j].clip.y||
            x>=(unsigned)(d[j].clip.x+d[j].clip.w)||y>=(unsigned)(d[j].clip.y+d[j].clip.h)))continue;
        struct Fragment a={.z=d[j].vertices[0].clip[2]*(1-u)+d[j].vertices[1].clip[2]*u,
          .order=j,.add=d[j].blend==kArRenderBlendMode_Add};
        const ArRenderColorF c=d[j].vertices[0].color;
        a.c[0]=c.r;a.c[1]=c.g;a.c[2]=c.b;a.c[3]=c.a;
        if(d[j].textured) {
          const uint8_t *t=textures[j]+((unsigned)(v*TH)*TW+(unsigned)(u*TW))*4;
          for(unsigned k=0;k<4;++k)a.c[k]*=t[k]/255.f;
        }
        if(a.c[3]<=.5f/255)continue;
        unsigned at=n++;
        while(at&&(f[at-1].z<a.z||(f[at-1].z==a.z&&f[at-1].order>a.order))) {
          f[at]=f[at-1];--at;
        }
        f[at]=a;
      }
      for(unsigned j=0;j<n;++j) {
        const float keep=f[j].add?1:1-f[j].c[3];
        for(unsigned k=0;k<3;++k)color[k]=f[j].c[k]*f[j].c[3]+color[k]*keep;
        if(!f[j].add)color[3]=f[j].c[3]+color[3]*keep;
        for(unsigned k=0;k<4;++k)color[k]=roundf(fminf(1,color[k])*255)/255;
      }
    }
    for(unsigned k=0;k<4;++k)out[(y*W+x)*4+k]=(uint8_t)lroundf(color[k]*255);
  }
}

static void HybridCheck(ArGpuDioramaDepthPass *p,SDL_GPUTexture *target,
    const DioramaDepthDraw *draws,SDL_GPUTexture *const *textures,
    const uint8_t *const *pixels,unsigned count,bool native,bool material,unsigned split) {
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(p->device);assert(cmd);
  DioramaSceneryLightEffectParams params={.shade={1,1,1,1}};
  ArGpuDioramaDepthMaterial materials[16]={0};
  if(material)for(unsigned i=0;i<count;++i)materials[i].scenery=&params;
  const unsigned first=split?split:count;
  assert(ArGpuDioramaDepthPass_EncodeHybridMaterials(p,cmd,target,W,H,draws,textures,
      material?materials:NULL,first,SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
  if(split)assert(ArGpuDioramaDepthPass_EncodeHybridMaterials(p,cmd,target,W,H,draws+split,textures+split,
      material?materials+split:NULL,count-split,SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false));
  const SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=W*H*4};
  SDL_GPUTransferBuffer *download=SDL_CreateGPUTransferBuffer(p->device,&info);assert(download);
  SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);assert(copy);
  const SDL_GPUTextureRegion source={.texture=target,.w=W,.h=H,.d=1};
  const SDL_GPUTextureTransferInfo destination={.transfer_buffer=download,.pixels_per_row=W,.rows_per_layer=H};
  SDL_DownloadFromGPUTexture(copy,&source,&destination);SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);assert(fence);
  assert(SDL_WaitForGPUFences(p->device,true,&fence,1));SDL_ReleaseGPUFence(p->device,fence);
  const uint8_t *actual=SDL_MapGPUTransferBuffer(p->device,download,false);assert(actual);
  uint8_t expected[W*H*4];
  if(native)Reference(expected,draws,pixels,count);else SortedReference(expected,draws,pixels,count);
  for(unsigned i=0;i<sizeof(expected);++i) {
    const int delta=(int)actual[i]-expected[i];
    if(abs(delta)>2)fprintf(stderr,"hybrid=%u pixel=%u,%u channel=%u actual=%u expected=%u\n",
      renders,i/4%W,i/4/W,i%4,actual[i],expected[i]);
    assert(abs(delta)<=2);
  }
  SDL_UnmapGPUTransferBuffer(p->device,download);SDL_ReleaseGPUTransferBuffer(p->device,download);
  ++renders;checked_pixels+=W*H;
}

int main(void) {
  setvbuf(stdout,NULL,_IONBF,0);
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_GPUDevice *gpu=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL|SDL_GPU_SHADERFORMAT_DXIL,true,NULL);
  if(!gpu){SDL_Quit();return 77;}
  printf("bounded world transparency backend=%s SDL=%d\n",SDL_GetGPUDeviceDriver(gpu),SDL_GetVersion());
  ArGpuDioramaDepthPass p;assert(ArGpuDioramaDepthPass_Init(&p,gpu));
  assert(ArGpuDioramaDepthPass_PrepareHybrid(&p,W,H,64,96));
  struct ArGpuDioramaDepthAlpha *retained=p.alpha;
  assert(ArGpuDioramaDepthPass_PrepareHybrid(&p,W,H,64,96)&&p.alpha==retained);
  SDL_GPUTexture *target=Texture(gpu,W,H,true),*texture=Texture(gpu,TW,TH,false);
  uint8_t art[TW*TH*4];memset(art,255,sizeof(art));
  for(unsigned y=0;y<TH;++y)art[(y*TW+1)*4+3]=0;
  Upload(gpu,texture,art);
  DioramaDepthVertex vertices[8][4];DioramaDepthDraw draws[8];
  SDL_GPUTexture *textures[8];const uint8_t *pixels[8];
  for(unsigned i=0;i<8;++i){textures[i]=texture;pixels[i]=art;}
  /* The rejected opaque-only diagnostic: near half-red arrives before opaque
   * far blue. Include crossing faces, holes and source-order permutations. */
  for(unsigned crossing=0;crossing<2;++crossing) {
    Quad(vertices[0],.15f,crossing?.8f:.15f,(ArRenderColorF){1,0,0,.5f});
    Quad(vertices[1],.75f,crossing?.2f:.75f,(ArRenderColorF){0,0,1,.5f});
    Quad(vertices[2],.9f,.9f,(ArRenderColorF){0,1,0,1});
    for(unsigned i=0;i<3;++i)draws[i]=Draw(vertices[i]);
    for(unsigned order=0;order<6;++order) {
      DioramaDepthDraw d[3];for(unsigned i=0;i<3;++i)d[i]=draws[orders[order][i]];
      HybridCheck(&p,target,d,textures,pixels,3,false,false,0);
      HybridCheck(&p,target,d,textures,pixels,3,false,true,0);
    }
  }
  /* Equal depths use later native source order, including alpha/opaque ties. */
  for(unsigned opaque=0;opaque<2;++opaque)for(unsigned reverse=0;reverse<2;++reverse) {
    Quad(vertices[0],.5f,.5f,(ArRenderColorF){1,0,0,.5f});
    Quad(vertices[1],.5f,.5f,(ArRenderColorF){0,0,1,opaque?1:.5f});
    DioramaDepthDraw d[]={Draw(vertices[reverse]),Draw(vertices[1-reverse])};
    HybridCheck(&p,target,d,textures,pixels,2,false,false,0);
  }
  /* Seven same-colored fractional layers prove the residual tail is present;
   * their compositing result is independent of the tail's approximate order. */
  for(unsigned i=0;i<7;++i){Quad(vertices[i],.1f+i*.1f,.1f+i*.1f,(ArRenderColorF){1,.25f,0,.25f});draws[i]=Draw(vertices[i]);}
  HybridCheck(&p,target,draws,textures,pixels,7,false,false,0);
  /* Additive light retains alpha and depth behavior between fractional faces. */
  draws[1].blend=kArRenderBlendMode_Add;draws[1].depth_mode=kDioramaDepth_Read;
  HybridCheck(&p,target,draws,textures,pixels,3,false,false,0);
  /* Native holes retain prior opaque depth; covered actor pixels preserve the
   * ordinary painter slot even for partial actor alpha. */
  Quad(vertices[0],.2f,.2f,(ArRenderColorF){1,0,0,1});
  Quad(vertices[1],.7f,.7f,(ArRenderColorF){0,0,1,.5f});
  Quad(vertices[2],.6f,.6f,(ArRenderColorF){0,1,0,1});
  for(unsigned i=0;i<3;++i)draws[i]=Draw(vertices[i]);
  draws[0].textured=draws[2].textured=false;draws[1].depth_mode=kDioramaDepth_NativeActor;
  HybridCheck(&p,target,draws,textures,pixels,3,true,false,0);
  HybridCheck(&p,target,draws,textures,pixels,3,true,false,1);
  assert(SDL_WaitForGPUIdle(gpu));ArGpuDioramaDepthPass_Destroy(&p);assert(!p.alpha);
  SDL_ReleaseGPUTexture(gpu,texture);SDL_ReleaseGPUTexture(gpu,target);SDL_DestroyGPUDevice(gpu);SDL_Quit();
  printf("bounded world transparency: %u renders, %u oracle pixels; fractional crossing, material coverage, native priority, stable ties, additive light and visible residual tail passed\n",renders,checked_pixels);
}
