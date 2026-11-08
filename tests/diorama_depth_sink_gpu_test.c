/* Real SDL/raw-GPU ordering oracle. The caller's arrays and material structs
 * die after each submission; texture contents/targets change between batches.
 * Pixel expectations are authored independently of the collector. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include "platform/sdl/diorama_depth_sink_sdl.h"
#include "platform/sdl/render_sdl_interop.h"
#include "support/test_assert.h"

enum { W=128,H=96 };
static unsigned checks;
static unsigned submit_scopes,submit_ends;
DioramaPerformanceScope DioramaPerformance_Begin(DioramaPerformanceStage stage) {
  assert(stage==kDioramaPerformance_Submit && submit_scopes==submit_ends);
  ++submit_scopes;
  return (DioramaPerformanceScope){.stage=stage,.active=true};
}
void DioramaPerformance_End(DioramaPerformanceScope scope) {
  assert(scope.active && scope.stage==kDioramaPerformance_Submit && submit_scopes==submit_ends+1);
  ++submit_ends;
}
static ArRenderTexture Make(ArRenderDevice *device,bool target,uint32_t color) {
  const ArRenderTextureDesc desc={.width=target?W:1,.height=target?H:1,
      .format=target?kArRenderPixelFormat_Abgr8888:kArRenderPixelFormat_Argb8888,
      .usage=target?kArRenderTextureUsage_Target:kArRenderTextureUsage_Streaming,
      .filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  ArRenderTexture texture;assert(ArRenderDevice_CreateTexture(device,&desc,&texture));
  if(!target)assert(ArRenderDevice_UpdateTexture(device,texture,NULL,&color,4));
  return texture;
}
static SDL_Surface *ReadTarget(ArRenderDevice *device,ArRenderTexture target) {
  assert(ArRenderDevice_SetRenderTarget(device,target));
  assert(ArSdlRenderBackend_SubmitPending(device));
  SDL_Surface *raw=SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(device),NULL);assert(raw);
  SDL_Surface *rgba=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_ARGB8888);assert(rgba);
  SDL_DestroySurface(raw);return rgba;
}
static void CheckPixel(const SDL_Surface *rgba,int x,int y,uint32_t expected) {
  uint32_t actual;memcpy(&actual,(uint8_t *)rgba->pixels+y*rgba->pitch+x*4,4);
  for(unsigned shift=0;shift<32;shift+=8) {
    const int delta=(int)((actual>>shift)&255)-(int)((expected>>shift)&255);
    if(abs(delta)>1)fprintf(stderr,"pixel %d,%d actual=%08x expected=%08x\n",x,y,actual,expected);
    assert(abs(delta)<=1);
  }
  ++checks;
}
static void Check(ArRenderDevice *device,ArRenderTexture target,int x,int y,uint32_t expected) {
  SDL_Surface *rgba=ReadTarget(device,target);
  CheckPixel(rgba,x,y,expected);SDL_DestroySurface(rgba);
}
static bool hybrid;
static const DioramaDepthSink *Begin(ArSdlDioramaDepthSink *state,ArRenderDevice *device,
    ArRenderTexture target) {
  assert(ArRenderDevice_SetRenderTarget(device,target));
  assert(ArRenderDevice_Clear(device,(ArRenderColorF){0,0,0,1}));
  const DioramaDepthSink *sink=hybrid ? ArSdlDioramaDepthSink_BeginHybrid(state,device) :
      ArSdlDioramaDepthSink_Begin(state,device);assert(sink);
  assert(sink->clear(sink->userdata,device));return sink;
}
static bool QuadW(const DioramaDepthSink *sink,ArRenderDevice *device,ArRenderTexture texture,
    float x,float y,float width,float height,float z,ArRenderColorF color,bool actor,
    const DioramaSceneryLightEffectParams *material,const float *clip_w) {
  ArRenderVertex2D vertices[]={{{x,y},color,{0,0}},{{x+width,y},color,{1,0}},
      {{x+width,y+height},color,{1,1}},{{x,y+height},color,{0,1}}};
  float depths[]={z,z,z,z};int32_t indices[]={0,1,2,0,2,3};
  float copied_w[4];if(clip_w)memcpy(copied_w,clip_w,sizeof(copied_w));
  const DioramaDepthSubmission draw={.texture=texture,.vertices=vertices,.depths=depths,
      .indices=indices,.vertex_count=4,.index_count=6,.native_actor=actor,
      .blend=kArRenderBlendMode_Alpha,.lighting=material,.clip_w=clip_w?copied_w:NULL};
  const bool ok=sink->submit(sink->userdata,device,&draw);
  memset(vertices,0,sizeof(vertices));memset(depths,0,sizeof(depths));memset(indices,0,sizeof(indices));
  memset(copied_w,0,sizeof(copied_w));
  return ok;
}
static bool Quad(const DioramaDepthSink *sink,ArRenderDevice *device,ArRenderTexture texture,
    float x,float y,float z,ArRenderColorF color,bool actor,const DioramaSceneryLightEffectParams *material) {
  return QuadW(sink,device,texture,x,y,8,8,z,color,actor,material,NULL);
}
static bool Overlay(ArRenderDevice *device,ArRenderTexture white,float x,float y,ArRenderColorF color) {
  const ArRenderRectF rect={x,y,8,8};
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend|kArRenderDrawState_Tint,
      .blend=kArRenderBlendMode_Opaque,.tint=color};
  return ArRenderDevice_DrawTextureWithState(device,white,NULL,&rect,&state);
}

/* Intentionally no SubmitPending here: the caller proves the boundary before
 * recording native work. Downloading in this command observes the actual GPU
 * order without an SDL readback accidentally submitting delayed drawing. */
