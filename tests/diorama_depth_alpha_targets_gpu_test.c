/* The established GPU fixture checks the independent pixel oracle. This
 * fixture additionally requires compact and RGBA32F fallback attachments to
 * produce byte-identical output on the same backend, including native actor
 * boundaries, continuation calls, filtering, ties and additive layers. */
#define main DioramaDepthLegacyFixtureMain
#include "diorama_depth_pass_gpu_test.c"
#undef main
#include "platform/sdl/diorama_depth_alpha_sdl.h"
#include "diorama/diorama_effect_backend.h"

typedef bool (*HybridEncode)(ArGpuDioramaDepthPass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *, unsigned, unsigned, const DioramaDepthDraw *,
    SDL_GPUTexture *const *, const ArGpuDioramaDepthMaterial *, unsigned,
    SDL_GPULoadOp, ArRenderColorF, bool);
bool TestDioramaDepthAlphaLegacy_Encode(ArGpuDioramaDepthPass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *, unsigned, unsigned, const DioramaDepthDraw *,
    SDL_GPUTexture *const *, const ArGpuDioramaDepthMaterial *, unsigned,
    SDL_GPULoadOp, ArRenderColorF, bool);
void TestDioramaDepthAlphaLegacy_Destroy(ArGpuDioramaDepthPass *);

static void Capture(HybridEncode encode, ArGpuDioramaDepthPass *p, SDL_GPUTexture *target,
    const DioramaDepthDraw *draws, SDL_GPUTexture *const *textures,
    const ArGpuDioramaDepthMaterial *materials, unsigned split, uint8_t *pixels) {
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(p->device);assert(cmd);
  const unsigned count=6, first=split?split:count;
  assert(encode(p,cmd,target,W,H,draws,textures,materials,first,
      SDL_GPU_LOADOP_CLEAR,(ArRenderColorF){0},true));
  if(split)assert(encode(p,cmd,target,W,H,draws+split,textures+split,
      materials?materials+split:NULL,count-split,SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false));
  /* A successful empty LOAD/no-clear must leave all prior output intact. */
  assert(encode(p,cmd,target,W,H,NULL,NULL,NULL,0,SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},false));
  const SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=W*H*4};
  SDL_GPUTransferBuffer *download=SDL_CreateGPUTransferBuffer(p->device,&info);assert(download);
  SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);assert(copy);
  const SDL_GPUTextureRegion source={.texture=target,.w=W,.h=H,.d=1};
  const SDL_GPUTextureTransferInfo destination={.transfer_buffer=download,.pixels_per_row=W,.rows_per_layer=H};
  SDL_DownloadFromGPUTexture(copy,&source,&destination);SDL_EndGPUCopyPass(copy);
  SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);assert(fence);
  assert(SDL_WaitForGPUFences(p->device,true,&fence,1));SDL_ReleaseGPUFence(p->device,fence);
  const void *actual=SDL_MapGPUTransferBuffer(p->device,download,false);assert(actual);
  memcpy(pixels,actual,W*H*4);
  SDL_UnmapGPUTransferBuffer(p->device,download);SDL_ReleaseGPUTransferBuffer(p->device,download);
}

int main(void) {
  setvbuf(stdout,NULL,_IONBF,0);
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_GPUDevice *gpu=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL|
      SDL_GPU_SHADERFORMAT_DXIL,true,NULL);
  if(!gpu){SDL_Quit();return 77;}
  printf("alpha target parity backend=%s SDL=%d\n",SDL_GetGPUDeviceDriver(gpu),SDL_GetVersion());
  ArGpuDioramaDepthPass compact,legacy;
  assert(ArGpuDioramaDepthPass_Init(&compact,gpu));
  assert(ArGpuDioramaDepthPass_Init(&legacy,gpu));
  SDL_GPUTexture *target=Texture(gpu,W,H,true),*texture=Texture(gpu,TW,TH,false);
  uint8_t art[TW*TH*4];
  for(unsigned y=0;y<TH;++y)for(unsigned x=0;x<TW;++x) {
    uint8_t *pixel=art+(y*TW+x)*4;
    pixel[0]=64+x*40;pixel[1]=224-y*32;pixel[2]=96+y*24;pixel[3]=x==1?0:255;
  }
  Upload(gpu,texture,art);
  uint8_t compact_pixels[W*H*4],legacy_pixels[W*H*4];
  uint64_t hash=UINT64_C(14695981039346656037);
  for(unsigned test=0;test<24;++test) {
    DioramaDepthVertex vertices[6][4];DioramaDepthDraw draws[6];SDL_GPUTexture *textures[6];
    DioramaSceneryLightEffectParams params={.shade={1,1,1,1}};
    ArGpuDioramaDepthMaterial materials[6]={0};
    for(unsigned i=0;i<6;++i) {
      const float left=.1f+i*.12f,right=(test&1)?.8f-i*.12f:left;
      Quad(vertices[i],left,right,(ArRenderColorF){1,.3f+i*.1f,1-i*.1f,i==5?1:.3f});
      draws[i]=Draw(vertices[i]);textures[i]=texture;materials[i].scenery=&params;
      draws[i].nearest=(test&2)==0;
    }
    if(test&4)draws[2].depth_mode=kDioramaDepth_NativeActor;
    if(test&8){draws[4].depth_mode=kDioramaDepth_Read;draws[4].blend=kArRenderBlendMode_Add;}
    if(test%3==1){draws[1].clip_enabled=true;draws[1].clip=(ArRenderRectI){20,12,73,30};}
    if(test%3==2)for(unsigned i=0;i<4;++i)vertices[1][i].clip[2]=vertices[0][i].clip[2];
    const unsigned split=test%3==0?0:test%3==1?1:3;
    const ArGpuDioramaDepthMaterial *bindings=test&16?materials:NULL;
    Capture(ArGpuDioramaDepthPass_EncodeHybridMaterials,&compact,target,draws,textures,bindings,split,compact_pixels);
    Capture(TestDioramaDepthAlphaLegacy_Encode,&legacy,target,draws,textures,bindings,split,legacy_pixels);
    assert(memcmp(compact_pixels,legacy_pixels,sizeof(compact_pixels))==0);
    for(unsigned i=0;i<sizeof(compact_pixels);++i)hash=(hash^compact_pixels[i])*UINT64_C(1099511628211);
  }
  assert(SDL_WaitForGPUIdle(gpu));
  TestDioramaDepthAlphaLegacy_Destroy(&legacy);
  ArGpuDioramaDepthPass_Destroy(&legacy);ArGpuDioramaDepthPass_Destroy(&compact);
  SDL_ReleaseGPUTexture(gpu,texture);SDL_ReleaseGPUTexture(gpu,target);SDL_DestroyGPUDevice(gpu);SDL_Quit();
  printf("alpha target parity: 24 byte-identical pairs, output-fnv1a64=%016llx\n",(unsigned long long)hash);
}
