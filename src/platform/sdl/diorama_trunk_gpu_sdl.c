/* Split native captured artwork before frame generation. Immutable room
 * masks are ROM-derived and shared with the portable editor path. */
#include "diorama/diorama_trunk_gpu.h"
#include "action/action_trunk.h"
#include "present/present.h"
#include "platform/sdl/render_sdl_internal.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "shaders/trunk_split_comp.h"
#include <string.h>

/* Keep the established core-mask and wall offsets unchanged. Only the first
 * three side masks participate in splitting; side snow artwork is CPU mesh
 * data and is not uploaded here. */
enum { kWidth=640,kHeight=352,
  kCoreArtBytes=sizeof(((ActionTrunkAssets *)0)->masks)+sizeof(((ActionTrunkAssets *)0)->wall),
  kSideMaskBytes=kActionTrunkMaxHeight*kActionTrunkSideMaskPitch,
  kSideArtBytes=(kActionTrunkSidePriority+1)*kSideMaskBytes,
  kArtBytes=kCoreArtBytes+kSideArtBytes };
_Static_assert(kActionTrunkSideSolid==0&&kActionTrunkSideRear==1&&
    kActionTrunkSidePriority==2,"trunk split side mask order");
static struct {
  SDL_GPUDevice *gpu;
  SDL_GPUComputePipeline *pipeline;
  SDL_GPUBuffer *art;
  SDL_GPUTransferBuffer *upload;
  SDL_GPUSampler *sampler;
  SDL_GPUTexture *native[3];
  SDL_Texture *view[3];
  const ActionTrunkAssets *assets;
  uint32_t revision;
  int last[12];
  unsigned brightness;
  bool valid;
} s_trunk;

void DioramaTrunkGpu_Reset(ArRenderDevice *device) {
  if(s_trunk.gpu) {
    (void)ArSdlRenderBackend_SubmitPending(device);
    for(unsigned i=0;i<3;++i) {
      SDL_DestroyTexture(s_trunk.view[i]);
      if(s_trunk.native[i])SDL_ReleaseGPUTexture(s_trunk.gpu,s_trunk.native[i]);
    }
    if(s_trunk.pipeline)SDL_ReleaseGPUComputePipeline(s_trunk.gpu,s_trunk.pipeline);
    if(s_trunk.art)SDL_ReleaseGPUBuffer(s_trunk.gpu,s_trunk.art);
    if(s_trunk.upload)SDL_ReleaseGPUTransferBuffer(s_trunk.gpu,s_trunk.upload);
    if(s_trunk.sampler)SDL_ReleaseGPUSampler(s_trunk.gpu,s_trunk.sampler);
  }
  memset(&s_trunk,0,sizeof(s_trunk));
}

bool DioramaTrunkGpu_Prepare(ArRenderDevice *device) {
  if(s_trunk.pipeline)return true;
  s_trunk.gpu=ArSdlRenderBackend_GpuDevice(device);
  if(!s_trunk.gpu)return false;
  static const GpuShaderBlobs shader={kTrunkSplitCompMSL,kTrunkSplitCompMSLSize,
      kTrunkSplitCompSPV,kTrunkSplitCompSPVSize,kTrunkSplitCompDXIL,kTrunkSplitCompDXILSize};
  s_trunk.pipeline=GpuShaderBlob_CreateCompute(s_trunk.gpu,&shader,"Trunk enclosure split",3,1,0,3,1,8,8);
  const SDL_GPUBufferCreateInfo buffer={.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,.size=kArtBytes};
  s_trunk.art=SDL_CreateGPUBuffer(s_trunk.gpu,&buffer);
  const SDL_GPUTransferBufferCreateInfo transfer={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=kArtBytes};
  s_trunk.upload=SDL_CreateGPUTransferBuffer(s_trunk.gpu,&transfer);
  const SDL_GPUSamplerCreateInfo sampler={.min_filter=SDL_GPU_FILTER_NEAREST,.mag_filter=SDL_GPU_FILTER_NEAREST,
      .mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  s_trunk.sampler=SDL_CreateGPUSampler(s_trunk.gpu,&sampler);
  if(!s_trunk.pipeline||!s_trunk.art||!s_trunk.upload||!s_trunk.sampler)goto failed;
  const SDL_GPUTextureCreateInfo desc={.type=SDL_GPU_TEXTURETYPE_2D,
      .format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
      .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
      .width=kWidth,.height=kHeight,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1};
  for(unsigned i=0;i<3;++i) {
    s_trunk.native[i]=SDL_CreateGPUTexture(s_trunk.gpu,&desc);if(!s_trunk.native[i])goto failed;
    const SDL_PropertiesID props=SDL_CreateProperties();
    const bool ok=props&&SDL_SetPointerProperty(props,SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,s_trunk.native[i])&&
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,SDL_PIXELFORMAT_RGBA32)&&
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,SDL_TEXTUREACCESS_TARGET)&&
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,kWidth)&&
        SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,kHeight);
    if(ok)s_trunk.view[i]=SDL_CreateTextureWithProperties(ArSdlRenderBackend_Renderer(device),props);
    SDL_DestroyProperties(props);
    if(!s_trunk.view[i]||!SDL_SetTextureBlendMode(s_trunk.view[i],SDL_BLENDMODE_BLEND)||
        !SDL_SetTextureScaleMode(s_trunk.view[i],SDL_SCALEMODE_NEAREST))goto failed;
  }
  return true;
