#include "diorama_depth_sink_sdl.h"
#include "diorama_depth_alpha_sdl.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_depth_gpu.h"
#include "render_sdl_internal.h"
#include "app/runtime_log.h"
#include "render/gpu_compile_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const SDL_GPUTextureFormat kDepthColorFormats[] = {
  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
};

static ArSdlDioramaDepthSink s_production_depth;
static bool s_draw_failed;

static void ReportProductionDepthFailure(const char *stage) {
  if (!s_draw_failed)
    RuntimeLog_Printf("[voxel-depth] %s failed: %s\n", stage, SDL_GetError());
  s_draw_failed = true;
}

static unsigned FormatSlot(SDL_GPUTextureFormat format) {
  for (unsigned i = 0; i < SDL_arraysize(kDepthColorFormats); ++i)
    if (kDepthColorFormats[i] == format) return i;
  return SDL_arraysize(kDepthColorFormats);
}

typedef struct ArSdlDioramaDepthBatch {
  DioramaDepthDraw *draws;
  SDL_GPUTexture **sources;
  ArGpuDioramaDepthMaterial *materials;
  DioramaSceneryLightEffectParams *params;
  SDL_GPUTexture *target;
  unsigned slot, width, height, capacity;
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
    batch->materials[i].scenery=batch->sources[i]?&batch->params[i]:NULL;
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
    /* Submit closed prior SDL drawing before this uninterrupted native batch.
     * Only a backend-owned drain can consume this same-call certificate. */
    (void)ArSdlRenderBackend_CertifyExternalSdlIdle(s->owner,s);
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
  s->prepared.target=NULL;
  if (s == &s_production_depth && (!drained || s->failed))
    ReportProductionDepthFailure("draw");
  return drained && !s->failed;
}

static bool FlushOrderedDraws(void *context,ArRenderDevice *device) {
  ArSdlDioramaDepthSink *s=context;
  return s->owner==device&&!s->failed&&Flush(s);
}

