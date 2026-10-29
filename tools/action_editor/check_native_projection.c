/* Real-GPU oracle for complete room assets, independent of unsafe raw warps.
 * CPU readback occurs only here to compare the reference and resident frames. */
#include <SDL3/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "room_preview_native.h"
#include "action/action_scene_snapshot.h"
#include "platform/sdl/action_effect_source_sdl.h"
#include "platform/sdl/render_sdl_interop.h"

static uint8_t s_input[1024*1024+1];
static ArRenderDevice s_device;
static ArGpuEffectSource s_source;
static SDL_GPUBuffer *s_motion;
static unsigned s_primitives;
static unsigned s_motion_mask;
static float s_phase;
static bool s_retained;
static bool s_invalid_slot;
static float s_brightness = 1;
static unsigned s_packet;
static uint64_t s_capture;
uint8_t *DioramaPreview_Input(void) { return s_input; }
unsigned DioramaPreview_Capacity(void) { return sizeof(s_input); }
ArRenderDevice *DioramaPreview_Device(void) { return &s_device; }
void DioramaPreview_Reset(void) { Diorama_ResetCompositorResources(&s_device); }
bool DioramaPreview_Begin(int width, int height) {
  (void)width; (void)height;
  return ArRenderDevice_SetRenderTarget(&s_device,ArRenderTexture_Invalid()) &&
      ArRenderDevice_SetViewport(&s_device,NULL) && ArRenderDevice_SetClipRect(&s_device,NULL) &&
      ArRenderDevice_Clear(&s_device,(ArRenderColorF){0,0,0,1});
}
static bool Effects(ArRenderDevice *device,const ActionEffectSourceBatch *batch,
    const DioramaProjection *view,const ActionMoonlightOcclusion *scenery,ArRenderBlendMode blend,float brightness) {
  s_primitives+=batch->count;
  ActionEffectSourceBatch source = *batch;
  /* Compare both first upload and reuse of the very same immutable packets.
   * Unique capture revisions also exercise replacement across camera poses. */
  if (s_retained && s_packet < kActionSourcePacketSlots) {
    source.packet_slot = s_packet;
    source.revision = s_capture * kActionSourcePacketSlots + s_packet + 1;
  } else if (s_invalid_slot) {
    source.packet_slot = kActionSourcePacketSlots;
    source.revision = 1; /* Out-of-range slots must behave as transient draws. */
  }
  ++s_packet;
  return ArGpuEffectSource_Draw(&s_source,device,s_motion,s_motion_mask,s_phase,&source,view,scenery,blend,brightness*s_brightness);
}
static bool Skybox(ArRenderDevice *device,ArRenderTexture texture,const DioramaSkyboxSourceDraw *draw) {
  return ArGpuEffectSource_Skybox(&s_source,device,s_motion,s_motion_mask?draw->motion_slot:-1,s_phase,texture,draw);
}
static bool EffectBatch(ArRenderDevice *device, bool begin) {
  (void)device;
  return begin ? ArGpuEffectSource_BeginBatch(&s_source) : ArGpuEffectSource_EndBatch(&s_source);
}
static unsigned Read(const char *path) {
  size_t n=0;void *data=SDL_LoadFile(path,&n);
  if(!data || n>=sizeof(s_input)){SDL_free(data);return 0;}
  memcpy(s_input,data,n);s_input[n]=0;SDL_free(data);return (unsigned)n;
}

static unsigned PixelDifference(const SDL_Surface *a, const SDL_Surface *b,
    unsigned tolerance, unsigned *peak) {
  unsigned changed = 0;
  for (int y = 0; y < a->h; ++y) for (int x = 0; x < a->w; ++x) {
    unsigned maximum = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
      const uint8_t av = ((const uint8_t *)a->pixels)[y*a->pitch+x*4+channel];
      const uint8_t bv = ((const uint8_t *)b->pixels)[y*b->pitch+x*4+channel];
      const unsigned delta = (unsigned)abs((int)av-(int)bv);
      if (delta > maximum) maximum = delta;
    }
    if (maximum > tolerance) ++changed;
    if (peak && maximum > *peak) *peak = maximum;
  }
  return changed;
}