static SDL_GPUCommandBuffer *NativeOverlaySnapshot(ArRenderDevice *device,
    ArRenderTexture target,ArRenderTexture color,int x,SDL_GPUTransferBuffer *download) {
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(device);
  SDL_GPUTexture *destination=SDL_GetPointerProperty(
      SDL_GetTextureProperties(ArSdlRenderBackend_UnwrapTexture(target)),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);
  SDL_GPUTexture *source=SDL_GetPointerProperty(
      SDL_GetTextureProperties(ArSdlRenderBackend_UnwrapTexture(color)),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL);
  assert(destination && source);
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);assert(cmd);
  const SDL_GPUBlitInfo blit={.source={.texture=source,.w=1,.h=1},
      .destination={.texture=destination,.x=x,.y=4,.w=8,.h=8},
      .load_op=SDL_GPU_LOADOP_LOAD,.filter=SDL_GPU_FILTER_NEAREST};
  SDL_BlitGPUTexture(cmd,&blit);
  SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);assert(copy);
  const SDL_GPUTextureRegion from={.texture=destination,.w=W,.h=H,.d=1};
  const SDL_GPUTextureTransferInfo to={.transfer_buffer=download,.pixels_per_row=W,.rows_per_layer=H};
  SDL_DownloadFromGPUTexture(copy,&from,&to);
  SDL_EndGPUCopyPass(copy);
  return cmd;
}

