#include "diorama_depth_sink_sdl.h"
#include "diorama_depth_alpha_sdl.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_depth_gpu.h"
#include "render_sdl_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct ArSdlDioramaDepthBatch {
  DioramaDepthDraw draws[kDioramaDepthMaximumDraws];
  SDL_GPUTexture *sources[kDioramaDepthMaximumDraws];
  ArGpuDioramaDepthMaterial materials[kDioramaDepthMaximumDraws];
  DioramaSceneryLightEffectParams params[kDioramaDepthMaximumDraws];
  SDL_GPUTexture *target;
  unsigned slot, width, height;
} ArSdlDioramaDepthBatch;

static SDL_GPUTexture *Texture(SDL_Texture *texture) {
  return texture ? SDL_GetPointerProperty(SDL_GetTextureProperties(texture),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL) : NULL;
}
static SDL_GPUTexture *Source(ArRenderTexture handle) {
  return Texture(ArSdlRenderBackend_UnwrapTexture(handle));
}
static SDL_Texture *Target(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *target = renderer ? SDL_GetRenderTarget(renderer) : NULL;
  if (!target && renderer) target = ((ArSdlRenderBackend *)device->context)->output_target;
  return target;
}

static bool Flush(void *context) {
  ArSdlDioramaDepthSink *s=context;
  if (!s->count) return true;
  const DioramaPerformanceScope submitted=DioramaPerformance_Begin(kDioramaPerformance_Submit);
  ArSdlDioramaDepthBatch *batch=s->batch;
  unsigned va=0,ia=0;
  for (unsigned i=0;i<s->count;++i) {
    /* Reserve can relocate the owned arrays between submissions. Resolve all
     * borrowed views only now, after the complete batch has been retained. */
    batch->draws[i].vertices=s->vertices+va;
    batch->draws[i].indices=s->indices+ia;
    va+=batch->draws[i].vertex_count;ia+=batch->draws[i].index_count;
  }
  SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(s->device);
  const bool encoded=cmd && (s->hybrid ? ArGpuDioramaDepthPass_EncodeHybridMaterials :
      ArGpuDioramaDepthPass_EncodeMaterials)(
      &s->targets[batch->slot].pass,cmd,batch->target,batch->width,batch->height,
      batch->draws,batch->sources,batch->materials,s->count,
      SDL_GPU_LOADOP_LOAD,(ArRenderColorF){0},!s->targets[batch->slot].valid);
  bool ok=false;
  if (encoded) ok=SDL_SubmitGPUCommandBuffer(cmd);
  else if (cmd) SDL_CancelGPUCommandBuffer(cmd);
  s->targets[batch->slot].valid=ok;
  if (ok) {
    ++s->stats.submissions;
    s->stats.draws+=s->count;
    if (s->stats.largest_batch<s->count) s->stats.largest_batch=s->count;
  } else {
    s->failed=true;
    SDL_SetError("shared depth batch failed (%u draws, %u vertices)",s->count,s->vertex_count);
  }
  /* Failure abandons work as well, so ordinary target/resource cleanup never
   * retries an invalid batch or retains a texture after its owner releases it. */
  s->count=s->vertex_count=s->index_count=0;
  DioramaPerformance_End(submitted);
  return ok;
}

static bool Finish(void *context,ArRenderDevice *device,bool success) {
  ArSdlDioramaDepthSink *s=context;
  if (s->owner!=device) return false;
  if (!success) s->count=s->vertex_count=s->index_count=0;
  const bool drained=ArSdlRenderBackend_SetExternalFlush(device,NULL,s);
  s->owner=NULL;
  return drained && !s->failed;
}

void ArSdlDioramaDepthSink_Reset(ArSdlDioramaDepthSink *s) {
  if (!s) return;
  if (s->owner) (void)Finish(s,s->owner,false);
  for (unsigned i=0;i<4;++i) ArGpuDioramaDepthPass_Destroy(&s->targets[i].pass);
  free(s->vertices);free(s->indices);free(s->batch);
  *s=(ArSdlDioramaDepthSink){0};
}

static bool Clear(void *context,ArRenderDevice *device) {
  ArSdlDioramaDepthSink *s=context;
  SDL_Texture *target=Target(device);
  if (!target || s->owner!=device || s->failed || !Flush(s)) return false;
  for (unsigned i=0;i<4;++i) if (s->targets[i].target==target) s->targets[i].valid=false;
  return true;
}

