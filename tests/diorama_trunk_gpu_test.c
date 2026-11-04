#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "action/action_trunk.h"
#include "diorama/diorama_trunk_gpu.h"
#include "present/present.h"
#include "platform/sdl/render_sdl_interop.h"
#include "support/test_assert.h"

static ActionTrunkAssets assets;
static FrameSlot frame;
static uint32_t input[3][640*352],expected[3][640*352];
int main(void) {
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_Window *window=SDL_CreateWindow("Trunk split parity",64,64,SDL_WINDOW_HIDDEN);
  ArRenderDevice device={0};
  if(!window||!ArSdlRenderBackend_CreateForWindow(&device,window,NULL))return 77;
  assert(DioramaTrunkGpu_Prepare(&device));
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(&device);
  SDL_Texture *target=SDL_GetRenderTarget(renderer);
  const unsigned planes[3]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  const uint32_t mask=(1u<<planes[0])|(1u<<planes[1])|(1u<<planes[2]);
  ArRenderTexture raw[3];
  const ArRenderTextureDesc desc={.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming,.filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Alpha};
  const uint32_t *src[3];uint32_t *dst[3];const size_t pitches[3]={640,640,640};
  for(unsigned band=0;band<3;++band) {
    for(unsigned i=0;i<640*352;++i)if(i%3==band)input[band][i]=0xff000000u|((i*173+(band+1)*79013)&0xffffffu);
    src[band]=input[band];dst[band]=expected[band];
    assert(ArRenderDevice_CreateTexture(&device,&desc,&raw[band]));
    assert(ArRenderDevice_UpdateTexture(&device,raw[band],NULL,input[band],640*4));
  }
  assets.room=6;assets.height=1024;assets.revision=1;
  for(unsigned i=0;i<sizeof(assets.masks);++i)((uint8_t *)assets.masks)[i]=(uint8_t)(i*19+i/17);
  for(unsigned i=0;i<320*64;++i)assets.wall[i]=0xff000000u|((i*4717+1234)&0xffffffu);
  frame.diorama_active=true;frame.diorama_trunk=&assets;frame.diorama_trunk_settings=DioramaTrunk_Default();
  frame.snes_height=224;frame.ws_extra_top=frame.ws_extra_bottom=64;frame.ws_extra=128;frame.obj_apron=64;
  unsigned checks=0;
  for(unsigned phase=0;phase<5;++phase) {
    frame.bg1_camera_x=450+phase*21;frame.bg1_camera_y=phase==4?12:300;
    frame.inidisp=phase==2?7:phase==3?0:15;
    const int ox=frame.bg1_camera_x-frame.ws_extra-frame.obj_apron,oy=frame.bg1_camera_y-frame.ws_extra_top+1;
    ActionTrunk_Split(&assets,ox,oy,640,352,640,src,pitches,dst,frame.inidisp);
    ArRenderTexture textures[kDioramaPlane_Count]={0};
    for(unsigned i=0;i<3;++i)textures[planes[i]]=raw[i];
    uint32_t changed=0;
    assert(DioramaTrunkGpu_Upload(&device,&frame,textures,phase?0:mask,&changed)&&changed==mask);
    for(unsigned i=0;i<3;++i) {
      assert(SDL_SetRenderTarget(renderer,ArSdlRenderBackend_UnwrapTexture(textures[planes[i]])));
      SDL_Surface *read=SDL_RenderReadPixels(renderer,NULL);assert(read);
      SDL_Surface *pixels=SDL_ConvertSurface(read,SDL_PIXELFORMAT_ARGB8888);assert(pixels);
      for(unsigned y=0;y<352;++y)for(unsigned x=0;x<640;++x) {
        const uint32_t actual=((uint32_t *)((uint8_t *)pixels->pixels+y*pixels->pitch))[x];
        if(actual!=expected[i][y*640+x])fprintf(stderr,"phase=%u plane=%u xy=%u/%u actual=%08x expected=%08x\n",phase,i,x,y,actual,expected[i][y*640+x]);
        assert(actual==expected[i][y*640+x]);++checks;
      }
      SDL_DestroySurface(pixels);SDL_DestroySurface(read);
    }
    assert(SDL_SetRenderTarget(renderer,target));
    changed=0;assert(DioramaTrunkGpu_Upload(&device,&frame,textures,0,&changed)&&!changed);
  }
  DioramaTrunkGpu_Reset(&device);
  for(unsigned i=0;i<3;++i)ArRenderDevice_DestroyTexture(&device,raw[i]);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();
  printf("trunk CPU/GPU parity: %u exact pixels; merged bands, source movement, native fades, boundaries and reuse passed\n",checks);
}