static void ExternalSubmissionChecks(ArSdlDioramaDepthSink *state,ArRenderDevice *device,
    ArRenderTexture target,ArRenderTexture white) {
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(device);
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(device);
  ArRenderTexture blue=Make(device,false,0xff0000ff),green=Make(device,false,0xff00ff00);
  SDL_GPUTransferBuffer *download[2];
  const SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=W*H*4};
  for(unsigned i=0;i<2;++i){download[i]=SDL_CreateGPUTransferBuffer(gpu,&info);assert(download[i]);}
  const DioramaDepthSink *sink=Begin(state,device,target);
  /* First native submission must close this queued SDL draw before collecting
   * its depth geometry. The following explicit SubmitPending drains a NONEMPTY
   * depth batch, unlike Finish followed by a later ordinary readback. */
  assert(Overlay(device,white,4,4,(ArRenderColorF){1,0,0,1}));
  assert(Quad(sink,device,white,4,4,.4f,(ArRenderColorF){0,1,0,1},false,NULL));
  assert(Quad(sink,device,white,24,4,.4f,(ArRenderColorF){1,0,0,1},false,NULL));
  assert(state->count==2);
  assert(ArSdlRenderBackend_SubmitPending(device));
  assert(!state->count && state->stats.submissions==1);
  SDL_GPUCommandBuffer *cmd=NativeOverlaySnapshot(device,target,blue,8,download[0]);
  assert(SDL_SubmitGPUCommandBuffer(cmd));

  /* A later raw SDL draw arrives after the certified boundary. Its following
   * empty external callback must not reuse that certificate: native green must
   * be submitted AFTER this yellow rectangle, with no intermediate CPU wait. */
  const SDL_FRect rectangle={10,4,8,8};
  assert(SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE));
  assert(SDL_SetRenderDrawColor(renderer,255,255,0,255));
  assert(SDL_RenderFillRect(renderer,&rectangle));
  assert(ArSdlRenderBackend_SubmitPending(device));
  assert(state->stats.submissions==1);
  cmd=NativeOverlaySnapshot(device,target,green,12,download[1]);
  SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);assert(fence);
  assert(SDL_WaitForGPUFences(gpu,true,&fence,1));SDL_ReleaseGPUFence(gpu,fence);
  assert(sink->finish(sink->userdata,device,true));
  for(unsigned snapshot=0;snapshot<2;++snapshot) {
    const uint8_t *pixels=SDL_MapGPUTransferBuffer(gpu,download[snapshot],false);assert(pixels);
    uint64_t hash=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<W*H*4;++i)hash=(hash^pixels[i])*UINT64_C(1099511628211);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
      uint32_t expected=0xff000000;
      if(y>=4 && y<12) {
        if(x>=4 && x<12)expected=0xff00ff00;
        if(x>=24 && x<32)expected=0xffff0000;
        if(x>=8 && x<16)expected=0xff0000ff;
        if(snapshot && x>=10 && x<18)expected=0xffffff00;
        if(snapshot && x>=12 && x<20)expected=0xff00ff00;
      }
      const uint8_t *actual=pixels+(y*W+x)*4;
      assert(actual[0]==((expected>>16)&255) && actual[1]==((expected>>8)&255) &&
          actual[2]==(expected&255) && actual[3]==255);
      ++checks;
    }
    printf("external-submit-snapshot=%u hybrid=%u rgba-fnv=%016llx\n",snapshot,
        hybrid?1u:0u,(unsigned long long)hash);
    SDL_UnmapGPUTransferBuffer(gpu,download[snapshot]);SDL_ReleaseGPUTransferBuffer(gpu,download[snapshot]);
  }
  ArRenderDevice_DestroyTexture(device,blue);ArRenderDevice_DestroyTexture(device,green);
}