static unsigned Capacity(unsigned count,unsigned maximum) {
  unsigned n=256;
  while (n<count && n<maximum) n*=2;
  return n<maximum?n:maximum;
}
static bool Reserve(ArSdlDioramaDepthSink *s,unsigned nv,unsigned ni) {
  if (!s->batch && !(s->batch=calloc(1,sizeof(*s->batch)))) return false;
  if (nv>s->capacity) {
    const unsigned capacity=Capacity(nv,kDioramaDepthMaximumVertices);
    DioramaDepthVertex *vertices=realloc(s->vertices,capacity*sizeof(*vertices));
    if (!vertices) return false;
    s->vertices=vertices;s->capacity=capacity;
  }
  if (ni>s->index_capacity) {
    const unsigned capacity=Capacity(ni,kDioramaDepthMaximumIndices);
    int32_t *indices=realloc(s->indices,capacity*sizeof(*indices));
    if (!indices) return false;
    s->indices=indices;s->index_capacity=capacity;
  }
  return true;
}

static unsigned TargetSlot(ArSdlDioramaDepthSink *s,SDL_Texture *target,SDL_GPUTextureFormat format) {
  unsigned slot=0;
  while (slot<4 && s->targets[slot].target!=target) ++slot;
  if (slot==4) {
    /* Preserve other prepared targets even if they have no depth this frame. */
    for (slot=0;slot<4 && s->targets[slot].target;++slot) {}
    if (slot==4) for (slot=0;slot<4 && s->targets[slot].valid;++slot) {}
    if (slot==4) return slot;
    s->targets[slot].target=target;
    s->targets[slot].valid=false;
  }
  ArGpuDioramaDepthPass *pass=&s->targets[slot].pass;
  if (pass->device && pass->color_format!=format) {
    ArGpuDioramaDepthPass_Destroy(pass);s->targets[slot].valid=false;
  }
  if (!pass->device && !ArGpuDioramaDepthPass_InitFormat(pass,s->device,format)) return 4;
  return slot;
}

bool ArSdlDioramaDepthSink_PrepareTarget(ArSdlDioramaDepthSink *s,ArRenderDevice *device,
    ArRenderTexture handle,unsigned vertices,unsigned indices) {
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(device);
  if (!s || !gpu || s->owner || (s->device && s->device!=gpu) ||
      vertices>kDioramaDepthMaximumVertices || indices>kDioramaDepthMaximumIndices) return false;
  SDL_Texture *target=ArRenderTexture_IsValid(handle)?ArSdlRenderBackend_UnwrapTexture(handle):
      ((ArSdlRenderBackend *)device->context)->output_target;
  if (!Texture(target)) return false;
  const SDL_PropertiesID properties=SDL_GetTextureProperties(target);
  const int width=(int)SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_WIDTH_NUMBER,0);
  const int height=(int)SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_HEIGHT_NUMBER,0);
  const SDL_GPUTextureFormat format=SDL_GetGPUTextureFormatFromPixelFormat((SDL_PixelFormat)
      SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_FORMAT_NUMBER,SDL_PIXELFORMAT_UNKNOWN));
  if (width<=0 || height<=0 || width>16384 || height>16384 ||
      SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_ACCESS_NUMBER,-1)!=SDL_TEXTUREACCESS_TARGET)
    return false;
  s->device=gpu;
  const unsigned slot=TargetSlot(s,target,format);
  return slot<4 && Reserve(s,vertices,indices) &&
      ArGpuDioramaDepthPass_Prepare(&s->targets[slot].pass,width,height,vertices,indices);
}

bool ArSdlDioramaDepthSink_PrepareHybridTarget(ArSdlDioramaDepthSink *s,ArRenderDevice *device,
    ArRenderTexture handle,unsigned vertices,unsigned indices) {
  if (!ArSdlDioramaDepthSink_PrepareTarget(s,device,handle,vertices,indices)) return false;
  SDL_Texture *target=ArRenderTexture_IsValid(handle)?ArSdlRenderBackend_UnwrapTexture(handle):
      ((ArSdlRenderBackend *)device->context)->output_target;
  for (unsigned i=0;i<4;++i) if (s->targets[i].target==target) {
    ArGpuDioramaDepthPass *pass=&s->targets[i].pass;
    return ArGpuDioramaDepthPass_PrepareHybrid(pass,pass->width,pass->height,vertices,indices);
  }
  return false;
}