failed:
  DioramaTrunkGpu_Reset(device);return false;
}

static bool UploadArt(const ActionTrunkAssets *assets) {
  uint8_t *bytes=SDL_MapGPUTransferBuffer(s_trunk.gpu,s_trunk.upload,true);
  if(!bytes)return false;
  memcpy(bytes,assets->masks,sizeof(assets->masks));
  memcpy(bytes+sizeof(assets->masks),assets->wall,sizeof(assets->wall));
  memcpy(bytes+kCoreArtBytes,assets->side_masks,kSideArtBytes);
  SDL_UnmapGPUTransferBuffer(s_trunk.gpu,s_trunk.upload);
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(s_trunk.gpu);
  SDL_GPUCopyPass *copy=cmd?SDL_BeginGPUCopyPass(cmd):NULL;
  if(!copy){if(cmd)SDL_CancelGPUCommandBuffer(cmd);return false;}
  const SDL_GPUTransferBufferLocation from={.transfer_buffer=s_trunk.upload};
  const SDL_GPUBufferRegion to={.buffer=s_trunk.art,.size=kArtBytes};
  SDL_UploadToGPUBuffer(copy,&from,&to,true);SDL_EndGPUCopyPass(copy);
  if(!SDL_SubmitGPUCommandBuffer(cmd))return false;
  s_trunk.assets=assets;s_trunk.revision=assets->revision;s_trunk.valid=false;
  return true;
}

bool DioramaTrunkGpu_Upload(ArRenderDevice *device,const FrameSlot *slot,
    ArRenderTexture textures[kDioramaPlane_Count],uint32_t source_changed,uint32_t *changed) {
  const ActionTrunkAssets *assets=slot->diorama_trunk;
  if(!slot->diorama_active||!assets||!slot->diorama_trunk_settings.enabled){s_trunk.valid=false;return true;}
  if(!DioramaTrunkGpu_Prepare(device))return false;
  if(s_trunk.assets!=assets||s_trunk.revision!=assets->revision) {
    if(!ArSdlRenderBackend_SubmitPending(device)||!UploadArt(assets))return false;
  }
  const int params[12]={kWidth,slot->snes_height+slot->ws_extra_top+slot->ws_extra_bottom,
      slot->bg1_camera_x-slot->ws_extra-slot->obj_apron,slot->bg1_camera_y-slot->ws_extra_top+1,
      (int)assets->height,2*kActionTrunkMaxHeight*kActionTrunkMaskPitch/4,0,(int)sizeof(assets->masks)/4,
      (int)assets->side_width,kCoreArtBytes/4,kActionTrunkSideMaskPitch,kSideMaskBytes/4};
  const unsigned planes[3]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
  const uint32_t mask=(1u<<planes[0])|(1u<<planes[1])|(1u<<planes[2]);
  const unsigned brightness=slot->inidisp&0x80?0:slot->inidisp&15;
  if(!s_trunk.valid||(source_changed&mask)||memcmp(s_trunk.last,params,sizeof(params))||s_trunk.brightness!=brightness) {
    SDL_GPUTextureSamplerBinding samples[3];
    for(unsigned i=0;i<3;++i) {
      SDL_Texture *input=ArSdlRenderBackend_UnwrapTexture(textures[planes[i]]);
      samples[i]=(SDL_GPUTextureSamplerBinding){.texture=input?SDL_GetPointerProperty(
          SDL_GetTextureProperties(input),SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,NULL):NULL,.sampler=s_trunk.sampler};
      if(!samples[i].texture)return false;
    }
    if(!ArSdlRenderBackend_SubmitPending(device))return false;
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(s_trunk.gpu);if(!cmd)return false;
    SDL_GPUStorageTextureReadWriteBinding outputs[3]={0};
    for(unsigned i=0;i<3;++i)outputs[i].texture=s_trunk.native[i];
    SDL_GPUComputePass *pass=SDL_BeginGPUComputePass(cmd,outputs,3,NULL,0);
    if(!pass){SDL_CancelGPUCommandBuffer(cmd);return false;}
    struct {int data[8];float fade[4];int side[4];} uniforms;
    memcpy(uniforms.data,params,sizeof(uniforms.data));
    uniforms.fade[0]=brightness/15.f;uniforms.fade[1]=uniforms.fade[2]=uniforms.fade[3]=0;
    memcpy(uniforms.side,params+8,sizeof(uniforms.side));
    SDL_PushGPUComputeUniformData(cmd,0,&uniforms,sizeof(uniforms));
    SDL_BindGPUComputePipeline(pass,s_trunk.pipeline);
    SDL_BindGPUComputeSamplers(pass,0,samples,3);
    SDL_BindGPUComputeStorageBuffers(pass,0,&s_trunk.art,1);
    SDL_DispatchGPUCompute(pass,kWidth/8,kHeight/8,1);SDL_EndGPUComputePass(pass);
    if(!SDL_SubmitGPUCommandBuffer(cmd))return false;
    memcpy(s_trunk.last,params,sizeof(params));s_trunk.brightness=brightness;s_trunk.valid=true;*changed|=mask;
  }
  for(unsigned i=0;i<3;++i)textures[planes[i]]=ArSdlRenderBackend_BorrowTexture(s_trunk.view[i]);
  return true;
}