static uint32_t GrainColor(unsigned x) {
  return 0xff000000u|((x*13u)<<16)|(((15-x)*11u)<<8)|(x*7u);
}
static uint32_t GrainSample(double u,bool linear) {
  if(!linear)return GrainColor((unsigned)floor(u*16));
  /* The existing crisp source repeats each native texel four times before
   * linear filtering. Compute its two taps without using renderer math. */
  const double at=u*64-.5,t=at-floor(at);
  const int a=(int)floor(at),b=a+1;
  const uint32_t ca=GrainColor((unsigned)(a<0?0:a>63?63:a)/4);
  const uint32_t cb=GrainColor((unsigned)(b<0?0:b>63?63:b)/4);
  uint32_t pixel=0xff000000u;
  for(unsigned shift=0;shift<24;shift+=8)
    pixel|=(uint32_t)lround(((ca>>shift)&255)*(1-t)+((cb>>shift)&255)*t)<<shift;
  return pixel;
}
static void PerspectiveChecks(ArSdlDioramaDepthSink *state,ArRenderDevice *device,
    ArRenderTexture target,ArRenderTexture white) {
  const float ones[]={1,1,1,1},weights[]={1,4,4,1};
  const ArRenderColorF full={1,1,1,1},red={1,0,0,1},green={0,1,0,1},blue={0,0,1,1};
  for(unsigned linear=0;linear<2;++linear) {
    const int width=linear?64:16,height=linear?4:1;
    uint32_t art[64*4];
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)art[y*width+x]=GrainColor(x/(linear?4:1));
    ArRenderTexture texture;
    assert(ArRenderDevice_CreateTexture(device,&(ArRenderTextureDesc){.width=width,.height=height,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming,
        .filter=linear?kArRenderFilter_Linear:kArRenderFilter_Nearest,
        .blend=kArRenderBlendMode_Alpha},&texture));
    assert(ArRenderDevice_UpdateTexture(device,texture,NULL,art,width*4));
    DioramaDepthVertex legacy_vertices[4];SDL_Surface *legacy=NULL;
    for(unsigned mode=0;mode<3;++mode) {
      const DioramaDepthSink *sink=Begin(state,device,target);
      assert(QuadW(sink,device,texture,16,16,96,64,.5f,full,false,NULL,
          mode==0?NULL:mode==1?ones:weights));
      assert(state->count==1 && state->vertex_count==4);
      if(!mode)memcpy(legacy_vertices,state->vertices,sizeof(legacy_vertices));
      else if(mode==1)assert(!memcmp(legacy_vertices,state->vertices,sizeof(legacy_vertices)));
      assert(sink->finish(sink->userdata,device,true));
      SDL_Surface *pixels=ReadTarget(device,target);
      if(!mode)legacy=pixels;
      else if(mode==1) {
        assert(legacy->pitch==pixels->pitch);
        assert(!memcmp(legacy->pixels,pixels->pixels,(size_t)pixels->pitch*pixels->h));
      }
      unsigned differs=0;
      for(int y=24;y<=72;y+=24)for(int x=20;x<=108;x+=8) {
        const double s=(x+.5-16)/96;
        /* Both triangles have W=1 on the left and W=4 on the right.
         * Rational interpolation gives u=(s/4)/((1-s)+s/4). */
        const double u=mode==2?(s/4)/((1-s)+s/4):s;
        const uint32_t expected=GrainSample(u,linear);
        CheckPixel(pixels,x,y,expected);differs+=expected!=GrainSample(s,linear);
      }
      if(mode==2)assert(differs>24); /* This oracle cannot pass with affine UVs. */
      if(mode)SDL_DestroySurface(pixels);
    }
    SDL_DestroySurface(legacy);ArRenderDevice_DestroyTexture(device,texture);
  }
  /* Invalid opt-ins cannot alter a queued valid draw or an actor's native
   * priority contract. The W array is borrowed only until submit returns. */
  const float invalid[]={NAN,INFINITY,0,-1};
  for(unsigned i=0;i<5;++i) {
    float w[]={1,4,4,1};if(i<4)w[i]=invalid[i];
    const DioramaDepthSink *sink=Begin(state,device,target);
    assert(Quad(sink,device,white,4,4,.2f,green,false,NULL));
    assert(!QuadW(sink,device,white,4,4,8,8,.1f,red,i==4,NULL,w));
    assert(state->count==1 && state->vertex_count==4);
    assert(sink->finish(sink->userdata,device,true));Check(device,target,8,8,0xff00ff00);
  }
  /* Identical screen depth still uses later native order, including equal-
   * depth fractional coverage in the hybrid transparency path. */
  const ArRenderColorF half_red={1,0,0,.5f};
  for(unsigned reverse=0;reverse<2;++reverse) {
    const DioramaDepthSink *sink=Begin(state,device,target);
    assert(QuadW(sink,device,white,4,4,8,8,.5f,reverse?half_red:blue,false,NULL,weights));
    assert(QuadW(sink,device,white,4,4,8,8,.5f,reverse?blue:half_red,false,NULL,weights));
    assert(sink->finish(sink->userdata,device,true));
    Check(device,target,8,8,reverse?0xff0000ff:0xff800080);
  }
  for(unsigned foreground=0;foreground<2;++foreground) {
    const DioramaDepthSink *sink=Begin(state,device,target);
    assert(QuadW(sink,device,white,4,4,8,8,.2f,red,false,NULL,weights));
    assert(Quad(sink,device,white,4,4,.8f,blue,true,NULL));
    if(foreground)assert(QuadW(sink,device,white,4,4,8,8,.9f,green,false,NULL,weights));
    assert(sink->finish(sink->userdata,device,true));
    Check(device,target,8,8,foreground?0xff00ff00:0xff0000ff);
  }
}