static bool Submit(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  ArSdlDioramaDepthSink *s=context;
  if (s->failed || s->owner!=device || !draw) return false;
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(device);
  SDL_Texture *target=Target(device), *art=ArSdlRenderBackend_UnwrapTexture(draw->texture);
  SDL_GPUTexture *destination=Texture(target), *source=Texture(art);
  SDL_Rect viewport,clip;
  float scale_x,scale_y;
  if (!renderer || !destination || s->device!=ArSdlRenderBackend_GpuDevice(device) ||
      !draw->vertices || !draw->depths || !draw->indices || draw->vertex_count<=0 ||
      draw->vertex_count>kDioramaDepthMaximumVertices || draw->index_count<=0 ||
      draw->index_count>kDioramaDepthMaximumIndices ||
      (ArRenderTexture_IsValid(draw->texture) && !source) ||
      !SDL_GetRenderViewport(renderer,&viewport) || !SDL_GetRenderScale(renderer,&scale_x,&scale_y) ||
      scale_x!=1 || scale_y!=1) return false;
  if (draw->clip_w) {
    if (draw->native_actor) return false;
    for (int i=0;i<draw->vertex_count;++i)
      if (!isfinite(draw->clip_w[i]) || draw->clip_w[i]<=0) return false;
  }
  const SDL_PropertiesID properties=SDL_GetTextureProperties(target);
  const int width=(int)SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_WIDTH_NUMBER,0);
  const int height=(int)SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_HEIGHT_NUMBER,0);
  const SDL_GPUTextureFormat format=SDL_GetGPUTextureFormatFromPixelFormat((SDL_PixelFormat)
      SDL_GetNumberProperty(properties,SDL_PROP_TEXTURE_FORMAT_NUMBER,SDL_PIXELFORMAT_UNKNOWN));
  if (width<=0 || height<=0) return false;
  clip=viewport;
  if (SDL_RenderClipEnabled(renderer)) {
    SDL_Rect local;
    if (!SDL_GetRenderClipRect(renderer,&local)) return false;
    local.x+=viewport.x;local.y+=viewport.y;
    if (!SDL_GetRectIntersection(&clip,&local,&clip)) return true;
  }
  const SDL_Rect bounds={0,0,width,height};
  if (!SDL_GetRectIntersection(&clip,&bounds,&clip)) return true;
  if (s->count && (s->batch->target!=destination || s->batch->width!=(unsigned)width ||
      s->batch->height!=(unsigned)height || s->count==kDioramaDepthMaximumDraws ||
      (unsigned)draw->vertex_count>kDioramaDepthMaximumVertices-s->vertex_count ||
      (unsigned)draw->index_count>kDioramaDepthMaximumIndices-s->index_count)) {
    if (!Flush(s)) return false;
  }
  const unsigned slot=TargetSlot(s,target,format);
  if (slot==4) return false;
  SDL_ScaleMode filter=SDL_SCALEMODE_NEAREST;
  if (art && !SDL_GetTextureScaleMode(art,&filter)) return false;
  DioramaSceneryLightEffectParams params=draw->lighting?*draw->lighting:
      (DioramaSceneryLightEffectParams){.shade={1,1,1,1}};
  const bool lit=ArRenderTexture_IsValid(params.illumination);
  ArGpuDioramaDepthMaterial material={
      .actor=source && draw->native_actor && (!lit || ArRenderTexture_IsValid(params.attributes))};
  if (lit) {
    material.illumination=Source(params.illumination);
    if (!material.illumination) return false;
    if (material.actor && ArRenderTexture_IsValid(params.attributes)) {
      material.attributes=Source(params.attributes);
      material.enemy_illumination=Source(params.enemy_illumination);
      if (!material.attributes || !material.enemy_illumination) return false;
    }
  } else {
    params.grade=(DioramaSceneryGrade){0};params.heat=(DioramaHeat){0};
    params.cloud_light=(DioramaCloudLight){0};params.shade=(ArRenderColorF){1,1,1,1};
    if (material.actor && params.rim_strength>0) {
      params.blur_radius=0;params.texel_width=1.f/SR_PPU_SURFACE_MAX_WIDTH;
      params.texel_height=1.f/SR_PPU_SURFACE_MAX_HEIGHT;
    }
  }
  if (!Reserve(s,s->vertex_count+(unsigned)draw->vertex_count,
      s->index_count+(unsigned)draw->index_count)) return false;
  /* Close preceding SDL work once at the start of an uninterrupted batch.
   * The backend drains this batch before the next ordinary draw or mutation. */
  if (!s->count && !ArSdlRenderBackend_SubmitPending(device)) return false;
  ArSdlDioramaDepthBatch *batch=s->batch;
  batch->target=destination;batch->slot=slot;batch->width=width;batch->height=height;
  for (int i=0;i<draw->vertex_count;++i) {
    const ArRenderVertex2D *v=&draw->vertices[i];
    const float w=draw->clip_w?draw->clip_w[i]:1;
    s->vertices[s->vertex_count+(unsigned)i]=(DioramaDepthVertex){
        .clip={(2*(v->position.x+viewport.x)/width-1)*w,
            (1-2*(v->position.y+viewport.y)/height)*w,draw->depths[i]*w,w},
        .color=v->color,.uv=v->tex_coord};
  }
  memcpy(s->indices+s->index_count,draw->indices,(size_t)draw->index_count*sizeof(*s->indices));
  batch->draws[s->count]=(DioramaDepthDraw){
      .vertex_count=(unsigned)draw->vertex_count,.index_count=(unsigned)draw->index_count,
      .depth_mode=draw->native_actor?kDioramaDepth_NativeActor:
          draw->blend==kArRenderBlendMode_Add?kDioramaDepth_Read:kDioramaDepth_Write,
      .blend=draw->blend,.textured=source!=NULL,.nearest=filter==SDL_SCALEMODE_NEAREST,
      .clip_enabled=true,.clip={clip.x,clip.y,clip.w,clip.h}};
  batch->params[s->count]=params;
  material.scenery=source?&batch->params[s->count]:NULL;
  batch->materials[s->count]=material;
  batch->sources[s->count]=source;
  ++s->count;s->vertex_count+=(unsigned)draw->vertex_count;s->index_count+=(unsigned)draw->index_count;
  return true;
}

