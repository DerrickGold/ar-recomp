/* Check actual blended pixels, not just draw commands. Native full-add must
 * add one opaque subscreen winner even when Voxel geometry overlaps itself. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "action/action_voxel_internal.h"
#include "diorama/diorama.h"
#include "platform/sdl/render_sdl_interop.h"
#include "platform/sdl/diorama_depth_sink_sdl.h"
#include "support/test_assert.h"

static uint32_t pixels[SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT];
static ArRenderTexture Skybox(void *context,ArRenderDevice *device,int source,
    bool fill,uint32_t color,bool *failed) {
  (void)device;(void)source;(void)fill;(void)color;*failed=false;
  return *(const ArRenderTexture *)context;
}
static void Fill(ArRenderDevice *device,ArRenderTexture texture,uint32_t color) {
  for(unsigned i=0;i<sizeof(pixels)/sizeof(*pixels);++i)pixels[i]=color;
  assert(ArRenderDevice_UpdateTexture(device,texture,NULL,pixels,SR_PPU_SURFACE_MAX_WIDTH*4));
}
static void Check(SDL_Surface *surface,int x,int y,unsigned expected) {
  const uint32_t actual=((uint32_t *)((uint8_t *)surface->pixels+y*surface->pitch))[x];
  for(unsigned shift=0;shift<24;shift+=8) {
    const int difference=(int)((actual>>shift)&255)-(int)((expected>>shift)&255);
    if(abs(difference)>1)fprintf(stderr,"pixel %d,%d: %08x expected %08x\n",x,y,actual,expected);
    assert(abs(difference)<=1);
  }
}
static bool (*finish_original)(void *,ArRenderDevice *,bool);
static bool RejectFinish(void *context,ArRenderDevice *device,bool success) {
  assert(finish_original(context,device,success));
  return false;
}
int main(void) {
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_Window *window=SDL_CreateWindow("Voxel subscreen pixel oracle",512,384,SDL_WINDOW_HIDDEN);
  ArRenderDevice device={0};
  if(!window || !ArSdlRenderBackend_CreateForWindow(&device,window,NULL))return 77;
  ArRenderTexture textures[kDioramaPlane_Count]={0};
  const uint8_t *present[kDioramaPlane_Count]={0};
  const unsigned planes[]={SR_PPU_OVERLAY_BG2,SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Far,
      kDioramaPlane_Obj2,kDioramaPlane_Bg1Hi,SR_PPU_OVERLAY_BG3};
  const uint32_t colors[]={0xff080c10,0xff0a141e,0xff3c3228,0xff501e0a,0xff05465a,0xffaaa096};
  const ArRenderTextureDesc desc={.width=SR_PPU_SURFACE_MAX_WIDTH,.height=SR_PPU_SURFACE_MAX_HEIGHT,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming,
      .filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  for(unsigned i=0;i<6;++i)assert(ArRenderDevice_CreateTexture(&device,&desc,&textures[planes[i]]));
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=5;voxel->active_room=1;voxel->profile=kActionVoxelProfile_MarahnaJungle;
  voxel->settings=ActionVoxel_SettingsForRoom(5,1);voxel->width=256;voxel->height=224;voxel->ready=true;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=present};
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.priority_surface=true};
  const DioramaView view={.viewport={95,79,256,224},.visible_width=256,.distance_scale=1};
  DioramaScene scene={.render=&options,.map_group=5,.map_number=1,.voxel_geometry=voxel,
      .additive_plane_mask=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Far)|
          (1u<<kDioramaPlane_Obj2)|(1u<<kDioramaPlane_Bg1Hi)};
  ArSdlDioramaDepthSink depth={0};
  /* Opaque front, actor, high native tile, non-math HUD, half-alpha front,
   * half-alpha lone addend, and ordinary non-additive rendering. */
  const unsigned expected[]={0x443e38,0x582a1a,0x0d526a,0xaaa096,0x2b2f33,0x0d161f,0x3c3228,0x2b2f33,0x443e38,0x0d161f};
  const DioramaResolvedLayer background={.plane=kDioramaPlane_Backdrop,.alpha=255,
      .source=DioramaLayerOrder_ActionBgSource(5,1,1)};
  for(unsigned shared=0;shared<3;++shared)
  for(unsigned test=0;test<10;++test)for(unsigned repeat=0;repeat<2;++repeat) {
    options.skybox=kDioramaSky_Off;options.resolved_layers=NULL;options.resolved_layer_count=0;
    options.resolve_skybox=NULL;
    scene.depth_sink=shared==2?ArSdlDioramaDepthSink_BeginHybrid(&depth,&device):
        shared?ArSdlDioramaDepthSink_Begin(&depth,&device):NULL;
    assert(!shared || scene.depth_sink);
    for(unsigned i=0;i<6;++i) {
      Fill(&device,textures[planes[i]],colors[i]);
      present[planes[i]]=i<3 || (test>=1 && test<=3 && i<=test+2)?(const uint8_t *)pixels:NULL;
    }
    scene.additive_plane_mask=test==6?0:(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Far)|
        (1u<<kDioramaPlane_Obj2)|(1u<<kDioramaPlane_Bg1Hi);
    if(test==4)Fill(&device,textures[kDioramaPlane_Bg1Far],0x803c3228);
    if(test==5 || (test>=7 && test<=9)) {
      present[kDioramaPlane_Bg1Far]=NULL;
      Fill(&device,textures[SR_PPU_OVERLAY_BG1],0x800a141e);
    }
    if(test>=7 && test<=9) {
      options.skybox=kDioramaSky_Only;options.resolved_layers=&background;options.resolved_layer_count=1;
      options.resolve_skybox=test==9?NULL:Skybox;options.skybox_userdata=&textures[kDioramaPlane_Bg1Far];
      if(test==8)present[SR_PPU_OVERLAY_BG1]=NULL;
    }
    DioramaProjection projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid);
    SDL_Surface *raw=SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&device),NULL);assert(raw);
    SDL_Surface *result=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_ARGB8888);assert(result);
    for(int y=180;y<200;++y)for(int x=212;x<232;++x)Check(result,x,y,expected[test]);
    Check(result,10,10,0);
    SDL_DestroySurface(result);SDL_DestroySurface(raw);
  }
  /* Even an early invalid-view exit must release the external batch scope.
   * A finish failure must invalidate the projection and reach the host. */
  DioramaView invalid=view;invalid.viewport.w=0;
  scene.depth_sink=ArSdlDioramaDepthSink_Begin(&depth,&device);assert(scene.depth_sink);
  DioramaProjection final_projection;
  assert(!PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&invalid,&scene,&final_projection)));
  assert(!depth.owner && !final_projection.valid);
  scene.depth_sink=ArSdlDioramaDepthSink_Begin(&depth,&device);assert(scene.depth_sink);
  finish_original=depth.sink.finish;depth.sink.finish=RejectFinish;
  assert(!PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&final_projection)));
  assert(!depth.owner && !final_projection.valid);
  scene.depth_sink=ArSdlDioramaDepthSink_Begin(&depth,&device);assert(scene.depth_sink);
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&final_projection)));
  assert(!depth.owner && final_projection.valid);
  Diorama_ResetCompositorResources(&device);ActionVoxel_Release(voxel);
  ArSdlDioramaDepthSink_Reset(&depth);
  for(unsigned i=0;i<6;++i)ArRenderDevice_DestroyTexture(&device,textures[planes[i]]);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();
  puts("Voxel subscreen: 60 pixel-checked renders, 24000 blended pixels; legacy/cutout/hybrid depth, actor/high priority, HUD, alpha, named/missing backdrop, offset viewport, finish failure and recovery passed");
}