void ArSdlDioramaDepthSink_Reset(ArSdlDioramaDepthSink *s) {
  if (!s) return;
  if (s->owner) (void)Finish(s,s->owner,false);
  for (unsigned i=0;i<4;++i) ArGpuDioramaDepthPass_Destroy(&s->targets[i].pass);
  for (unsigned i = 0; i < SDL_arraysize(s->formats); ++i)
    ArGpuDioramaDepthResources_Destroy(s->formats[i]);
  if(s->batch) {
    free(s->batch->draws);free(s->batch->sources);
    free(s->batch->materials);free(s->batch->params);
    free(s->batch);
  }
  free(s->vertices);free(s->indices);
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
static bool Reserve(ArSdlDioramaDepthSink *s,unsigned nd,unsigned nv,unsigned ni) {
  const unsigned max_draws=s->hybrid?kDioramaDepthHybridMaximumDraws:kDioramaDepthMaximumDraws;
  const unsigned max_vertices=s->hybrid?kDioramaDepthHybridMaximumVertices:kDioramaDepthMaximumVertices;
  const unsigned max_indices=s->hybrid?kDioramaDepthHybridMaximumIndices:kDioramaDepthMaximumIndices;
  if(nd>max_draws || nv>max_vertices || ni>max_indices) return false;
  if (!s->batch && !(s->batch=calloc(1,sizeof(*s->batch)))) return false;
  ArSdlDioramaDepthBatch *batch=s->batch;
  if(nd>batch->capacity) {
    const unsigned capacity=Capacity(nd,max_draws);
    DioramaDepthDraw *draws=realloc(batch->draws,capacity*sizeof(*draws));
    if(!draws)return false;
    batch->draws=draws;
    SDL_GPUTexture **sources=realloc(batch->sources,capacity*sizeof(*sources));
    if(!sources)return false;
    batch->sources=sources;
    ArGpuDioramaDepthMaterial *materials=realloc(batch->materials,capacity*sizeof(*materials));
    if(!materials)return false;
    batch->materials=materials;
    DioramaSceneryLightEffectParams *params=realloc(batch->params,capacity*sizeof(*params));
    if(!params)return false;
    batch->params=params;
    batch->capacity=capacity;
  }
  if (nv>s->capacity) {
    const unsigned capacity=Capacity(nv,max_vertices);
    DioramaDepthVertex *vertices=realloc(s->vertices,capacity*sizeof(*vertices));
    if (!vertices) return false;
    s->vertices=vertices;s->capacity=capacity;
  }
  if (ni>s->index_capacity) {
    const unsigned capacity=Capacity(ni,max_indices);
    int32_t *indices=realloc(s->indices,capacity*sizeof(*indices));
    if (!indices) return false;
    s->indices=indices;s->index_capacity=capacity;
  }
  return true;
}

static unsigned FindTargetSlot(ArSdlDioramaDepthSink *s,SDL_Texture *target) {
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
  return slot;
}

static ArGpuDioramaDepthResources *ResourcesForFormat(ArSdlDioramaDepthSink *s,
    SDL_GPUTextureFormat format) {
  const unsigned i = FormatSlot(format);
  if (i == SDL_arraysize(kDepthColorFormats)) return NULL;
  if (!s->formats[i]) s->formats[i] = ArGpuDioramaDepthResources_Create(s->device, format);
  return s->formats[i];
}

static unsigned TargetSlot(ArSdlDioramaDepthSink *s, SDL_Texture *target,
    SDL_GPUTextureFormat format) {
  const unsigned slot = FindTargetSlot(s, target);
  if (slot == 4) return slot;
  ArGpuDioramaDepthPass *pass = &s->targets[slot].pass;
  if (pass->resources && pass->resources->color_format == format) return slot;
  ArGpuDioramaDepthPass_Destroy(pass);
  s->targets[slot].valid = false;
  ArGpuDioramaDepthResources *resources = ResourcesForFormat(s, format);
  if (!resources) return 4;
  ArGpuDioramaDepthPass_BorrowResources(pass, resources);
  return slot;
}

typedef struct DepthTargetInfo {
  SDL_GPUTexture *attachment;
  SDL_GPUTextureFormat format;
  unsigned width, height, slot;
} DepthTargetInfo;

static bool ReadDepthTarget(SDL_Texture *target, DepthTargetInfo *out) {
  if (!target) return false;
  const SDL_PropertiesID properties = SDL_GetTextureProperties(target);
  SDL_GPUTexture *attachment = SDL_GetPointerProperty(properties,
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
  const Sint64 width = SDL_GetNumberProperty(properties, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0);
  const Sint64 height = SDL_GetNumberProperty(properties, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0);
  if (!attachment || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
      SDL_GetNumberProperty(properties, SDL_PROP_TEXTURE_ACCESS_NUMBER, -1) != SDL_TEXTUREACCESS_TARGET)
    return false;
  *out = (DepthTargetInfo){
    .attachment = attachment,
    .format = SDL_GetGPUTextureFormatFromPixelFormat((SDL_PixelFormat)
        SDL_GetNumberProperty(properties, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN)),
    .width = (unsigned)width,
    .height = (unsigned)height,
  };
  return true;
}

static bool PrepareTarget(ArSdlDioramaDepthSink *s, ArRenderDevice *device,
    SDL_Texture *target, unsigned vertices, unsigned indices, bool hybrid, DepthTargetInfo *prepared) {
  SDL_GPUDevice *gpu = ArSdlRenderBackend_GpuDevice(device);
  if (!s || !gpu || s->owner || (s->device && s->device != gpu) ||
      vertices > kDioramaDepthMaximumVertices || indices > kDioramaDepthMaximumIndices)
    return false;
  DepthTargetInfo info;
  if (!ReadDepthTarget(target, &info)) return false;
  s->device = gpu;
  info.slot = TargetSlot(s, target, info.format);
  const bool ok = info.slot < 4 && Reserve(s, kDioramaDepthMaximumDraws, vertices, indices) &&
      (hybrid ? ArGpuDioramaDepthPass_PrepareHybrid : ArGpuDioramaDepthPass_Prepare)(
          &s->targets[info.slot].pass, info.width, info.height, vertices, indices);
  if (ok && prepared) *prepared = info;
  return ok;
}

static SDL_Texture *ExplicitTarget(ArRenderDevice *device, ArRenderTexture handle) {
  if (ArRenderTexture_IsValid(handle)) return ArSdlRenderBackend_UnwrapTexture(handle);
  return ArSdlRenderBackend_Renderer(device)
      ? ((ArSdlRenderBackend *)device->context)->output_target : NULL;
}

bool ArSdlDioramaDepthSink_PrepareTarget(ArSdlDioramaDepthSink *s, ArRenderDevice *device,
    ArRenderTexture handle, unsigned vertices, unsigned indices) {
  return PrepareTarget(s, device, ExplicitTarget(device, handle), vertices, indices, false, NULL);
}

bool ArSdlDioramaDepthSink_PrepareHybridTarget(ArSdlDioramaDepthSink *s, ArRenderDevice *device,
    ArRenderTexture handle, unsigned vertices, unsigned indices) {
  return PrepareTarget(s, device, ExplicitTarget(device, handle), vertices, indices, true, NULL);
}

static bool SubmitDepthDraw(ArSdlDioramaDepthSink *s,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  if (s->failed || s->owner!=device || !draw) return false;
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(device);
  SDL_Texture *target=Target(device), *art=ArSdlRenderBackend_UnwrapTexture(draw->texture);
  /* Target mutation drains this scope. Reuse the validated size, format
   * slot and GPU attachment while it still owns the same SDL target. */
  const bool same_target=s->count && target==s->targets[s->batch->slot].target;
  SDL_GPUTexture *destination, *source=Texture(art);
  SDL_Rect viewport,clip;
  float scale_x,scale_y;
  if (!renderer || !target || s->device!=ArSdlRenderBackend_GpuDevice(device) ||
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
  int width,height;
  unsigned slot;
  if(same_target) {
    destination=s->batch->target;
    width=(int)s->batch->width;height=(int)s->batch->height;slot=s->batch->slot;
  } else if (target == s->prepared.target && s->targets[s->prepared.slot].target == target) {
    if (s->count && !Flush(s)) return false;
    destination = s->prepared.attachment;
    width = (int)s->prepared.width;
    height = (int)s->prepared.height;
    slot = s->prepared.slot;
  } else {
    DepthTargetInfo info;
    if (!ReadDepthTarget(target, &info)) return false;
    destination=info.attachment;
    width=(int)info.width;
    height=(int)info.height;
    if(s->count && !Flush(s))return false;
    slot=TargetSlot(s,target,info.format);
  }
  if (width<=0 || height<=0 || slot==4) return false;
  clip=viewport;
  if (SDL_RenderClipEnabled(renderer)) {
    SDL_Rect local;
    if (!SDL_GetRenderClipRect(renderer,&local)) return false;
    local.x+=viewport.x;local.y+=viewport.y;
    if (!SDL_GetRectIntersection(&clip,&local,&clip)) return true;
  }
  const SDL_Rect bounds={0,0,width,height};
  if (!SDL_GetRectIntersection(&clip,&bounds,&clip)) return true;
  /* A hybrid scope must retain both passes until a real ordering boundary.
   * Flushing mid-scope would resolve fractional faces against partial depth. */
  if (s->count && !s->hybrid &&
      (s->count==kDioramaDepthMaximumDraws ||
       (unsigned)draw->vertex_count>kDioramaDepthMaximumVertices-s->vertex_count ||
       (unsigned)draw->index_count>kDioramaDepthMaximumIndices-s->index_count)) {
    if (!Flush(s)) return false;
  }
  SDL_ScaleMode filter=SDL_SCALEMODE_NEAREST;
  if (art && !SDL_GetTextureScaleMode(art,&filter)) return false;
  DioramaSceneryLightEffectParams params=draw->lighting?*draw->lighting:
      (DioramaSceneryLightEffectParams){.shade={1,1,1,1}};
  if(params.heat_owned.mode>0) {
    if(!draw->heat_world_xy||draw->native_actor||!draw->clip_w)return false;
    for(int i=0;i<draw->vertex_count;++i)
      if(!isfinite(draw->heat_world_xy[i].x)||!isfinite(draw->heat_world_xy[i].y))return false;
  }
  const bool lit=ArRenderTexture_IsValid(params.illumination);
  ArGpuDioramaDepthMaterial material={
      .actor=source && draw->native_actor && (!lit || ArRenderTexture_IsValid(params.attributes))};
  const DioramaDepthTextureBank *bank=draw->texture_bank;
  if(bank) {
    if(!bank->slots || !source || filter!=SDL_SCALEMODE_NEAREST || draw->native_actor ||
        draw->blend==kArRenderBlendMode_Add || draw->heat_world_xy ||
        DioramaHeat_Active(&params.heat) || params.heat_owned.mode>0) return false;
    for(unsigned i=0;i<2;++i) {
      SDL_Texture *alternate=ArSdlRenderBackend_UnwrapTexture(bank->additional[i]);
      SDL_ScaleMode alternate_filter;
      material.additional[i]=Texture(alternate);
      if(!material.additional[i] || !SDL_GetTextureScaleMode(alternate,&alternate_filter) ||
          alternate_filter!=SDL_SCALEMODE_NEAREST) return false;
    }
    for(int i=0;i<draw->vertex_count;++i)if(bank->slots[i]>2)return false;
  }
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
  if (!Reserve(s,s->count+1,s->vertex_count+(unsigned)draw->vertex_count,
      s->index_count+(unsigned)draw->index_count)) return false;
  /* Close preceding SDL work once at the start of an uninterrupted batch.
   * The backend drains this batch before the next ordinary draw or mutation. */
  if (!s->count && !ArSdlRenderBackend_SubmitPending(device)) return false;
  ArSdlDioramaDepthBatch *batch=s->batch;
  batch->target=destination;batch->slot=slot;batch->width=width;batch->height=height;
  bool cutout=draw->binary_texture_alpha && source && filter==SDL_SCALEMODE_NEAREST &&
      !draw->native_actor && draw->blend==kArRenderBlendMode_Alpha && params.mode<=.5f;
  for (int i=0;i<draw->vertex_count;++i) {
    const ArRenderVertex2D *v=&draw->vertices[i];
    cutout &= v->color.a==1.f;
    const float w=draw->clip_w?draw->clip_w[i]:1;
    s->vertices[s->vertex_count+(unsigned)i]=(DioramaDepthVertex){
        .clip={(2*(v->position.x+viewport.x)/width-1)*w,
            (1-2*(v->position.y+viewport.y)/height)*w,draw->depths[i]*w,w},
        .color=v->color,.uv=v->tex_coord,
        .heat_world_xy=bank?(ArRenderPointF){bank->slots[i],0}:
            draw->heat_world_xy?draw->heat_world_xy[i]:(ArRenderPointF){0}};
  }
  memcpy(s->indices+s->index_count,draw->indices,(size_t)draw->index_count*sizeof(*s->indices));
  batch->draws[s->count]=(DioramaDepthDraw){
      .vertex_count=(unsigned)draw->vertex_count,.index_count=(unsigned)draw->index_count,
      .depth_mode=draw->native_actor?kDioramaDepth_NativeActor:
          (draw->read_only_depth || draw->blend==kArRenderBlendMode_Add)?kDioramaDepth_Read:kDioramaDepth_Write,
      .blend=draw->blend,.textured=source!=NULL,.nearest=filter==SDL_SCALEMODE_NEAREST,
      .cutout=cutout,
      .clip_enabled=true,.clip={clip.x,clip.y,clip.w,clip.h}};
  batch->params[s->count]=params;
  batch->materials[s->count]=material;
  batch->sources[s->count]=source;
  ++s->count;s->vertex_count+=(unsigned)draw->vertex_count;s->index_count+=(unsigned)draw->index_count;
  return true;
}

static bool Submit(void *context, ArRenderDevice *device, const DioramaDepthSubmission *draw) {
  ArSdlDioramaDepthSink *s = context;
  if (SubmitDepthDraw(s, device, draw)) return true;
  if (s == &s_production_depth && s->owner == device) s->failed = true;
  return false;
}

const DioramaDepthSink *ArSdlDioramaDepthSink_Begin(ArSdlDioramaDepthSink *s,ArRenderDevice *device) {
  SDL_GPUDevice *gpu=ArSdlRenderBackend_GpuDevice(device);
  if (!s || !gpu || s->owner || (s->device && s->device!=gpu)) return NULL;
  if (!ArSdlRenderBackend_SetExternalFlush(device,Flush,s)) return NULL;
  s->device=gpu;s->owner=device;s->failed=false;
  s->hybrid=false;
  s->sink=(DioramaDepthSink){.submit=Submit,.userdata=s,.texture_banks=true,.clear=Clear,
      .finish=Finish,.flush=FlushOrderedDraws};
  s->stats.draws=s->stats.submissions=s->stats.largest_batch=0;
  for (unsigned i=0;i<4;++i) s->targets[i].valid=false;
  return &s->sink;
}

const DioramaDepthSink *ArSdlDioramaDepthSink_BeginHybrid(ArSdlDioramaDepthSink *s,ArRenderDevice *device) {
  const DioramaDepthSink *sink=ArSdlDioramaDepthSink_Begin(s,device);
  if (sink) s->hybrid=true;
  return sink;
}

enum { kRoomVertices = 32768, kRoomIndices = 98304 };

static struct {
  SDL_Thread *thread;
  SDL_GPUDevice *gpu;
  SDL_GPUTextureFormat format;
  ArGpuDioramaDepthResources *resources;
  SDL_AtomicInt done;
  DioramaDepthPreparation result;
  bool cancelled;
} s_preparation;

static bool PrepareFormat(ArGpuDioramaDepthResources *resources) {
  return ArGpuDioramaDepthResources_Prepare(resources) &&
      ArGpuDioramaDepthAlpha_PreparePipelines(resources);
}

static int SDLCALL PrepareDepthOnWorker(void *context) {
  (void)context;
  GpuCompileLog_BeginPreparation(NULL, NULL);
  ArGpuDioramaDepthResources *resources =
      ArGpuDioramaDepthResources_Create(s_preparation.gpu, s_preparation.format);
  if (!PrepareFormat(resources)) {
    RuntimeLog_Printf("[voxel-depth] cannot prepare format %d: %s\n",
        (int)s_preparation.format, SDL_GetError());
    ArGpuDioramaDepthResources_Destroy(resources);
    resources = NULL;
  }
  s_preparation.resources = resources;
  GpuCompileLog_EndPreparation();
  SDL_SetAtomicInt(&s_preparation.done, 1);
  return 0;
}

static SDL_GPUTextureFormat RequiredFormat(ArRenderDevice *device, DioramaDepthTarget target) {
  if (target == kDioramaDepthTarget_Crt)
    return SDL_GetGPUTextureFormatFromPixelFormat(ArSdlRenderBackend_PreferredTextureFormat(
        ArSdlRenderBackend_Renderer(device)));
  if (target == kDioramaDepthTarget_SceneScale)
    return SDL_GetGPUTextureFormatFromPixelFormat(SDL_PIXELFORMAT_ARGB8888);
  if (target != kDioramaDepthTarget_Output) return SDL_GPU_TEXTUREFORMAT_INVALID;
  DepthTargetInfo info;
  return ReadDepthTarget(ExplicitTarget(device, ArRenderTexture_Invalid()), &info)
      ? info.format : SDL_GPU_TEXTUREFORMAT_INVALID;
}

/* The render owner publishes resources only after completion, and only for
 * the device and format actually requested. Reset also joins a cancelled job
 * before destroying its device. */
static void JoinDepthPreparation(SDL_GPUDevice *gpu) {
  SDL_WaitThread(s_preparation.thread, NULL);
  s_preparation.thread = NULL;
  ArGpuDioramaDepthResources *resources = s_preparation.resources;
  s_preparation.resources = NULL;
  if (s_preparation.cancelled || !gpu || gpu != s_preparation.gpu ||
      (s_production_depth.device && s_production_depth.device != gpu)) {
    s_preparation.result = kDioramaDepthPreparation_Cancelled;
  } else if (!resources) {
    s_preparation.result = kDioramaDepthPreparation_Failed;
  } else {
    const unsigned i = FormatSlot(resources->color_format);
    s_production_depth.device = gpu;
    /* A pass may already borrow the resident object. Keep its lifetime intact. */
    ArGpuDioramaDepthResources *resident = s_production_depth.formats[i];
    if (!resident) {
      s_production_depth.formats[i] = resources;
      resources = NULL;
    } else if (!resident->hybrid_ready) {
      ArGpuDioramaDepthResources partial = *resident;
      *resident = *resources;
      *resources = partial;
    }
    s_preparation.result = kDioramaDepthPreparation_Ready;
  }
  ArGpuDioramaDepthResources_Destroy(resources);
}

DioramaDepthPreparation DioramaDepthGpu_Poll(ArRenderDevice *device) {
  if (!s_preparation.thread) return s_preparation.result;
  if (!SDL_GetAtomicInt(&s_preparation.done)) return kDioramaDepthPreparation_Pending;
  JoinDepthPreparation(ArSdlRenderBackend_GpuDevice(device));
  return s_preparation.result;
}

DioramaDepthPreparation DioramaDepthGpu_Request(ArRenderDevice *device, DioramaDepthTarget target) {
  SDL_GPUDevice *gpu = ArSdlRenderBackend_GpuDevice(device);
  if (!gpu || !ArRenderDevice_IsReady(device)) return kDioramaDepthPreparation_Failed;
  const SDL_GPUTextureFormat format = RequiredFormat(device, target);
  const unsigned slot = FormatSlot(format);
  if (slot == SDL_arraysize(kDepthColorFormats) ||
      (s_production_depth.device && s_production_depth.device != gpu))
    return kDioramaDepthPreparation_Failed;
  if (s_preparation.thread && (s_preparation.format != format || s_preparation.gpu != gpu))
    DioramaDepthGpu_CancelPreparation();
  (void)DioramaDepthGpu_Poll(device);
  const ArGpuDioramaDepthResources *resident = s_production_depth.formats[slot];
  if (resident && resident->hybrid_ready) {
    if (!s_preparation.thread) s_preparation.result = kDioramaDepthPreparation_Ready;
    return kDioramaDepthPreparation_Ready;
  }
  if (s_preparation.thread) return kDioramaDepthPreparation_Pending;
  s_preparation.gpu = gpu;
  s_preparation.format = format;
  s_preparation.cancelled = false;
  SDL_SetAtomicInt(&s_preparation.done, 0);
  s_preparation.thread = SDL_CreateThread(PrepareDepthOnWorker, "voxel-depth", NULL);
  s_preparation.result = s_preparation.thread ? kDioramaDepthPreparation_Pending
                                               : kDioramaDepthPreparation_Failed;
  return s_preparation.result;
}

void DioramaDepthGpu_CancelPreparation(void) {
  if (s_preparation.thread) s_preparation.cancelled = true;
}

bool DioramaDepthGpu_DrawFailed(void) { return s_draw_failed; }

bool DioramaDepthGpu_TakeDrawFailure(void) {
  const bool failed = s_draw_failed;
  s_draw_failed = false;
  return failed;
}

const DioramaDepthSink *DioramaDepthGpu_Begin(ArRenderDevice *device, bool needs_subscreen) {
  if (s_draw_failed) return NULL;
  SDL_Texture *target = Target(device);
  DepthTargetInfo info;
  if (PrepareTarget(&s_production_depth, device, target, kRoomVertices, kRoomIndices, true, &info) &&
      (!needs_subscreen || PrepareFormat(ResourcesForFormat(&s_production_depth,
          SDL_GetGPUTextureFormatFromPixelFormat(SDL_PIXELFORMAT_ARGB8888))))) {
    const DioramaDepthSink *sink = ArSdlDioramaDepthSink_BeginHybrid(&s_production_depth, device);
    if (sink) {
      s_production_depth.prepared.target = target;
      s_production_depth.prepared.attachment = info.attachment;
      s_production_depth.prepared.slot = info.slot;
      s_production_depth.prepared.width = info.width;
      s_production_depth.prepared.height = info.height;
      return sink;
    }
  }
  ReportProductionDepthFailure("target setup");
  return NULL;
}

void DioramaDepthGpu_Reset(ArRenderDevice *device) {
  (void)device;
  if (s_preparation.thread) JoinDepthPreparation(NULL);
  ArSdlDioramaDepthSink_Reset(&s_production_depth);
  s_preparation.result = kDioramaDepthPreparation_Idle;
  s_draw_failed = false;
}
