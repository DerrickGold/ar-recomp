#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "action/action_trunk.h"
#include "diorama/diorama_trunk_gpu.h"
#include "present/present.h"
#include "platform/sdl/render_sdl_interop.h"
#include "support/test_assert.h"

static ActionTrunkAssets assets;
static ActionTrunkAssets replacement;
static FrameSlot frame;
static uint32_t input[3][640*352],expected[3][640*352];

static void SetSideMasks(ActionTrunkAssets *a) {
  for(unsigned y=0;y<kActionTrunkMaxHeight;++y)for(unsigned x=0;x<kActionTrunkSideWidth;++x) {
    const unsigned pattern=(x/8+y/5)%8,at=y*kActionTrunkSideMaskPitch+x/8;
    const uint8_t bit=(uint8_t)(1u<<(x&7));
    if(pattern==1||pattern==4||pattern==6)a->side_masks[kActionTrunkSideSolid][at]|=bit;
    if(pattern==2||pattern==5||pattern==6)a->side_masks[kActionTrunkSideRear][at]|=bit;
    if(pattern>=3&&pattern<=6)a->side_masks[kActionTrunkSidePriority][at]|=bit;
    /* Deliberately dirty the core and snow-only bits: neither may change the
     * established core split or acquire a side-split meaning. */
    a->side_masks[kActionTrunkSideSnow][at]|=bit;
  }
}

/* Independently retain the original core formula, and check each specified
 * side ownership case before using the production CPU result as GPU oracle. */
static void CheckCpuOwnership(const ActionTrunkAssets *a,int ox,int oy,unsigned brightness) {
  assert(kActionTrunkLeft==320&&kActionTrunkWidth==384&&kActionTrunkMaskPitch==48);
  assert(sizeof(a->masks)==3u*2048u*48u);
  for(int y=0;y<352;++y)for(int x=0;x<640;++x) {
    const int at=y*640+x,wx=x+ox,wy=y+oy;
    uint32_t low=input[2][at],high=0,far=0;
    if(input[0][at]>>24)low=input[0][at];
    if(input[1][at]>>24)low=input[1][at];
    if(wy>=0&&(unsigned)wy<a->height) {
      if(wx>=320&&wx<704) {
        const unsigned mask_at=(unsigned)wy*48+(unsigned)(wx-320)/8;
        const unsigned bit=1u<<(wx&7);
        if(a->masks[0][mask_at]&bit){high=low;low=0;}
        if(a->masks[2][mask_at]&bit)low=high=0;
        if(wx>=352&&wx<672) {
          const uint32_t color=a->wall[(wy&63)*320+wx-352];far=color;
          if(brightness<15) {
            far=color&0xff000000u;
            for(unsigned shift=0;shift<=16;shift+=8)
              far|=((((color>>shift)&255u)*brightness+7)/15)<<shift;
          }
        }
      } else if(wx>=0&&(unsigned)wx<a->side_width) {
        const unsigned mask_at=(unsigned)wy*kActionTrunkSideMaskPitch+(unsigned)wx/8;
        const unsigned bit=1u<<(wx&7);
        if(a->side_masks[kActionTrunkSideRear][mask_at]&bit){far=low;low=0;}
        else if(a->side_masks[kActionTrunkSideSolid][mask_at]&bit)far=low;
        if(a->side_masks[kActionTrunkSidePriority][mask_at]&bit){high=low;low=0;}
      }
    }
    assert(expected[0][at]==low&&expected[1][at]==high&&expected[2][at]==far);
  }
}

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
    for(unsigned i=0;i<640*352;++i)
      if(i%13&&(i%3==band||i%17==0))input[band][i]=0xff000000u|((i*173+(band+1)*79013)&0xffffffu);
    src[band]=input[band];dst[band]=expected[band];
    assert(ArRenderDevice_CreateTexture(&device,&desc,&raw[band]));
    assert(ArRenderDevice_UpdateTexture(&device,raw[band],NULL,input[band],640*4));
  }
  assets.room=6;assets.height=1024;assets.revision=1;
  for(unsigned i=0;i<sizeof(assets.masks);++i)((uint8_t *)assets.masks)[i]=(uint8_t)(i*19+i/17);
  for(unsigned i=0;i<320*64;++i)assets.wall[i]=0xff000000u|((i*4717+1234)&0xffffffu);
  SetSideMasks(&assets);
  frame.diorama_active=true;frame.diorama_trunk=&assets;frame.diorama_trunk_settings=DioramaTrunk_Default();
  frame.snes_height=224;frame.ws_extra_top=frame.ws_extra_bottom=64;frame.ws_extra=128;frame.obj_apron=64;
  unsigned checks=0;
  for(unsigned phase=0;phase<15;++phase) {
    if(phase<5) {
      /* Legacy side_width=0: all old core/fade/capture cases must remain exact. */
      frame.bg1_camera_x=450+phase*21;frame.bg1_camera_y=phase==4?12:300;
      frame.inidisp=phase==2?7:phase==3?0:15;
    } else {
      static const int origins[10][2]={{-64,300},{640,300},{704,-16},{320,300},{64,1900},
          {640,2040},{640,300},{640,300},{640,300},{640,300}};
      frame.bg1_camera_x=origins[phase-5][0]+frame.ws_extra+frame.obj_apron;
      frame.bg1_camera_y=origins[phase-5][1]+frame.ws_extra_top-1;
      frame.inidisp=phase==6||phase==9?7:phase==7?0:15;
      if(phase==5){assets.height=2048;assets.side_width=1024;++assets.revision;}
      if(phase==12) {
        for(unsigned kind=0;kind<=kActionTrunkSidePriority;++kind)
          for(unsigned i=0;i<sizeof(assets.side_masks[0]);++i)assets.side_masks[kind][i]^=255;
        ++assets.revision;
      }
      if(phase==13) {
        replacement=assets;
        for(unsigned i=0;i<sizeof(replacement.side_masks[0]);++i)
          replacement.side_masks[kActionTrunkSideRear][i]^=255;
        /* Same revision and camera, different immutable asset identity. */
        frame.diorama_trunk=&replacement;
      }
      if(phase==14){replacement.side_width=0;++replacement.revision;}
    }
    const int ox=frame.bg1_camera_x-frame.ws_extra-frame.obj_apron,oy=frame.bg1_camera_y-frame.ws_extra_top+1;
    ActionTrunk_Split(frame.diorama_trunk,ox,oy,640,352,640,src,pitches,dst,frame.inidisp);
    CheckCpuOwnership(frame.diorama_trunk,ox,oy,frame.inidisp);
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
  printf("trunk CPU/GPU parity: %u exact pixels; legacy core, side ownership/priority, transparent sources, merged bands, negative/cropped origins, fades, asset replacement and reuse passed\n",checks);
}