static uint64_t FrameHash(const SDL_Surface *image) {
  uint64_t hash = UINT64_C(14695981039346656037);
  if (!image) return 0;
  for (int y = 0; y < image->h; ++y)
    for (int x = 0; x < image->w * 4; ++x) {
      hash ^= ((const uint8_t *)image->pixels)[y * image->pitch + x];
      hash *= UINT64_C(1099511628211);
    }
  return hash;
}
int main(int argc,char **argv) {
  if(argc<4){fprintf(stderr,"usage: %s output-dir layers.ini room.arscene [...]\nOptional AR_ORACLE_EFFECTS=effects.ini overlays authored recipes on each room.\n",argv[0]);return 2;}
  if(!SDL_Init(SDL_INIT_VIDEO))return 77;
  SDL_Window *window=SDL_CreateWindow("Complete-room GPU projection comparison",720,448,SDL_WINDOW_HIDDEN);
  if(!window || !ArSdlRenderBackend_CreateForWindow(&s_device,window,NULL))return 77;
  SDL_GPUDevice *gpu_device=ArSdlRenderBackend_GpuDevice(&s_device);
  if(!gpu_device || !ArGpuEffectSource_Init(&s_source,gpu_device)) {
    fprintf(stderr,"shader initialization: %s\n",SDL_GetError());return 1;
  }
  RoomPreview_SetSourceEffectBatch(EffectBatch);
  const SDL_GPUBufferCreateInfo info={.size=256,.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
  s_motion=SDL_CreateGPUBuffer(gpu_device,&info);
  if(!s_motion)return 1;
  const SDL_GPUTransferBufferCreateInfo upload_info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=256};
  SDL_GPUTransferBuffer *upload=SDL_CreateGPUTransferBuffer(gpu_device,&upload_info);
  int32_t *vectors=upload?SDL_MapGPUTransferBuffer(gpu_device,upload,false):NULL;
  if(!vectors)return 1;
  memset(vectors,0,256);
  for(unsigned i=0;i<8;++i){vectors[i*8]=3;vectors[i*8+1]=-3;vectors[i*8+2]=-2;vectors[i*8+3]=2;vectors[i*8+4]=1;}
  SDL_UnmapGPUTransferBuffer(gpu_device,upload);
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu_device);
  SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
  const SDL_GPUTransferBufferLocation from={.transfer_buffer=upload};
  const SDL_GPUBufferRegion to={.buffer=s_motion,.size=256};
  SDL_UploadToGPUBuffer(copy,&from,&to,false);SDL_EndGPUCopyPass(copy);
  if(!SDL_SubmitGPUCommandBuffer(cmd))return 1;
  SDL_ReleaseGPUTransferBuffer(gpu_device,upload);
  unsigned failures=0;
  for(int room=3;room<argc;++room) {
    unsigned n=Read(argv[room]);
    if(!n || !RoomPreview_Load(n) || !(n=Read(argv[2])) || !RoomPreview_Configure(n)) {
      fprintf(stderr,"room load failed: %s\n",argv[room]);++failures;continue;
    }
    const char *effects = getenv("AR_ORACLE_EFFECTS");
    if(effects && (!(n=Read(effects)) || !RoomPreview_ConfigureEffects(n))) {
      fprintf(stderr,"effects load failed: %s\n",effects);++failures;continue;
    }
    const char *player = getenv("AR_ORACLE_PLAYER");
    if (player && (!(n=Read(player)) || !RoomPreview_LoadPlayer(n))) {
      fprintf(stderr,"player artwork load failed: %s\n",player);++failures;continue;
    }
    unsigned w=RoomPreview_Width(),h=RoomPreview_Height();
    unsigned named=RoomPreview_SkyboxRoom();
    if(named) {
      char path[1024];const char *slash=strrchr(argv[room],'/');
      snprintf(path,sizeof(path),"%.*s%02x%02x.arscene",slash?(int)(slash-argv[room]+1):0,argv[room],named>>16,(named>>8)&255);
      if(!(n=Read(path)) || !RoomPreview_LoadSkybox(n)){fprintf(stderr,"named skybox load failed: %s\n",path);++failures;continue;}
    }
    for(unsigned pose=0;pose<3;++pose)for(unsigned sky=0;sky<3;++sky)for(unsigned motion=0;motion<3;++motion) {
      SDL_Surface *images[8]={0};
      static const char *const names[] = {"reference", "resident", "retained-cold", "retained-warm",
          "fade-reference", "fade-retained", "fade-restored", "invalid-slot"};
      const unsigned image_count = sizeof(images)/sizeof(images[0]);
      ++s_capture;
      const int x=(int)((w>256?w-256:0)*pose/2), y=(int)((h>224?h-224:0)*(2-pose)/2);
      if (player && getenv("AR_ORACLE_MIST_WAKE")) {
        RoomPreview_SetSourceProjection(NULL,NULL);
        if (!RoomPreview_SetReceivers(3,x+101,y+196,x+179,y+196) ||
            !RoomPreview_Render(x,y,99+pose*43,120,64,720,448,1,pose==1?.13f:0,pose==1?-.11f:0,sky,0)) {
          fprintf(stderr,"wake seed failed\n");++failures;continue;
        }
      }
      if (player && !RoomPreview_SetReceivers(3,x+104,y+196,x+176,y+196)) {
        fprintf(stderr,"receiver setup failed\n");++failures;continue;
      }
      s_primitives=0;
      s_motion_mask=motion?255:0;s_phase=motion==1?.25f:.75f;
      const float dx=motion==1?-1.25f:motion==2?-.5f:0;
      ArRenderPointF offsets[kDioramaPlane_Count];
      for(unsigned i=0;i<kDioramaPlane_Count;++i)offsets[i]=(ArRenderPointF){dx,-dx};
      for(unsigned resident=0;resident<image_count;++resident) {
        s_retained = resident == 2 || resident == 3 || resident == 5 || resident == 6;
        s_invalid_slot = resident == 7;
        s_brightness = resident == 4 || resident == 5 ? .6f : 1;
        s_packet = 0;
        RoomPreview_SetSourceProjection(resident?Effects:NULL,resident?Skybox:NULL);
        RoomPreview_SetProjectionOffsets(resident?NULL:offsets,(ArRenderPointF){resident?0:dx,resident?0:-dx});
        if(!RoomPreview_Render(x,y,100+pose*43,120,64,720,448,1,pose==1?.13f:0,pose==1?-.11f:0,sky,0)) {
          fprintf(stderr,"render failed room=%s pose=%u sky=%u resident=%u: %s\n",argv[room],pose,sky,resident,SDL_GetError());break;
        }
        SDL_Surface *raw=SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&s_device),NULL);
        if(raw)images[resident]=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
      }
      unsigned changed=0,over=0,peak=0,retained_cold=0,retained_warm=0;
      unsigned fade_changed=0,restored_changed=0,invalid_slot_changed=0;
      bool rendered = true;
      for(unsigned i=0;i<image_count;++i)rendered &= images[i] != NULL;
      if(!rendered)++failures;
      else {
        changed=PixelDifference(images[0],images[1],0,&peak);
        over=PixelDifference(images[0],images[1],2,NULL);
        retained_cold=PixelDifference(images[1],images[2],0,NULL);
        retained_warm=PixelDifference(images[1],images[3],0,NULL);
        fade_changed=PixelDifference(images[4],images[5],0,NULL);
        restored_changed=PixelDifference(images[1],images[6],0,NULL);
        invalid_slot_changed=PixelDifference(images[1],images[7],0,NULL);
        const bool mismatch = over || retained_cold || retained_warm || fade_changed || restored_changed || invalid_slot_changed;
        if(mismatch) ++failures;
        if(mismatch || getenv("AR_ORACLE_SAVE")) {
          for(unsigned i=0;i<image_count;++i){char path[1024];snprintf(path,sizeof(path),"%s/room-%02d-p%u-s%u-m%u-%s.png",argv[1],room-3,pose,sky,motion,names[i]);SDL_SavePNG(images[i],path);}
        }
      }
      printf("{\"room\":\"%s\",\"pose\":%u,\"sky\":%u,\"motion\":%u,\"named_skybox\":%u,\"primitives\":%u,\"changed\":%u,\"over2\":%u,\"peak\":%u,\"retained_cold_changed\":%u,\"retained_warm_changed\":%u,\"fade_changed\":%u,\"restored_changed\":%u,\"invalid_slot_changed\":%u,\"gpu_hash\":\"%016" PRIx64 "\",\"rendered\":%s}\n",
          argv[room],pose,sky,motion,named,s_primitives,changed,over,peak,retained_cold,retained_warm,fade_changed,restored_changed,invalid_slot_changed,FrameHash(images[1]),rendered?"true":"false");fflush(stdout);
      for(unsigned i=0;i<image_count;++i)SDL_DestroySurface(images[i]);
    }
    RoomPreview_Reset();
  }
  Diorama_ResetCompositorResources(&s_device);
  ArGpuEffectSource_Destroy(&s_source);SDL_ReleaseGPUBuffer(gpu_device,s_motion);
  ArSdlRenderBackend_Destroy(&s_device);SDL_DestroyWindow(window);SDL_Quit();
  return failures?1:0;
}