int main(int argc,char **argv) {
  hybrid=argc==2 && !strcmp(argv[1],"--hybrid");
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_Window *window=SDL_CreateWindow("Deferred depth ordering",W,H,SDL_WINDOW_HIDDEN);
  ArRenderDevice device={0};
  if(!window || !ArSdlRenderBackend_CreateForWindow(&device,window,NULL))return 77;
  ArRenderTexture target=Make(&device,true,0),other=Make(&device,true,0);
  ArRenderTexture white=Make(&device,false,0xffffffff),black=Make(&device,false,0xff000000);
  ArRenderTexture mutable=Make(&device,false,0xffff0000);
  const ArRenderColorF red={1,0,0,1},green={0,1,0,1},blue={0,0,1,1},full={1,1,1,1};
  ArSdlDioramaDepthSink state={0};
  SDL_Texture *bound=SDL_GetRenderTarget(ArSdlRenderBackend_Renderer(&device));
  assert(!ArSdlDioramaDepthSink_PrepareTarget(&state,&device,white,1024,1536));
  assert(!ArSdlDioramaDepthSink_PrepareTarget(&state,&device,target,kDioramaDepthMaximumVertices+1,1536));
  assert(ArSdlDioramaDepthSink_PrepareTarget(&state,&device,target,1024,1536));
  assert(ArSdlDioramaDepthSink_PrepareTarget(&state,&device,other,1024,1536));
  assert(ArSdlDioramaDepthSink_PrepareTarget(&state,&device,ArRenderTexture_Invalid(),1024,1536));
  assert(state.targets[0].target!=state.targets[1].target && !state.owner);
  assert(SDL_GetRenderTarget(ArSdlRenderBackend_Renderer(&device))==bound);
  const ArGpuDioramaDepthPass prepared[3]={state.targets[0].pass,state.targets[1].pass,state.targets[2].pass};
  const DioramaDepthVertex *prepared_vertices=state.vertices;
  const int32_t *prepared_indices=state.indices;
  assert(ArSdlDioramaDepthSink_PrepareTarget(&state,&device,target,1024,1536));
  assert(!memcmp(&state.targets[0].pass,&prepared[0],sizeof(prepared[0])));
  const DioramaDepthSink *sink=Begin(&state,&device,target);
  assert(!ArSdlDioramaDepthSink_PrepareTarget(&state,&device,target,1024,1536));
  for(unsigned i=0;i<80;++i) {
    DioramaSceneryLightEffectParams p={.illumination=black,.shade={1,1,1,1},
        .grade={.strength=1,.saturation=0,.shadows=i%2?0x00ff00:0xff0000,
            .highlights=i%2?0x00ff00:0xff0000}};
    assert(Quad(sink,&device,white,4+i%10*10,4+i/10*10,.4f,full,false,&p));
    memset(&p,0,sizeof(p));
  }
  assert(state.count==80 && state.stats.submissions==0);
  assert(submit_scopes==0 && submit_ends==0);
  ArSdlDioramaDepthSink competing={0};
  assert(!ArSdlDioramaDepthSink_Begin(&competing,&device));
  assert(state.count==80); /* Rejected ownership cannot drain or steal work. */
  assert(Overlay(&device,white,4,4,blue));
  assert(state.count==0 && state.stats.submissions==1 && state.stats.largest_batch==80);
  assert(submit_scopes==1 && submit_ends==1);
  assert(sink->finish(sink->userdata,&device,true));assert(!state.owner);
  for(unsigned i=0;i<80;++i)Check(&device,target,8+i%10*10,8+i/10*10,
      i==0?0xff0000ff:i%2?0xff00ff00:0xffff0000);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,mutable,4,4,.3f,full,false,NULL));
  uint32_t pixel=0xff00ff00;
  assert(ArRenderDevice_UpdateTexture(&device,mutable,NULL,&pixel,4));
  assert(state.stats.submissions==1);
  assert(Quad(sink,&device,mutable,24,4,.3f,full,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));
  Check(&device,target,8,8,0xffff0000);Check(&device,target,28,8,0xff00ff00);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(ArRenderDevice_SetRenderTarget(&device,other));
  assert(ArRenderDevice_Clear(&device,(ArRenderColorF){0,0,0,1}));
  assert(sink->clear(sink->userdata,&device));
  assert(Quad(sink,&device,white,4,4,.3f,green,false,NULL));
  assert(ArRenderDevice_SetRenderTarget(&device,target));
  assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));
  Check(&device,target,8,8,0xffff0000);Check(&device,other,8,8,0xff00ff00);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(ArRenderDevice_Clear(&device,(ArRenderColorF){0,0,0,1}));
  assert(sink->clear(sink->userdata,&device));
  assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));Check(&device,target,8,8,0xff0000ff);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,mutable,4,4,.2f,full,false,NULL));
  ArRenderDevice_DestroyTexture(&device,mutable);mutable=ArRenderTexture_Invalid();
  assert(state.stats.submissions==1);
  assert(sink->finish(sink->userdata,&device,true));Check(&device,target,8,8,0xff00ff00);

  if (hybrid) {
    /* The reported overlap with filtered coverage: a nearer partial world
     * fragment must transmit the farther opaque surface even when submitted
     * first. Both submission orders have the same independent expectation. */
    const ArRenderColorF half_red={1,0,0,.5f};
    for(unsigned reverse=0;reverse<2;++reverse) {
      sink=Begin(&state,&device,target);
      if(reverse) assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
      assert(Quad(sink,&device,white,4,4,.2f,half_red,false,NULL));
      if(!reverse) assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
      assert(sink->finish(sink->userdata,&device,true));
      Check(&device,target,8,8,0xff800080);
    }
    /* Ordinary effect draws keep their authored boundary; no pending
     * transparency can escape and cover the later SDL overlay. */
    sink=Begin(&state,&device,target);
    assert(Quad(sink,&device,white,4,4,.2f,half_red,false,NULL));
    assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
    assert(Overlay(&device,white,4,4,green));
    assert(sink->finish(sink->userdata,&device,true));
    Check(&device,target,8,8,0xff00ff00);
  }

  sink=Begin(&state,&device,target);
  for(unsigned i=0;i<kDioramaDepthMaximumDraws+1;++i)
    assert(Quad(sink,&device,white,4,4,.2f,i==kDioramaDepthMaximumDraws?blue:red,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));
  assert(state.stats.draws==kDioramaDepthMaximumDraws+1 && state.stats.submissions==2);
  Check(&device,target,8,8,0xff0000ff);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(Quad(sink,&device,white,4,4,.8f,blue,true,NULL));
  assert(Quad(sink,&device,white,4,4,.9f,green,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));
  assert(state.stats.submissions==1);Check(&device,target,8,8,0xff00ff00);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(sink->finish(sink->userdata,&device,false));
  assert(state.stats.submissions==0);Check(&device,target,8,8,0xff000000);

  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(Quad(sink,&device,white,4,4,NAN,blue,false,NULL));
  assert(!Overlay(&device,white,4,4,green));
  assert(!sink->finish(sink->userdata,&device,true));
  assert(!state.owner && !state.count && state.stats.submissions==0);
  Check(&device,target,8,8,0xff000000);
  sink=Begin(&state,&device,target);
  assert(Quad(sink,&device,white,4,4,.2f,green,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));Check(&device,target,8,8,0xff00ff00);

  /* The logical output uses the swapchain's format (BGRA on Metal), unlike
   * the RGBA offscreen targets; preparation must retain the right pipelines. */
  sink=Begin(&state,&device,ArRenderTexture_Invalid());
  assert(Quad(sink,&device,white,4,4,.2f,red,false,NULL));
  assert(Quad(sink,&device,white,4,4,.8f,blue,false,NULL));
  assert(sink->finish(sink->userdata,&device,true));
  Check(&device,ArRenderTexture_Invalid(),8,8,0xffff0000);

  ExternalSubmissionChecks(&state,&device,target,white);
  PerspectiveChecks(&state,&device,target,white);

  assert(submit_scopes==submit_ends && submit_scopes>10);
  assert(state.vertices==prepared_vertices && state.indices==prepared_indices);
  for(unsigned i=0;i<3;++i) {
    const ArGpuDioramaDepthPass *current=&state.targets[i].pass;
    assert(current->vertices==prepared[i].vertices && current->indices==prepared[i].indices &&
        current->upload==prepared[i].upload && current->depth==prepared[i].depth);
    assert(!memcmp(current->pipelines,prepared[i].pipelines,sizeof(current->pipelines)));
  }
  ArSdlDioramaDepthSink_Reset(&state);
  assert(ArRenderDevice_SetRenderTarget(&device,ArRenderTexture_Invalid()));
  const ArRenderTexture resources[]={target,other,white,black};
  for(unsigned i=0;i<sizeof(resources)/sizeof(*resources);++i)ArRenderDevice_DestroyTexture(&device,resources[i]);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();
  printf("Deferred depth: %u independent pixel checks; perspective UVs with native/crisp4x art, invalid W and affine parity, prepared resources, copied draw/material state, SDL overlay, texture update/destruction, targets, clears, capacity, native priority, measured drains and failure recovery passed\n",checks);
  return 0;
}
