/* Exact resource handoff: BGRA/RGBA, transparent RGB, crops, twelve bands,
 * repeated writes without cycling, and a smaller extent that must erase old
 * padding. --worker also checks main-thread upload -> worker-owned native
 * pack -> main-thread consumption. The semaphore publishes CPU submission,
 * not GPU completion; only the final readback oracle waits for a GPU fence. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "platform/sdl/gpu_frame_handoff_sdl.h"
#include "support/test_assert.h"

enum { W=64, H=48, N=12, Bytes=W*H*4, Total=Bytes*N };

typedef struct PackWorker {
  ArGpuActionScenePass *pass;
  SDL_GPUTexture *atlas;
  SDL_GPUBuffer *motion;
  const ArGpuFramePack *planes;
  SDL_Semaphore *start, *submitted;
  SDL_ThreadID owner;
  bool stop;
} PackWorker;

static int SDLCALL PackOnWorker(void *context) {
  PackWorker *worker = context;
  assert(SDL_GetCurrentThreadID() != worker->owner);
  for (;;) {
    SDL_WaitSemaphore(worker->start);
    if (worker->stop) break;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(worker->pass->device);
    assert(cmd);
    assert(ArGpuFrameHandoff_EncodePack(worker->pass, cmd, worker->atlas,
        W, H*N, worker->motion, worker->planes, N));
    /* Command buffers cannot be handed to another thread for submission. */
    assert(SDL_SubmitGPUCommandBuffer(cmd));
    SDL_SignalSemaphore(worker->submitted);
  }
  return 0;
}