const DioramaDepthSink *ArSdlDioramaDepthSink_Begin(ArSdlDioramaDepthSink *s,ArRenderDevice *device) {
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(device);
  if (!s || !gpu || s->owner || (s->device && s->device!=gpu)) return NULL;
  if (!ArSdlRenderBackend_SetExternalFlush(device,Flush,s)) return NULL;
  s->device=gpu;s->owner=device;s->failed=false;
  s->hybrid=false;
  s->sink=(DioramaDepthSink){.submit=Submit,.userdata=s,.clear=Clear,.finish=Finish};
  s->stats.draws=s->stats.submissions=s->stats.largest_batch=0;
  for (unsigned i=0;i<4;++i) s->targets[i].valid=false;
  return &s->sink;
}

const DioramaDepthSink *ArSdlDioramaDepthSink_BeginHybrid(ArSdlDioramaDepthSink *s,ArRenderDevice *device) {
  const DioramaDepthSink *sink=ArSdlDioramaDepthSink_Begin(s,device);
  if (sink) s->hybrid=true;
  return sink;
}

static ArSdlDioramaDepthSink s_production_depth;

bool DioramaDepthGpu_Prepare(ArRenderDevice *device) {
  /* Reserve the representative room workload and all material pipelines at
   * setup. Larger rooms grow bounded storage through the same adapter. */
  return ArSdlDioramaDepthSink_PrepareHybridTarget(&s_production_depth,device,
      ArRenderTexture_Invalid(),32768,98304);
}

const DioramaDepthSink *DioramaDepthGpu_Begin(ArRenderDevice *device) {
  SDL_Texture *target=Target(device);
  if (!target || !ArSdlDioramaDepthSink_PrepareHybridTarget(&s_production_depth,device,
      ArSdlRenderBackend_BorrowTexture(target),32768,98304)) return NULL;
  return ArSdlDioramaDepthSink_BeginHybrid(&s_production_depth,device);
}

void DioramaDepthGpu_Reset(ArRenderDevice *device) {
  (void)device;
  ArSdlDioramaDepthSink_Reset(&s_production_depth);
}