static SDL_GPUTexture *Texture(SDL_GPUDevice *gpu, unsigned height, SDL_GPUTextureFormat format) {
  const SDL_GPUTextureCreateInfo info = {.type=SDL_GPU_TEXTURETYPE_2D, .format=format,
      .width=W, .height=height, .layer_count_or_depth=1, .num_levels=1,
      .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET};
  SDL_GPUTexture *t=SDL_CreateGPUTexture(gpu,&info); assert(t); return t;
}
int main(int argc, char **argv) {
  const bool threaded = argc == 2 && strcmp(argv[1], "--worker") == 0;
  assert(argc == 1 || threaded);
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_GPUDevice *gpu=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL,true,NULL);
  if (!gpu) {SDL_Quit(); return 77;}
  ArGpuActionScenePass pass={0}; assert(ArGpuActionScenePass_Init(&pass,gpu));
  SDL_GPUTexture *sources[N], *destinations[N], *atlas=Texture(gpu,H*N,SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
  for(unsigned i=0;i<N;++i) {
    sources[i]=Texture(gpu,H,i%2 ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    destinations[i]=Texture(gpu,H,SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
  }
  const SDL_GPUBufferCreateInfo mi={.usage=SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,.size=256};
  SDL_GPUBuffer *motion=SDL_CreateGPUBuffer(gpu,&mi);assert(motion);
  SDL_GPUTransferBufferCreateInfo ti={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=Total};
  SDL_GPUTransferBuffer *upload=SDL_CreateGPUTransferBuffer(gpu,&ti);assert(upload);
  ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  SDL_GPUTransferBuffer *download=SDL_CreateGPUTransferBuffer(gpu,&ti);assert(download);
  PackWorker worker = {.pass=&pass, .atlas=atlas, .motion=motion,
      .owner=SDL_GetCurrentThreadID()};
  SDL_Thread *thread = NULL;
  if (threaded) {
    worker.start = SDL_CreateSemaphore(0);
    worker.submitted = SDL_CreateSemaphore(0);
    assert(worker.start && worker.submitted);
    thread = SDL_CreateThread(PackOnWorker, "native-pack-test", &worker);
    assert(thread);
  }
  unsigned char expected[Total];
  const unsigned steps = threaded ? 64 : 3;
  for(unsigned step=0;step<steps;++step) {
    unsigned char *input=SDL_MapGPUTransferBuffer(gpu,upload,true);assert(input);
    for(unsigned i=0;i<N;++i) for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) {
      unsigned at=i*Bytes+(y*W+x)*4;
      input[at]=(unsigned char)(x*3+y+step*47); input[at+1]=(unsigned char)(y*7+x+step*31);
      input[at+2]=(unsigned char)(x*11+y*3+step*13); input[at+3]=(unsigned char)((x+y)%5 ? 193 : 0);
    }
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
    memset(expected,0,sizeof(expected));
    ArGpuFramePack pack[N]; ArGpuFrameUnpack unpack[N];
    for(unsigned i=0;i<N;++i) {
      const bool small = step % 3 == 2;
      int w=(small?23:39)-(int)i, h=(small?21:33), dx=small?17:7;
      pack[i]=(ArGpuFramePack){.source={sources[i],W,H,{3,4,(float)w,(float)h}},.destination={0,(int)i*H,w,h}};
      unpack[i]=(ArGpuFrameUnpack){.source=atlas,.destination=destinations[i],
        .source_width=W,.source_height=H*N,.destination_width=W,.destination_height=H,
        .source_region={0,(int)i*H,w,h},.destination_origin={dx,2},.clear_destination=step%3!=1};
      for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        unsigned a=i*Bytes+((y+4)*W+x+3)*4, b=i*Bytes+((y+2)*W+x+dx)*4;
        expected[b]=input[a+(i%2?2:0)]; expected[b+1]=input[a+1];
        expected[b+2]=input[a+(i%2?0:2)]; expected[b+3]=input[a+3];
      }
    }
    SDL_UnmapGPUTransferBuffer(gpu,upload);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);assert(copy);
    for(unsigned i=0;i<N;++i) {
      const SDL_GPUTextureTransferInfo from={.transfer_buffer=upload,.offset=i*Bytes,.pixels_per_row=W,.rows_per_layer=H};
      const SDL_GPUTextureRegion to={.texture=sources[i],.w=W,.h=H,.d=1};
      SDL_UploadToGPUTexture(copy,&from,&to,false);
    }
    SDL_EndGPUCopyPass(copy);
    if (threaded) {
      assert(SDL_SubmitGPUCommandBuffer(cmd));
      /* These descriptors/resources stay owned until submission is published.
       * Subsequent GPU consumers are submitted in order, without a GPU wait. */
      worker.planes = pack;
      SDL_SignalSemaphore(worker.start);
      cmd = SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
      SDL_WaitSemaphore(worker.submitted);
    } else {
      assert(ArGpuFrameHandoff_EncodePack(&pass,cmd,atlas,W,H*N,motion,pack,N));
    }
    assert(ArGpuFrameHandoff_EncodeUnpack(cmd,unpack,N));
    copy=SDL_BeginGPUCopyPass(cmd);assert(copy);
    for(unsigned i=0;i<N;++i) {
      const SDL_GPUTextureRegion from={.texture=destinations[i],.w=W,.h=H,.d=1};
      const SDL_GPUTextureTransferInfo to={.transfer_buffer=download,.offset=i*Bytes,.pixels_per_row=W,.rows_per_layer=H};
      SDL_DownloadFromGPUTexture(copy,&from,&to);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *f=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);assert(f);
    assert(SDL_WaitForGPUFences(gpu,true,&f,1));SDL_ReleaseGPUFence(gpu,f);
    unsigned char *actual=SDL_MapGPUTransferBuffer(gpu,download,false);assert(actual);
    for(unsigned b=0;b<Total;++b) if(actual[b]!=expected[b]) {
      fprintf(stderr,"step=%u byte=%u actual=%u expected=%u\n",step,b,actual[b],expected[b]);assert(false);
    }
    SDL_UnmapGPUTransferBuffer(gpu,download);
    if (!threaded || step + 1 == steps)
      printf("%s %s handoff: %u steps, twelve planes byte exact, padding zero\n",
          SDL_GetGPUDeviceDriver(gpu), threaded ? "worker" : "main", step + 1);
    cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
    unpack[0].source_region.x=-1;assert(!ArGpuFrameHandoff_EncodeUnpack(cmd,unpack,N));
    unpack[0].source_region.x=0;unpack[1].destination=destinations[0];assert(!ArGpuFrameHandoff_EncodeUnpack(cmd,unpack,N));
    pack[0].destination.x=W;assert(!ArGpuFrameHandoff_EncodePack(&pass,cmd,atlas,W,H*N,motion,pack,N));
    assert(SDL_CancelGPUCommandBuffer(cmd));
  }
  if (threaded) {
    worker.stop = true;
    SDL_SignalSemaphore(worker.start);
    SDL_WaitThread(thread, NULL);
    SDL_DestroySemaphore(worker.start);
    SDL_DestroySemaphore(worker.submitted);
  }
  SDL_ReleaseGPUTransferBuffer(gpu,upload);SDL_ReleaseGPUTransferBuffer(gpu,download);
  SDL_ReleaseGPUBuffer(gpu,motion);SDL_ReleaseGPUTexture(gpu,atlas);
  for(unsigned i=0;i<N;++i){SDL_ReleaseGPUTexture(gpu,sources[i]);SDL_ReleaseGPUTexture(gpu,destinations[i]);}
  ArGpuActionScenePass_Destroy(&pass);SDL_DestroyGPUDevice(gpu);SDL_Quit();return 0;
}
