#include "diorama/diorama_gpu_policy.h"
#include "diorama/diorama_frame_generation.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present/present.h"
#include "platform/sdl/presentation_geometry_sdl.h"
#include "platform/sdl/render_sdl_internal.h"
#include "platform/sdl/gpu_global_motion_sdl.h"
#include "platform/sdl/gpu_block_motion_sdl.h"
#include "platform/sdl/gpu_frame_handoff_sdl.h"
#include "platform/sdl/action_effect_source_sdl.h"
#include "present/presentation_frame_generation.h"

enum {
  kFrameGenerationMaximumPairSpanMs = 50,
  kFrameGenerationTextureBytes =
      kFrameSlotLayerTextureWidth * kFrameSlotLayerTextureHeight *
      (int)sizeof(uint32_t),
  kFrameGenerationMaximumVertices =
      (kPresentationFrameGenerationMaximumBlocksX + 1) *
      (kPresentationFrameGenerationMaximumBlocksY + 1),
  kFrameGenerationMaximumIndices =
      kPresentationFrameGenerationMaximumBlocks * 6,
};

typedef struct DioramaFrameGenerationPlane {
  uint32_t *previous_pixels;
  uint32_t *current_pixels;
  SDL_Texture *previous_texture;
  SDL_Texture *current_texture;
  SDL_Texture *generated_texture;
  int output_x;
  int texture_width;
  int texture_height;
  PresentationFrameGenerationMotionField motion;
  bool current_valid;
  bool pair_valid;
  bool gpu_only;
  bool generated_padding_valid;
} DioramaFrameGenerationPlane;

typedef struct DioramaFrameGenerationKey {
  uint64_t timestamp_ns;
  uint32_t plane_mask;
  uint32_t additive_plane_mask;
  int width;
  int height;
  uint8_t bg_mode;
  uint8_t map_group;
  uint8_t map_number;
  uint8_t layer_section;
  bool valid;
} DioramaFrameGenerationKey;

enum { kFrameGenerationPlaneCount = kDioramaPlane_Count + 1 };
static DioramaFrameGenerationPlane s_planes[kFrameGenerationPlaneCount];
static DioramaFrameGenerationKey s_last_key;
static ArRenderPointF s_present_offsets[kFrameGenerationPlaneCount];
static uint64_t s_pair_timestamp_ns;
static uint32_t s_pair_mask;
static uint32_t s_generated_mask;
static bool s_wait_trace;
static uint64_t s_last_wait_ns;
static SDL_Vertex s_vertices[kFrameGenerationMaximumVertices];
static int s_indices[kFrameGenerationMaximumIndices];
static int s_index_blocks_x = -1;
static int s_index_blocks_y = -1;
static const int kQuadIndices[] = {0, 1, 2, 1, 3, 2};

static DioramaGpuPolicy s_policy;

/* Owned mode keeps background/skybox/residual and actor
 * analysis/synthesis on GPU without CPU image copies. Only global vectors are
 * downloaded for the existing CPU effect projection. Validation also retains
 * CPU analysis and reads the actor field to check exact parity. */
static bool GpuOwnsAnalysis(void) {
  return s_policy.motion == kDioramaGpuMotion_Owned;
}

static bool GpuMotionPlane(int plane) {
  return plane == SR_PPU_OVERLAY_BG1 || plane == SR_PPU_OVERLAY_BG2 ||
      plane == kDioramaPlane_Bg1Hi || plane == kDioramaPlane_Bg2Hi ||
      plane == kDioramaPlane_Bg1Far || plane == kDioramaPlane_Bg2Far ||
      plane == kDioramaFrameGenerationSkybox || plane == kDioramaPlane_Backdrop ||
      DioramaPlaneIsObjectPriority(plane);
}

static const int kGpuPlanes[12] = {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi,
    kDioramaPlane_Bg1Far, SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far,
    kDioramaFrameGenerationSkybox, kDioramaPlane_Backdrop,
    SR_PPU_OVERLAY_OBJ, kDioramaPlane_Obj1, kDioramaPlane_Obj2, kDioramaPlane_Obj3};
static struct {
  ArGpuGlobalMotion motion;
  ArGpuBlockMotion blocks;
  ArGpuActionScenePass pack;
  ArGpuEffectSource effects;
  SDL_GPUBuffer *identity_motion;
  bool resident, effects_attempted, effects_disabled, effects_failed, failed;
  float phase;
  unsigned resident_captures, metadata_downloads, source_draws, source_primitives;
  SDL_Texture *previous, *current, *output, *block_output;
  uint32_t mask, presented_mask;
  SDL_GPUTransferBuffer *validation;
  SDL_GPUFence *projection_fence;
  uint32_t pending_analysis_mask;
  unsigned checks, errors;
  bool attempted, ready, endpoint;
} s_gpu;
static bool s_source_allowed;
void DioramaFrameGeneration_AllowSourceProjection(bool allowed) { s_source_allowed = allowed; }
bool DioramaFrameGeneration_SourceProjectionActive(void) { return s_gpu.resident; }
bool DioramaFrameGeneration_SourceProjectionFailed(void) { return s_gpu.effects_failed; }
unsigned DioramaFrameGeneration_MetadataReadbackCount(void) { return s_gpu.metadata_downloads; }

/* Diagnostic stage controls also permit isolated packing/unpacking comparisons. */
static bool NativeHandoff(bool packing) {
  return packing ? s_policy.pack : s_policy.unpack;
}

/* One immutable native job. Renderer handles and all frame metadata are
 * resolved on main; the worker never reads a FrameSlot, settings or WRAM.
 * Scratch/resources remain borrowed until FinishCapture publishes submission.
 * GPU lifetime is governed by ordered submissions, not semaphore completion. */
typedef struct NativeCaptureJob {
  ArGpuGlobalMotion motion;
  ArGpuBlockMotion blocks;
  ArGpuActionScenePass *pack;
  ArGpuFramePack planes[kArGpuFrameHandoffMaximumPlanes];
  SDL_GPUTexture *previous, *current;
  SDL_GPUTransferBuffer *download;
  unsigned count;
  uint32_t mask;
  bool ok;
  SDL_GPUFence *fence;
  uint64_t queued_ns, started_ns, submitted_ns;
} NativeCaptureJob;

static struct {
  SDL_Thread *thread;
  SDL_Semaphore *start, *done;
  NativeCaptureJob job;
  bool attempted, pending, stop;
  FILE *trace;
  unsigned jobs;
} s_capture;

static void RunNativeCapture(NativeCaptureJob *job) {
  job->started_ns = SDL_GetTicksNS();
  SDL_GPUDevice *gpu = job->motion.device;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
  if (!cmd) goto finished;
  if (!ArGpuFrameHandoff_EncodePack(job->pack, cmd, job->current,
          job->motion.width, job->motion.height * 12, job->motion.motion,
          job->planes, job->count)) goto cancel;
  if (!job->mask) {
    job->ok = SDL_SubmitGPUCommandBuffer(cmd);
    goto finished;
  }
  if (!ArGpuGlobalMotion_Analyze(&job->motion, cmd, job->previous, job->current))
    goto cancel;
  if (!job->download) {
    if (!ArGpuBlockMotion_Analyze(&job->blocks, cmd, job->previous, job->current)) goto cancel;
    job->ok = SDL_SubmitGPUCommandBuffer(cmd);
    goto finished;
  }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) goto cancel;
  const SDL_GPUBufferRegion source = {.buffer = job->motion.motion,
      .size = 8 * sizeof(ArGpuGlobalMotionResult)};
  const SDL_GPUTransferBufferLocation dest = {.transfer_buffer = job->download};
  SDL_DownloadFromGPUBuffer(copy, &source, &dest);
  SDL_EndGPUCopyPass(copy);
  job->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!job->fence) goto finished;
  /* Preserve the existing fence-before-actor-search ordering exactly. */
  cmd = SDL_AcquireGPUCommandBuffer(gpu);
  if (!cmd) goto finished;
  if (!ArGpuBlockMotion_Analyze(&job->blocks, cmd, job->previous, job->current))
    goto cancel;
  job->ok = SDL_SubmitGPUCommandBuffer(cmd);
  goto finished;
cancel:
  SDL_CancelGPUCommandBuffer(cmd);
finished:
  job->submitted_ns = SDL_GetTicksNS();
}

static int SDLCALL NativeCaptureWorker(void *unused) {
  (void)unused;
  for (;;) {
    SDL_WaitSemaphore(s_capture.start);
    if (s_capture.stop) break;
    RunNativeCapture(&s_capture.job);
    SDL_SignalSemaphore(s_capture.done);
  }
  return 0;
}

void DioramaFrameGeneration_FinishCapture(void) {
  if (!s_capture.pending) return;
  const uint64_t start = SDL_GetTicksNS();
  if (s_capture.thread) SDL_WaitSemaphore(s_capture.done);
  const uint64_t complete = SDL_GetTicksNS();
  const NativeCaptureJob *job = &s_capture.job;
  s_gpu.endpoint = job->ok;
  s_gpu.mask = job->ok ? job->mask : 0;
  if (!job->ok) {
    s_pair_mask &= ~job->mask;
    s_gpu.failed = true;
    s_gpu.resident = false;
    SDL_Log("[gpu-frame-generation] native capture failed; retaining reference until renderer reset");
  }
  s_gpu.projection_fence = job->fence;
  if (s_capture.trace) fprintf(s_capture.trace, "%u,%d,%llu,%llu,%llu,%llu,%llu,%u,%d\n",
      ++s_capture.jobs, s_capture.thread != NULL,
      (unsigned long long)job->queued_ns, (unsigned long long)job->started_ns,
      (unsigned long long)job->submitted_ns, (unsigned long long)start,
      (unsigned long long)complete, job->mask, job->ok);
  s_capture.pending = false;
}

static void StopNativeCapture(void) {
  DioramaFrameGeneration_FinishCapture();
  if (s_capture.thread) {
    s_capture.stop = true;
    SDL_SignalSemaphore(s_capture.start);
    SDL_WaitThread(s_capture.thread, NULL);
  }
  if (s_capture.start) SDL_DestroySemaphore(s_capture.start);
  if (s_capture.done) SDL_DestroySemaphore(s_capture.done);
  if (s_capture.trace) fclose(s_capture.trace);
  memset(&s_capture, 0, sizeof(s_capture));
}

static void StartNativeCapture(void) {
  if (s_capture.attempted) return;
  s_capture.attempted = true;
  const char *path = getenv("AR_GPU_PREPARE_TRACE");
  if (path && *path) {
    s_capture.trace = fopen(path, "a");
    if (s_capture.trace) {
      setvbuf(s_capture.trace, NULL, _IOFBF, 65536);
      if (ftell(s_capture.trace) == 0)
        fprintf(s_capture.trace, "job,worker,queued_ns,started_ns,submitted_ns,join_ns,joined_ns,mask,ok\n");
    }
  }
  const char *worker = getenv("AR_GPU_PREPARE_WORKER");
  if (!worker || strcmp(worker, "1")) return;
  s_capture.start = SDL_CreateSemaphore(0);
  s_capture.done = SDL_CreateSemaphore(0);
  if (s_capture.start && s_capture.done)
    s_capture.thread = SDL_CreateThread(NativeCaptureWorker, "GPU preparation", NULL);
  if (!s_capture.thread) SDL_Log("[gpu-prepare-worker] unavailable; using inline preparation");
  else SDL_Log("[gpu-prepare-worker] native packing/analysis worker active");
}

static void ResetGpu(void) {
  StopNativeCapture();
  if (s_gpu.resident_captures || s_gpu.metadata_downloads)
    SDL_Log("[gpu-effect-projection] resident-captures=%u metadata-downloads=%u bytes=%u source-draws=%u primitives=%u",
        s_gpu.resident_captures, s_gpu.metadata_downloads, s_gpu.metadata_downloads * 256,
        s_gpu.source_draws, s_gpu.source_primitives);
  if (s_gpu.identity_motion) SDL_ReleaseGPUBuffer(s_gpu.effects.device, s_gpu.identity_motion);
  ArGpuEffectSource_Destroy(&s_gpu.effects);
  if (s_gpu.projection_fence) SDL_ReleaseGPUFence(s_gpu.motion.device, s_gpu.projection_fence);
  SDL_DestroyTexture(s_gpu.previous);
  SDL_DestroyTexture(s_gpu.current);
  SDL_DestroyTexture(s_gpu.output);
  SDL_DestroyTexture(s_gpu.block_output);
  ArGpuBlockMotion_Destroy(&s_gpu.blocks);
  ArGpuActionScenePass_Destroy(&s_gpu.pack);
  if (s_gpu.validation) SDL_ReleaseGPUTransferBuffer(s_gpu.motion.device, s_gpu.validation);
  if (s_gpu.motion.device) ArGpuGlobalMotion_Destroy(&s_gpu.motion);
  memset(&s_gpu, 0, sizeof(s_gpu));
}

static bool EnsureGpu(ArRenderDevice *device, int width, int height) {
  if (s_policy.motion == kDioramaGpuMotion_Off || s_gpu.failed) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const ArSdlRenderBackend *backend = renderer ? device->context : NULL;
  /* External/legacy renderers cannot submit without also presenting their
   * window. Only the ordered adapter establishes this dependency boundary. */
  if (!backend || !backend->output_window || !backend->gpu_device) return false;
  if (!s_gpu.attempted) {
    s_gpu.attempted = true;
    s_gpu.ready = ArGpuGlobalMotion_Init(&s_gpu.motion, backend->gpu_device) &&
        ArGpuBlockMotion_Init(&s_gpu.blocks, backend->gpu_device);
    s_gpu.motion.plane_count = 8;
    s_gpu.motion.input_plane_count = 12;
  }
  if (!s_gpu.ready) return false;
  if (s_gpu.previous && s_gpu.current && s_gpu.output && s_gpu.motion.height == (unsigned)height && s_gpu.blocks.width == (unsigned)width)
    return true;
  SDL_DestroyTexture(s_gpu.output); s_gpu.output = NULL;
  SDL_DestroyTexture(s_gpu.block_output); s_gpu.block_output = NULL;
  SDL_DestroyTexture(s_gpu.previous); s_gpu.previous = NULL;
  SDL_DestroyTexture(s_gpu.current); s_gpu.current = NULL;
  s_gpu.endpoint = false;
  if (!ArGpuGlobalMotion_Resize(&s_gpu.motion, kFrameSlotLayerTextureWidth, (unsigned)height)) return false;
  if (!ArGpuBlockMotion_Resize(&s_gpu.blocks, (unsigned)width, (unsigned)height)) return false;
  s_gpu.previous = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, kFrameSlotLayerTextureWidth, height * 12);
  s_gpu.current = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, kFrameSlotLayerTextureWidth, height * 12);
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, kFrameSlotLayerTextureWidth);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height * 8);
  SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, s_gpu.motion.output);
  s_gpu.output = SDL_CreateTextureWithProperties(renderer, props);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height * 4);
  SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, s_gpu.blocks.output);
  s_gpu.block_output = SDL_CreateTextureWithProperties(renderer, props);
  SDL_DestroyProperties(props);
  const bool configured = s_gpu.previous && s_gpu.current && s_gpu.output && s_gpu.block_output &&
      SDL_SetTextureBlendMode(s_gpu.block_output, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(s_gpu.block_output, SDL_SCALEMODE_NEAREST) &&
      SDL_SetTextureBlendMode(s_gpu.output, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(s_gpu.output, SDL_SCALEMODE_NEAREST);
  if (!configured) {
    /* Never reuse partially configured targets on the next capture. Latch
     * failure until renderer reset, as with unsupported compute pipelines. */
    ResetGpu();
    s_gpu.attempted = true;
  }
  return configured;
}

bool DioramaFrameGeneration_UsesGpuAnalysis(ArRenderDevice *device, const FrameSlot *slot) {
  if (!slot || !slot->diorama_active || !slot->interp_setting_enabled) return false;
  s_policy = DioramaGpuPolicy_ForRoom(slot->diorama_map_group, slot->diorama_map_number);
  return GpuOwnsAnalysis() && EnsureGpu(device, slot->snes_width + slot->obj_apron * 2,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom);
}

static SDL_GPUTexture *NativeGpuTexture(SDL_Texture *texture) {
  return SDL_GetPointerProperty(SDL_GetTextureProperties(texture),
      SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
}

/* Keep the small projection result asynchronous until presentation actually
 * needs it. The fence precedes actor search: CPU projection must never wait on
 * actor fields, which have no CPU consumer. Validation waits immediately.
 * Append the download to analysis rather than submitting a second command
 * buffer solely for 256 bytes. This function consumes cmd on every path. */
static bool QueueGpuResults(SDL_GPUCommandBuffer *cmd, bool actors) {
  SDL_GPUDevice *gpu = s_gpu.motion.device;
  const unsigned global_bytes = 8 * sizeof(ArGpuGlobalMotionResult);
  const unsigned actor_words = (4 * 880 + 4) * 4;
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
      .size = global_bytes + actor_words * sizeof(int32_t)};
  if (!s_gpu.validation) s_gpu.validation = SDL_CreateGPUTransferBuffer(gpu, &info);
  if (!s_gpu.validation) { SDL_CancelGPUCommandBuffer(cmd); return false; }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) { SDL_CancelGPUCommandBuffer(cmd); return false; }
  const SDL_GPUBufferRegion source = {.buffer = s_gpu.motion.motion, .size = global_bytes};
  const SDL_GPUTransferBufferLocation dest = {.transfer_buffer = s_gpu.validation};
  SDL_DownloadFromGPUBuffer(copy, &source, &dest);
  ++s_gpu.metadata_downloads;
  if (actors) {
    const SDL_GPUBufferRegion field = {.buffer = s_gpu.blocks.motion, .size = actor_words * sizeof(int32_t)};
    const SDL_GPUTransferBufferLocation actor_dest = {.transfer_buffer = s_gpu.validation, .offset = global_bytes};
    SDL_DownloadFromGPUBuffer(copy, &field, &actor_dest);
  }
  SDL_EndGPUCopyPass(copy);
  s_gpu.projection_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  return s_gpu.projection_fence != NULL;
}

static bool ApplyGpuResults(uint32_t analyzed_mask) {
  const bool own = GpuOwnsAnalysis();
  SDL_GPUDevice *gpu = s_gpu.motion.device;
  const unsigned global_bytes = 8 * sizeof(ArGpuGlobalMotionResult);
  if (!s_gpu.projection_fence) return false;
  SDL_GPUFence *fence = s_gpu.projection_fence;
  s_gpu.projection_fence = NULL;
  const uint64_t wait_start = s_wait_trace ? SDL_GetTicksNS() : 0;
  const bool complete = SDL_QueryGPUFence(gpu, fence) || SDL_WaitForGPUFences(gpu, true, &fence, 1);
  if (s_wait_trace) s_last_wait_ns += SDL_GetTicksNS() - wait_start;
  SDL_ReleaseGPUFence(gpu, fence);
  if (!complete) return false;
  const ArGpuGlobalMotionResult *results = SDL_MapGPUTransferBuffer(gpu, s_gpu.validation, false);
  if (!results) return false;
  bool matched = true;
  for (unsigned i = 0; i < 8; ++i) {
    if (!(analyzed_mask & (1u << kGpuPlanes[i]))) continue;
    DioramaFrameGenerationPlane *plane = &s_planes[kGpuPlanes[i]];
    PresentationFrameGenerationMotionField *field = &plane->motion;
    if (own) {
      *field = (PresentationFrameGenerationMotionField){.width = plane->texture_width,
          .height = plane->texture_height, .blocks_x = 1, .blocks_y = 1,
          .uniform = true, .valid = results[i].valid != 0};
      field->forward_dx[0] = (int8_t)results[i].forward_x;
      field->forward_dy[0] = (int8_t)results[i].forward_y;
      field->backward_dx[0] = (int8_t)results[i].backward_x;
      field->backward_dy[0] = (int8_t)results[i].backward_y;
      plane->pair_valid = field->valid;
      s_pair_mask &= ~(1u << kGpuPlanes[i]);
      if (field->valid) s_pair_mask |= 1u << kGpuPlanes[i];
      continue;
    }
    const ArGpuGlobalMotionResult expected = {field->forward_dx[0], field->forward_dy[0],
        field->backward_dx[0], field->backward_dy[0], field->valid, {0}};
    ++s_gpu.checks;
    if (memcmp(results + i, &expected, sizeof(expected))) {
      ++s_gpu.errors; matched = false;
      SDL_Log("[gpu-bg-motion-check] mismatch plane=%d GPU=(%d,%d,%d,%d,%d) CPU=(%d,%d,%d,%d,%d)",
          kGpuPlanes[i], results[i].forward_x, results[i].forward_y,
          results[i].backward_x, results[i].backward_y, results[i].valid,
          expected.forward_x, expected.forward_y, expected.backward_x, expected.backward_y, expected.valid);
    }
  }
  if (!own) {
    const int32_t *actors = (const int32_t *)((const uint8_t *)results + global_bytes);
    for (unsigned i = 0; i < 4; ++i) {
      if (!(analyzed_mask & (1u << kGpuPlanes[8+i]))) continue;
      const PresentationFrameGenerationMotionField *field = &s_planes[kGpuPlanes[8+i]].motion;
      bool same = actors[(4*880+i)*4] == field->valid;
      for (unsigned b = 0; b < 880 && same; ++b) {
        const int32_t *v = actors + (i*880+b)*4;
        same = v[0] == field->forward_dx[b] && v[1] == field->forward_dy[b] &&
            v[2] == field->backward_dx[b] && v[3] == field->backward_dy[b];
      }
      ++s_gpu.checks;
      if (!same) {
        ++s_gpu.errors; matched = false;
        SDL_Log("[gpu-bg-motion-check] mismatch actor plane=%d", kGpuPlanes[8+i]);
      }
    }
  }
  SDL_UnmapGPUTransferBuffer(gpu, s_gpu.validation);
  if (!own && (s_gpu.checks < 7 || s_gpu.checks % 100 < 6))
    SDL_Log("[gpu-bg-motion-check] checks=%u errors=%u", s_gpu.checks, s_gpu.errors);
  return matched;
}

uint32_t DioramaFrameGeneration_GpuPlaneMask(void) { return s_gpu.presented_mask; }
uint32_t DioramaFrameGeneration_GeneratedPlaneMask(void) { return s_generated_mask; }
void DioramaFrameGeneration_EnableWaitTrace(bool enabled) {
  s_wait_trace = enabled;
  s_last_wait_ns = 0;
}
uint64_t DioramaFrameGeneration_LastWaitNs(void) { return s_last_wait_ns; }

/* Encode current endpoint atlas and global analysis together. The atlas is
 * cleared once, source RGBA/BGRA conversion is sampled (never raw-copied), and
 * the original CPU/current texture selection is preserved for oracle mode. */
static bool GatherGpuEndpoints(ArGpuFramePack planes[kArGpuFrameHandoffMaximumPlanes],
    const ArRenderTexture source_textures[kFrameGenerationPlaneCount], uint32_t analyzed_mask,
    int height, unsigned *count, uint32_t *mask) {
  *count = 0;
  *mask = 0;
  for (int i = 0; i < 12; ++i) {
    const int index = kGpuPlanes[i];
    const DioramaFrameGenerationPlane *plane = &s_planes[index];
    if (i < 8) {
      s_gpu.motion.extents[i][0] = (float)(plane->current_valid ? plane->texture_width : 1);
      s_gpu.motion.extents[i][1] = (analyzed_mask & (1u << index)) ? 1.0f : 0.0f;
    } else s_gpu.blocks.extents[i-8][3] = (analyzed_mask & (1u << index)) ? 1.0f : 0.0f;
    if (!plane->current_valid) continue;
    SDL_Texture *source = plane->gpu_only
        ? ArSdlRenderBackend_UnwrapTexture(source_textures[index]) : plane->current_texture;
    if (!source) return false;
    const SDL_PropertiesID props = SDL_GetTextureProperties(source);
    planes[(*count)++] = (ArGpuFramePack){
        .source = {.texture = NativeGpuTexture(source),
          .width = (unsigned)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_WIDTH_NUMBER, 0),
          .height = (unsigned)SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_HEIGHT_NUMBER, 0),
          .source = {(float)(plane->gpu_only ? plane->output_x : 0), 0,
              (float)plane->texture_width, (float)height}},
        .destination = {0, i * height, plane->texture_width, height}};
    if (GpuOwnsAnalysis() ? (analyzed_mask & (1u << index)) != 0 :
        plane->pair_valid && (plane->motion.uniform || i >= 8)) *mask |= 1u << index;
  }
  return *count != 0;
}

static bool PackGpuEndpoints(SDL_GPUCommandBuffer *cmd,
    const ArRenderTexture source_textures[kFrameGenerationPlaneCount], uint32_t analyzed_mask,
    int height, uint32_t *mask) {
  if (!s_gpu.pack.device && !ArGpuActionScenePass_Init(&s_gpu.pack, s_gpu.motion.device)) return false;
  ArGpuFramePack planes[kArGpuFrameHandoffMaximumPlanes];
  unsigned count;
  return GatherGpuEndpoints(planes, source_textures, analyzed_mask, height, &count, mask) &&
      ArGpuFrameHandoff_EncodePack(&s_gpu.pack, cmd, NativeGpuTexture(s_gpu.current),
      kFrameSlotLayerTextureWidth, (unsigned)height * 12, s_gpu.motion.motion, planes, count);
}

static void CaptureOwnedNative(ArRenderDevice *device, bool had_previous, int height,
    uint32_t analyzed_mask, const ArRenderTexture sources[kFrameGenerationPlaneCount]) {
  if (!s_gpu.pack.device && !ArGpuActionScenePass_Init(&s_gpu.pack, s_gpu.motion.device)) return;
  NativeCaptureJob job = {.pack = &s_gpu.pack,
      .previous = NativeGpuTexture(s_gpu.previous), .current = NativeGpuTexture(s_gpu.current)};
  if (!GatherGpuEndpoints(job.planes, sources, analyzed_mask, height, &job.count, &job.mask)) return;
  if (!had_previous) job.mask = 0;
  if (job.mask && !s_gpu.resident && !s_gpu.validation) {
    const SDL_GPUTransferBufferCreateInfo info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = 8 * sizeof(ArGpuGlobalMotionResult) + (4 * 880 + 4) * 4 * sizeof(int32_t)};
    s_gpu.validation = SDL_CreateGPUTransferBuffer(s_gpu.motion.device, &info);
    if (!s_gpu.validation) return;
  }
  if (!ArSdlRenderBackend_SubmitPending(device)) return;
  job.motion = s_gpu.motion;
  job.blocks = s_gpu.blocks;
  job.download = s_gpu.resident ? NULL : s_gpu.validation;
  if (job.mask) {
    if (s_gpu.resident) ++s_gpu.resident_captures;
    else ++s_gpu.metadata_downloads;
  }
  s_gpu.pending_analysis_mask = analyzed_mask;
  s_pair_mask |= job.mask;
  StartNativeCapture();
  job.queued_ns = SDL_GetTicksNS();
  s_capture.job = job;
  s_capture.pending = true;
  if (s_capture.thread) SDL_SignalSemaphore(s_capture.start);
  else {
    RunNativeCapture(&s_capture.job);
    DioramaFrameGeneration_FinishCapture();
  }
}

/* Projection is useful without interpolation too. Initialize only its own
 * resources in that mode, not the large motion-search/capture atlas. The zero
 * candidate mask and disabled skybox slot prevent reading identity storage. */
static bool EnsureSourceProjection(ArRenderDevice *device) {
  if (!s_source_allowed || !s_policy.resident || s_gpu.effects_disabled) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const ArSdlRenderBackend *backend = renderer ? device->context : NULL;
  if (!backend || !backend->output_window || !backend->gpu_device) return false;
  if (!s_gpu.effects_attempted) {
    s_gpu.effects_attempted = true;
    const SDL_GPUBufferCreateInfo info = {.size=256,
        .usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
    if (ArGpuEffectSource_Init(&s_gpu.effects,backend->gpu_device))
      s_gpu.identity_motion = SDL_CreateGPUBuffer(backend->gpu_device,&info);
    if (!s_gpu.identity_motion) {
      s_gpu.effects_disabled = true;
      SDL_Log("[gpu-effect-projection] initialization unavailable; reference path retained: %s",SDL_GetError());
    }
  }
  return !s_gpu.effects_disabled;
}
static SDL_GPUBuffer *SourceMotion(void) {
  return s_gpu.presented_mask ? s_gpu.motion.motion : s_gpu.identity_motion;
}

static void CaptureGpu(ArRenderDevice *device, int width, int height, bool continuous, uint32_t analyzed_mask,
    const ArRenderTexture source_textures[kFrameGenerationPlaneCount]) {
  s_gpu.mask = 0;
  // A drained source may never be presented. Retire its handle without a CPU
  // wait; submissions stay ordered, and only the newest download is mapped.
  if (s_gpu.projection_fence) {
    SDL_ReleaseGPUFence(s_gpu.motion.device, s_gpu.projection_fence);
    s_gpu.projection_fence = NULL;
  }
  s_gpu.resident = false;
  if (!EnsureGpu(device, width, height)) { s_gpu.endpoint = false; return; }
  s_gpu.resident = EnsureSourceProjection(device);
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const bool had_previous = s_gpu.endpoint && continuous;
  s_gpu.endpoint = false;
  SDL_Texture *swap = s_gpu.previous; s_gpu.previous = s_gpu.current; s_gpu.current = swap;
  if (NativeHandoff(true) && GpuOwnsAnalysis()) {
    CaptureOwnedNative(device, had_previous, height, analyzed_mask, source_textures);
    return;
  }
  uint32_t mask = 0;
  SDL_GPUCommandBuffer *cmd = NULL;
  if (NativeHandoff(true)) {
    if (!ArSdlRenderBackend_SubmitPending(device)) return;
    cmd = SDL_AcquireGPUCommandBuffer(s_gpu.motion.device);
    if (!cmd) return;
    if (!PackGpuEndpoints(cmd, source_textures, analyzed_mask, height, &mask)) {
      SDL_CancelGPUCommandBuffer(cmd); return;
    }
  } else {
    SDL_Texture *old_target = SDL_GetRenderTarget(renderer);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a)) return;
    bool ok = SDL_SetRenderTarget(renderer, s_gpu.current) &&
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
        SDL_SetRenderViewport(renderer, NULL) && SDL_SetRenderClipRect(renderer, NULL) &&
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0) && SDL_RenderClear(renderer);

    for (int i = 0; i < 12 && ok; ++i) {
      const int index = kGpuPlanes[i];
      const DioramaFrameGenerationPlane *plane = &s_planes[index];
      if (i < 8) {
        s_gpu.motion.extents[i][0] = (float)(plane->current_valid ? plane->texture_width : 1);
        s_gpu.motion.extents[i][1] = (analyzed_mask & (1u << index)) ? 1.0f : 0.0f;
      } else {
        s_gpu.blocks.extents[i-8][3] = (analyzed_mask & (1u << index)) ? 1.0f : 0.0f;
      }
      if (!plane->current_valid) continue;
      SDL_Texture *source = plane->gpu_only
          ? ArSdlRenderBackend_UnwrapTexture(source_textures[index]) : plane->current_texture;
      SDL_BlendMode blend;
      if (!source || !SDL_GetTextureBlendMode(source, &blend)) { ok = false; break; }
      const SDL_FRect src = {(float)(plane->gpu_only ? plane->output_x : 0), 0,
          (float)plane->texture_width, (float)height};
      const SDL_FRect dest = {0, (float)(i * height), src.w, src.h};
      const bool copied = SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) &&
          SDL_RenderTexture(renderer, source, &src, &dest);
      const bool restored = SDL_SetTextureBlendMode(source, blend);
      ok = copied && restored;
      if (GpuOwnsAnalysis() ? (analyzed_mask & (1u << index)) != 0 :
          plane->pair_valid && (plane->motion.uniform || i >= 8)) mask |= 1u << index;
    }
    const bool target_restored = SDL_SetRenderTarget(renderer, old_target);
    const bool color_restored = SDL_SetRenderDrawColor(renderer, r, g, b, a);
    if (!ok || !target_restored || !color_restored || !ArSdlRenderBackend_SubmitPending(device)) return;
  }
  if (!had_previous || !mask) {
    s_gpu.endpoint = !cmd || SDL_SubmitGPUCommandBuffer(cmd);
    return;
  }
  s_gpu.endpoint = true;
  if (!cmd) cmd = SDL_AcquireGPUCommandBuffer(s_gpu.motion.device);
  if (!cmd) { s_gpu.endpoint = false; return; }
  SDL_GPUTexture *previous = NativeGpuTexture(s_gpu.previous), *current = NativeGpuTexture(s_gpu.current);
  const bool own = GpuOwnsAnalysis();
  if (!previous || !current || !ArGpuGlobalMotion_Analyze(&s_gpu.motion, cmd, previous, current) ||
      (!own && !ArGpuBlockMotion_Analyze(&s_gpu.blocks, cmd, previous, current))) {
    SDL_CancelGPUCommandBuffer(cmd); s_gpu.endpoint = false; return;
  }
  const bool validate = s_policy.motion == kDioramaGpuMotion_Validate;
  if (own || validate) {
    if (!QueueGpuResults(cmd, !own)) { s_gpu.endpoint = false; return; }
    if (validate && !ApplyGpuResults(analyzed_mask)) return;
  } else if (!SDL_SubmitGPUCommandBuffer(cmd)) { s_gpu.endpoint = false; return; }
  if (own) {
    s_gpu.pending_analysis_mask = analyzed_mask;
    // Acceptance is applied when the result is consumed by Prepare. Preserve
    // candidate bits so a BG-only pair can reach that consumption point.
    s_pair_mask |= mask;
    // Actor search remains asynchronous behind the projection download.
    cmd = SDL_AcquireGPUCommandBuffer(s_gpu.motion.device);
    if (!cmd) return;
    if (!ArGpuBlockMotion_Analyze(&s_gpu.blocks, cmd, previous, current)) {
      SDL_CancelGPUCommandBuffer(cmd); return;
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return;
  }
  s_gpu.mask = own ? mask & s_pair_mask : mask;
}

static void ResolveGpuProjection(void) {
  if (s_gpu.projection_fence) {
    if (!ApplyGpuResults(s_gpu.pending_analysis_mask)) {
      s_gpu.mask = 0;
      s_pair_mask &= ~s_gpu.pending_analysis_mask;
    } else s_gpu.mask &= s_pair_mask;
  }
}

static bool UnpackGpuPlanes(SDL_GPUCommandBuffer *cmd, uint32_t mask) {
  ArGpuFrameUnpack planes[kArGpuFrameHandoffMaximumPlanes];
  unsigned count = 0;
  for (int i = 0; i < 12; ++i) {
    const int index = kGpuPlanes[i];
    if (!(mask & (1u << index))) continue;
    const DioramaFrameGenerationPlane *p = &s_planes[index];
    const bool global = i < 8;
    planes[count++] = (ArGpuFrameUnpack){
        .source = global ? s_gpu.motion.output : s_gpu.blocks.output,
        .destination = NativeGpuTexture(p->generated_texture),
        .source_width = global ? s_gpu.motion.width : s_gpu.blocks.width,
        .source_height = s_gpu.motion.height * (global ? 8 : 4),
        .destination_width = kFrameSlotLayerTextureWidth,
        .destination_height = kFrameSlotLayerTextureHeight,
        .source_region = {0, (int)((global ? i : i-8) * s_gpu.motion.height), p->texture_width, p->texture_height},
        .destination_origin = {p->output_x, 0},
        .clear_destination = !p->generated_padding_valid};
  }
  return ArGpuFrameHandoff_EncodeUnpack(cmd, planes, count);
}

static uint32_t PrepareGpu(ArRenderDevice *device, float phase) {
  const char *option = getenv("AR_GPU_PROJECTION_PREPARE_FIRST");
  const bool prepare_first = !option || strcmp(option, "0") != 0;
  if (!prepare_first) ResolveGpuProjection();
  if (!s_gpu.mask || !ArSdlRenderBackend_SubmitPending(device)) return 0;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_gpu.motion.device);
  if (!cmd) return 0;
  if (!ArGpuGlobalMotion_Warp(&s_gpu.motion, cmd, NativeGpuTexture(s_gpu.previous),
          NativeGpuTexture(s_gpu.current), phase)) {
    SDL_CancelGPUCommandBuffer(cmd); return 0;
  }
  unsigned actor_mask = 0;
  for (unsigned i = 0; i < 4; ++i) if (s_gpu.mask & (1u << kGpuPlanes[8+i])) actor_mask |= 1u << i;
  if (!ArGpuBlockMotion_Warp(&s_gpu.blocks, cmd, NativeGpuTexture(s_gpu.previous),
      NativeGpuTexture(s_gpu.current), phase, actor_mask)) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
  if (NativeHandoff(false)) {
    const uint32_t copied = s_gpu.mask;
    if (!UnpackGpuPlanes(cmd, copied)) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return 0;
    for (int i = 0; i < 12; ++i)
      if (copied & (1u << kGpuPlanes[i])) s_planes[kGpuPlanes[i]].generated_padding_valid = true;
    if (prepare_first) ResolveGpuProjection();
    static bool reported_native;
    if (!reported_native) {
      reported_native = true;
      SDL_Log("[gpu-frame-handoff] native %s active; retained padding, one copy pass; projection=%s",
          NativeHandoff(true) ? "pack/unpack" : "unpack", s_gpu.resident ? "GPU" : "CPU");
    }
    return copied & s_gpu.mask;
  }
  if (!SDL_SubmitGPUCommandBuffer(cmd)) return 0;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *target = SDL_GetRenderTarget(renderer);
  Uint8 r = 0, g = 0, b = 0, a = 0;
  if (!SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a)) return 0;
  uint32_t mask = 0;
  for (int i = 0; i < 12; ++i) {
    const int index = kGpuPlanes[i];
    if (!(s_gpu.mask & (1u << index))) continue;
    const DioramaFrameGenerationPlane *plane = &s_planes[index];
    const SDL_FRect src = {0, (float)((i < 8 ? i : i-8) * s_gpu.motion.height),
        (float)plane->texture_width, (float)plane->texture_height};
    const SDL_FRect dst = {(float)plane->output_x, 0, src.w, src.h};
    if (SDL_SetRenderTarget(renderer, plane->generated_texture) &&
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
        SDL_SetRenderViewport(renderer, NULL) && SDL_SetRenderClipRect(renderer, NULL) &&
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0) && SDL_RenderClear(renderer) &&
        SDL_RenderTexture(renderer, i < 8 ? s_gpu.output : s_gpu.block_output, &src, &dst)) mask |= 1u << index;
  }
  const bool restored = SDL_SetRenderTarget(renderer, target);
  const bool color_restored = SDL_SetRenderDrawColor(renderer, r, g, b, a);
  /* GPU kernels already read confidence and return the current endpoint when
   * motion is rejected. Queue their work and destination copies before the
   * CPU waits for projection metadata, then expose only accepted planes. */
  if (prepare_first) ResolveGpuProjection();
  mask &= s_gpu.mask;
  static bool reported;
  if (mask && !reported) {
    reported = true;
    SDL_Log("[gpu-bg-motion] resident analysis/synthesis active; %s",
        GpuOwnsAnalysis() ? "CPU pixel analysis disabled; 256-byte projection metadata download" :
        "CPU projection oracle retained");
  }
  return restored && color_restored ? mask : 0;
}

_Static_assert(
    kFrameSlotLayerTextureWidth <=
        kPresentationFrameGenerationMaximumWidth,
    "capture texture width exceeds frame-generation capacity");
_Static_assert(
    kFrameSlotLayerTextureHeight <=
        kPresentationFrameGenerationMaximumHeight,
    "capture texture height exceeds frame-generation capacity");

static void DestroyPlaneEndpoints(DioramaFrameGenerationPlane *plane) {
  SDL_DestroyTexture(plane->previous_texture);
  SDL_DestroyTexture(plane->current_texture);
  plane->previous_texture = NULL;
  plane->current_texture = NULL;
  plane->generated_padding_valid = false;
  plane->output_x = 0;
  plane->texture_width = 0;
  plane->texture_height = 0;
}

static void DestroyPlaneTextures(DioramaFrameGenerationPlane *plane) {
  DestroyPlaneEndpoints(plane);
  SDL_DestroyTexture(plane->generated_texture);
  plane->generated_texture = NULL;
}

void DioramaFrameGeneration_Reset(void) {
  ResetGpu();
  memset(s_present_offsets,0,sizeof(s_present_offsets));
  for (int plane = 0; plane < kFrameGenerationPlaneCount; plane++) {
    DestroyPlaneTextures(&s_planes[plane]);
    s_planes[plane].current_valid = false;
    s_planes[plane].pair_valid = false;
    memset(&s_planes[plane].motion, 0, sizeof(s_planes[plane].motion));
  }
  memset(&s_last_key, 0, sizeof(s_last_key));
  s_pair_timestamp_ns = 0;
  s_pair_mask = 0;
  s_generated_mask = 0;
  s_last_wait_ns = 0;
  s_index_blocks_x = -1;
  s_index_blocks_y = -1;
}

void DioramaFrameGeneration_Shutdown(void) {
  DioramaFrameGeneration_Reset();
  for (int plane = 0; plane < kFrameGenerationPlaneCount; plane++) {
    free(s_planes[plane].previous_pixels);
    free(s_planes[plane].current_pixels);
    s_planes[plane].previous_pixels = NULL;
    s_planes[plane].current_pixels = NULL;
  }
}

static bool EnsurePlaneBuffers(DioramaFrameGenerationPlane *plane) {
  if (!plane->previous_pixels)
    plane->previous_pixels = malloc(kFrameGenerationTextureBytes);
  if (!plane->current_pixels)
    plane->current_pixels = malloc(kFrameGenerationTextureBytes);
  return plane->previous_pixels && plane->current_pixels;
}

/* Scale mode is passed explicitly rather than inferred from the access type.
 * The endpoints are sampled by the warp and must stay LINEAR even though they
 * are now render targets; deriving it from the access would silently switch
 * them to NEAREST and change every generated frame. */
static SDL_Texture *CreatePlaneTexture(SDL_Renderer *renderer,
                                       SDL_TextureAccess access,
                                       SDL_ScaleMode scale_mode,
                                       int width, int height, int plane, bool generated) {
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, generated ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_ARGB8888,
      access, width, height);
  if (!texture) return NULL;
  if (!SDL_SetTextureScaleMode(texture, scale_mode) ||
      !SDL_SetTextureBlendMode(
          texture, plane == kDioramaPlane_Backdrop
              ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND)) {
    SDL_DestroyTexture(texture);
    return NULL;
  }
  return texture;
}

static bool EnsurePlaneTextures(SDL_Renderer *renderer, int plane_index,
                                const DioramaPlaneCaptureRegion *region) {
  DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
  if (plane->output_x != region->x ||
      plane->texture_width != region->width ||
      plane->texture_height != region->height)
    /* The output has fixed dimensions and can still be the caller's bound
     * target. Only the capture-sized endpoints need replacement here. */
    DestroyPlaneEndpoints(plane);
  /* The endpoints are filled by copying the already-uploaded compositor
   * texture on the GPU rather than by a second upload of the same pixels, so
   * they are render targets. Frame generation already required target
   * textures for `generated_texture`, so this adds no new device requirement
   * and needs no separate fallback. */
  if (!plane->gpu_only && !plane->previous_texture)
    plane->previous_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_TARGET,
                           SDL_SCALEMODE_LINEAR,
                           region->width, region->height, plane_index, false);
  if (!plane->gpu_only && !plane->current_texture)
    plane->current_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_TARGET,
                           SDL_SCALEMODE_LINEAR,
                           region->width, region->height, plane_index, false);
  if (!plane->generated_texture)
    plane->generated_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_TARGET,
                           SDL_SCALEMODE_NEAREST,
                           kFrameSlotLayerTextureWidth,
                           kFrameSlotLayerTextureHeight, plane_index, true);
  const bool ready = plane->generated_texture &&
      (plane->gpu_only || (plane->previous_texture && plane->current_texture));
  if (ready) {
    plane->output_x = region->x;
    plane->texture_width = region->width;
    plane->texture_height = region->height;
  }
  return ready;
}

static bool KeysAreContinuous(const DioramaFrameGenerationKey *previous,
                              const DioramaFrameGenerationKey *current,
                              uint8_t capture_ticks) {
  if (!previous->valid || !capture_ticks ||
      current->timestamp_ns <= previous->timestamp_ns)
    return false;
  if (current->timestamp_ns - previous->timestamp_ns >=
      (uint64_t)kFrameGenerationMaximumPairSpanMs *
          kNanosecondsPerMillisecond)
    return false;
  return previous->width == current->width &&
      previous->height == current->height &&
      previous->bg_mode == current->bg_mode &&
      previous->map_group == current->map_group &&
      previous->map_number == current->map_number &&
      previous->layer_section == current->layer_section &&
      previous->additive_plane_mask == current->additive_plane_mask;
}

static void CopySurfaceRegion(
    uint32_t *destination, const uint8_t *source, size_t source_pitch_bytes,
    const DioramaPlaneCaptureRegion *region) {
  const size_t row_bytes = (size_t)region->width * sizeof(uint32_t);
  const size_t source_x_bytes = (size_t)region->x * sizeof(uint32_t);
  for (int y = 0; y < region->height; y++) {
    memcpy(&destination[(size_t)y * kFrameSlotLayerTextureWidth],
           source + (size_t)y * source_pitch_bytes + source_x_bytes,
           row_bytes);
  }
}

void DioramaFrameGeneration_CaptureWithSkybox(
    ArRenderDevice *device, const FrameSlot *slot,
    const ArRenderTexture layer_textures[kDioramaPlane_Count],
    const uint8_t *const layer_pixels[kDioramaPlane_Count],
    const size_t layer_pitches[kDioramaPlane_Count],
    uint32_t changed_plane_mask, ArRenderTexture skybox_texture,
    bool skybox_changed) {
  DioramaFrameGeneration_FinishCapture();
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  s_pair_timestamp_ns = 0;
  s_pair_mask = 0;
  s_gpu.mask = 0;
  s_gpu.resident = false;
  s_policy = slot ? DioramaGpuPolicy_ForRoom(slot->diorama_map_group, slot->diorama_map_number)
                  : (DioramaGpuPolicy){0};
  if (renderer && slot && slot->diorama_active && !slot->interp_setting_enabled &&
      slot->capture_ticks && !(slot->inidisp & 0x80u)) {
    s_last_key.valid = false;
    s_gpu.resident = EnsureSourceProjection(device);
    if (s_gpu.resident) ++s_gpu.resident_captures;
    return;
  }
  if (!renderer || !slot || !layer_textures || !layer_pixels || !layer_pitches ||
      !slot->diorama_active ||
      !slot->interp_setting_enabled || !slot->capture_ticks ||
      slot->turbo_active || (slot->inidisp & 0x80u) != 0) {
    s_last_key.valid = false;
    return;
  }
  ArRenderTexture source_textures[kFrameGenerationPlaneCount];
  const uint8_t *pixels[kFrameGenerationPlaneCount];
  size_t pitch_bytes[kFrameGenerationPlaneCount];
  memcpy(source_textures, layer_textures, sizeof(*layer_textures) * kDioramaPlane_Count);
  memcpy(pixels, layer_pixels, sizeof(*layer_pixels) * kDioramaPlane_Count);
  memcpy(pitch_bytes, layer_pitches, sizeof(*layer_pitches) * kDioramaPlane_Count);
  const bool have_skybox = ArRenderTexture_IsValid(skybox_texture) &&
      (slot->diorama_skybox_surface.data || (slot->background_packet &&
        (slot->background_packet->owned_sources & 4u))) && !slot->diorama_skybox_periodic;
  source_textures[kDioramaFrameGenerationSkybox] = skybox_texture;
  pixels[kDioramaFrameGenerationSkybox] = have_skybox
      ? slot->diorama_skybox_surface.data : NULL;
  pitch_bytes[kDioramaFrameGenerationSkybox] =
      (size_t)slot->diorama_skybox_surface.pitch_bytes;
  if (skybox_changed) changed_plane_mask |= 1u << kDioramaFrameGenerationSkybox;

  const int width = slot->snes_width + slot->obj_apron * 2;
  const int height = slot->snes_height +
      slot->ws_extra_top + slot->ws_extra_bottom;
  const uint32_t plane_mask = (slot->diorama_plane_request_mask &
      slot->diorama_plane_content_mask) |
      (have_skybox ? 1u << kDioramaFrameGenerationSkybox : 0u);
  DioramaFrameGenerationKey current = {
    .timestamp_ns = slot->timestamp_ns,
    .plane_mask = plane_mask,
    .additive_plane_mask = slot->diorama_plane_additive_mask,
    .width = width,
    .height = height,
    .bg_mode = slot->bg_mode,
    .map_group = slot->diorama_map_group,
    .map_number = slot->diorama_map_number,
    .layer_section = slot->diorama_layer_section,
    .valid = width > 0 &&
        width <= kFrameSlotLayerTextureWidth &&
        width <= kPresentationFrameGenerationMaximumWidth &&
        height > 0 &&
        height <= kFrameSlotLayerTextureHeight &&
        height <= kPresentationFrameGenerationMaximumHeight,
  };
  if (!current.valid) {
    s_last_key.valid = false;
    return;
  }

  uint32_t analyzed_bg_mask = 0;
  const bool own_gpu_analysis = GpuOwnsAnalysis() && EnsureGpu(device, width, height);
  const bool continuous =
      KeysAreContinuous(&s_last_key, &current, slot->capture_ticks);
  /* Endpoint copies bind their own render target. Capture the caller's once
   * and restore it after the loop so no plane's early exit can leave the
   * renderer pointing at a private texture. */
  SDL_Texture *const entry_target = SDL_GetRenderTarget(renderer);
  for (int plane_index = 0; plane_index < kFrameGenerationPlaneCount;
       plane_index++) {
    DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
    plane->pair_valid = false;
    const bool gpu_analysis = own_gpu_analysis && GpuMotionPlane(plane_index);
    if (plane->gpu_only != gpu_analysis) {
      plane->current_valid = false;
      DestroyPlaneEndpoints(plane);
      plane->gpu_only = gpu_analysis;
    }
    const bool gpu_source = slot->background_packet &&
        ((DioramaPlanes_GpuOwnedMask(slot->background_packet) |
          (slot->background_packet->owned_sources & 4u ? 1u << kDioramaFrameGenerationSkybox : 0u)) & (1u << plane_index)) &&
        ArRenderTexture_IsValid(source_textures[plane_index]);
    if (!(plane_mask & (1u << plane_index)) || (!pixels[plane_index] &&
        !(gpu_source && (gpu_analysis || plane_index == kDioramaFrameGenerationSkybox)))) {
      plane->current_valid = false;
      continue;
    }
    DioramaPlaneCaptureRegion region;
    if (plane_index == kDioramaFrameGenerationSkybox) {
      region = (DioramaPlaneCaptureRegion){
        0, (int)slot->diorama_skybox_surface.width_pixels,
        (int)slot->diorama_skybox_surface.height_pixels,
      };
    } else if (!DioramaPlaneCaptureRegion_Resolve(
            plane_index, width, height, slot->obj_apron,
            slot->diorama_bg_apron_mask, &region)) {
      plane->current_valid = false;
      continue;
    }
    if (region.width <= 0 || region.width > kFrameSlotLayerTextureWidth ||
        region.height <= 0 || region.height > kFrameSlotLayerTextureHeight ||
        (!gpu_analysis && pixels[plane_index] && pitch_bytes[plane_index] <
            (size_t)(region.x + region.width) * sizeof(uint32_t))) {
      plane->current_valid = false;
      continue;
    }
    const bool region_matches =
        plane->output_x == region.x &&
        plane->texture_width == region.width &&
        plane->texture_height == region.height;
    /* The synchronized raw texture is already the exact current endpoint.
     * A byte-identical private pair cannot add an intermediate image, so keep
     * the retained endpoint in place and do no CPU copy, private upload, motion
     * search, or synthesis for this plane. Invalid private state still retries
     * even when the raw upload mirror reported no change. */
    if (!(changed_plane_mask & (1u << plane_index)) &&
        plane->current_valid && region_matches)
      continue;
    if (!gpu_analysis && !EnsurePlaneBuffers(plane)) {
      plane->current_valid = false;
      continue;
    }

    if (!gpu_analysis) {
      uint32_t *swap = plane->previous_pixels;
      plane->previous_pixels = plane->current_pixels;
      plane->current_pixels = swap;
    }
    const bool had_previous = plane->current_valid && continuous &&
        region_matches && (s_last_key.plane_mask & (1u << plane_index));
    if (!gpu_analysis) {
      if (pixels[plane_index]) CopySurfaceRegion(
          plane->current_pixels, pixels[plane_index], pitch_bytes[plane_index], &region);
      else for (int y = 0; y < region.height; ++y)
        for (int x = 0; x < region.width; ++x)
          plane->current_pixels[y * kFrameSlotLayerTextureWidth + x] =
              SrPpuBgPacket_Color(slot->background_packet, 2, 0, x, y);
    }
    if (!EnsurePlaneTextures(renderer, plane_index, &region)) {
      plane->current_valid = false;
      continue;
    }

    if (gpu_analysis) {
      /* The atlas owns both GPU endpoints already. Copy the raw source into
       * it directly instead of retaining another pair of identical textures. */
      plane->current_valid = true;
      if (had_previous) {
        analyzed_bg_mask |= 1u << plane_index;
        if (DioramaPlaneIsObjectPriority(plane_index)) {
          plane->pair_valid = true;
          s_pair_mask |= 1u << plane_index;
        }
      }
      continue;
    }

    /* Keep both private endpoints capture-sized. Linear warping can then
     * sample their physical texture edge without touching the stale padding
     * carried by the fixed-size compositor textures. Swapping means only one
     * endpoint is refreshed per captured plane, not two. */
    SDL_Texture *texture_swap = plane->previous_texture;
    plane->previous_texture = plane->current_texture;
    plane->current_texture = texture_swap;
    /* Diorama_Upload has already sent exactly these pixels to the compositor
     * texture earlier in this same presentation. Uploading them a second time
     * duplicated the whole transfer -- and on backends with a large per-call
     * cost the duplicate was more expensive than the bytes. Copy the valid
     * region on the GPU instead. Copying only the region is also what made the
     * private endpoint necessary in the first place: it leaves the compositor
     * texture's fixed-size padding behind. */
    SDL_Texture *source =
        ArSdlRenderBackend_UnwrapTexture(source_textures[plane_index]);
    if (!source) {
      plane->current_valid = false;
      continue;
    }
    /* An exact copy, not a composite: the source's own blend mode would
     * otherwise alpha-blend these texels over an undefined target. */
    SDL_BlendMode source_blend = SDL_BLENDMODE_NONE;
    const bool had_blend = SDL_GetTextureBlendMode(source, &source_blend);
    const SDL_FRect source_rect = {
      (float)region.x, 0.0f, (float)region.width, (float)region.height,
    };
    const SDL_FRect destination_rect = {
      0.0f, 0.0f, (float)region.width, (float)region.height,
    };
    const bool copied =
        SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) &&
        SDL_SetRenderTarget(renderer, plane->current_texture) &&
        SDL_RenderTexture(renderer, source, &source_rect, &destination_rect);
    if (had_blend) (void)SDL_SetTextureBlendMode(source, source_blend);
    if (!copied) {
      plane->current_valid = false;
      continue;
    }
    plane->current_valid = true;
    if (!had_previous) continue;
    const PresentationFrameGenerationAnalysisMode mode =
        DioramaPlaneIsObjectPriority(plane_index)
            ? kPresentationFrameGenerationAnalysis_Blocks
            : kPresentationFrameGenerationAnalysis_Global;
    if (GpuMotionPlane(plane_index))
      analyzed_bg_mask |= 1u << plane_index;
    plane->pair_valid = PresentationFrameGeneration_Analyze(
        plane->previous_pixels, plane->current_pixels,
        region.width, region.height,
        kFrameSlotLayerTextureWidth, kFrameSlotLayerTextureWidth,
        mode, &plane->motion);
    if (plane->pair_valid) s_pair_mask |= 1u << plane_index;
  }
  if (SDL_GetRenderTarget(renderer) != entry_target)
    (void)SDL_SetRenderTarget(renderer, entry_target);
  CaptureGpu(device, width, height, continuous, analyzed_bg_mask, source_textures);
  s_pair_timestamp_ns = s_pair_mask ? slot->timestamp_ns : 0;
  s_last_key = current;
}

static bool EnsureBlockIndices(int blocks_x, int blocks_y) {
  if (blocks_x == s_index_blocks_x && blocks_y == s_index_blocks_y)
    return true;
  const int index_count = blocks_x * blocks_y * 6;
  if (blocks_x <= 0 || blocks_y <= 0 ||
      index_count > kFrameGenerationMaximumIndices)
    return false;
  const int columns = blocks_x + 1;
  int index = 0;
  for (int row = 0; row < blocks_y; row++) {
    for (int column = 0; column < blocks_x; column++) {
      const int top_left = row * columns + column;
      s_indices[index++] = top_left;
      s_indices[index++] = top_left + 1;
      s_indices[index++] = top_left + columns;
      s_indices[index++] = top_left + 1;
      s_indices[index++] = top_left + columns + 1;
      s_indices[index++] = top_left + columns;
    }
  }
  s_index_blocks_x = blocks_x;
  s_index_blocks_y = blocks_y;
  return true;
}

static bool BuildMesh(
    const PresentationFrameGenerationMotionField *motion,
    int output_x, bool forward, float amount, int *out_vertices,
    const int **out_indices, int *out_index_count) {
  const int columns = motion->uniform ? 2 : motion->blocks_x + 1;
  const int rows = motion->uniform ? 2 : motion->blocks_y + 1;
  const int vertex_count = columns * rows;
  if (vertex_count > kFrameGenerationMaximumVertices)
    return false;

  int vertex = 0;
  for (int row = 0; row < rows; row++) {
    const int source_y = motion->uniform
        ? row * motion->height
        : row == motion->blocks_y
            ? motion->height
            : row * kPresentationFrameGenerationBlockSize;
    for (int column = 0; column < columns; column++) {
      const int source_x = motion->uniform
          ? column * motion->width
          : column == motion->blocks_x
              ? motion->width
              : column * kPresentationFrameGenerationBlockSize;
      float dx = 0.0f, dy = 0.0f;
      PresentationFrameGeneration_MotionAt(
          motion, forward,
          source_x < motion->width ? source_x : motion->width - 1,
          source_y < motion->height ? source_y : motion->height - 1,
          &dx, &dy);
      s_vertices[vertex++] = (SDL_Vertex){
        .position = {
          (float)(output_x + source_x) + dx * amount,
          (float)source_y + dy * amount,
        },
        .color = {1.0f, 1.0f, 1.0f, 1.0f},
        .tex_coord = {
          (float)source_x / (float)motion->width,
          (float)source_y / (float)motion->height,
        },
      };
    }
  }
  if (motion->uniform) {
    *out_indices = kQuadIndices;
    *out_index_count = (int)(sizeof(kQuadIndices) / sizeof(kQuadIndices[0]));
  } else {
    if (!EnsureBlockIndices(motion->blocks_x, motion->blocks_y)) return false;
    *out_indices = s_indices;
    *out_index_count = motion->blocks_x * motion->blocks_y * 6;
  }
  *out_vertices = vertex_count;
  return true;
}

static bool DrawEndpoint(SDL_Renderer *renderer, SDL_Texture *texture,
                         const PresentationFrameGenerationMotionField *motion,
                         int output_x, bool forward, float amount) {
  int vertex_count = 0, index_count = 0;
  const int *indices = NULL;
  if (!BuildMesh(
          motion, output_x, forward, amount,
          &vertex_count, &indices, &index_count))
    return false;
  SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
  SDL_ScaleMode old_scale = SDL_SCALEMODE_NEAREST;
  if (!SDL_GetTextureBlendMode(texture, &old_blend) ||
      !SDL_GetTextureScaleMode(texture, &old_scale))
    return false;
  /* SDL_RenderGeometry takes alpha from SDL_Vertex.color and explicitly
   * ignores texture alpha modulation. Requiring SetTextureAlphaModFloat here
   * would reject otherwise-capable backends where that optional texture state
   * is unsupported, without changing a single generated pixel. */
  const bool configured =
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  const bool drawn = configured && SDL_RenderGeometry(
      renderer, texture, s_vertices, vertex_count,
      indices, index_count);
  /* Cleanup is deliberately non-short-circuiting. A backend failure restoring
   * one property must not prevent attempts to restore every other property. */
  bool restored = true;
  if (!SDL_SetTextureScaleMode(texture, old_scale)) restored = false;
  if (!SDL_SetTextureBlendMode(texture, old_blend)) restored = false;
  return drawn && restored;
}

static bool GeneratePlane(
    SDL_Renderer *renderer, int plane_index,
    SDL_Texture *old_target, float phase) {
  DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
  if (!plane->pair_valid || !plane->previous_texture ||
      !plane->current_texture || !plane->generated_texture)
    return false;

  if (!SDL_SetRenderTarget(renderer, plane->generated_texture)) return false;
  /* Logical presentation, viewport, and clip state are target-specific in
   * SDL. Disabling them on the old window/scene target before this switch does
   * not configure a newly selected private target; inheriting its creation-time
   * logical transform scales and clips native plane coordinates into garbage.
   * These generated targets are internal-only, so pin their state explicitly. */
  const bool configured = SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
      SDL_SetRenderViewport(renderer, NULL) &&
      SDL_SetRenderClipRect(renderer, NULL) &&
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
  /* Render only the nearer endpoint. With a trustworthy bidirectional field,
   * both warps meet at the same intermediate position; drawing the farther
   * endpoint again cannot add motion information. It does keep old transparent
   * sprite pixels alive and cross-fades animation poses, which looks like
   * latched input/ghosting on pixel art. Nearest-endpoint ownership preserves
   * the selected capture's exact alpha and changes ownership only at midpoint. */
  bool generated = configured && SDL_RenderClear(renderer);
  if (generated && phase < 0.5f) {
    generated = DrawEndpoint(
        renderer, plane->previous_texture, &plane->motion,
        plane->output_x, true, phase);
  } else if (generated) {
    generated = DrawEndpoint(
        renderer, plane->current_texture, &plane->motion,
        plane->output_x, false, 1.0f - phase);
  }
  return SDL_SetRenderTarget(renderer, old_target) && generated;
}

ArRenderPointF DioramaFrameGeneration_PlaneOffset(int plane) {
  if (plane < 0 || plane >= kFrameGenerationPlaneCount) return (ArRenderPointF){0,0};
  return s_present_offsets[plane];
}

uint32_t DioramaFrameGeneration_PrepareWithSkybox(
    ArRenderDevice *device, const FrameSlot *slot, float alpha,
    const ArRenderTexture current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    ArRenderTexture resolved_textures[kDioramaPlane_Count],
    ArRenderTexture skybox_texture, ArRenderTexture *resolved_skybox) {
  DioramaFrameGeneration_FinishCapture();
  memset(s_present_offsets,0,sizeof(s_present_offsets));
  s_gpu.presented_mask = 0;
  s_gpu.phase = 1;
  s_generated_mask = 0;
  s_last_wait_ns = 0;
  if (resolved_skybox) *resolved_skybox = skybox_texture;
  if (!resolved_textures || !current_textures) return 0;
  memcpy(resolved_textures, current_textures,
         sizeof(ArRenderTexture) * kDioramaPlane_Count);
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *native_current_textures[kFrameGenerationPlaneCount];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    native_current_textures[plane] =
        ArSdlRenderBackend_UnwrapTexture(current_textures[plane]);
  native_current_textures[kDioramaFrameGenerationSkybox] =
      ArSdlRenderBackend_UnwrapTexture(skybox_texture);
  current_plane_mask &= (1u << kDioramaPlane_Count) - 1u;
  if (resolved_skybox && ArRenderTexture_IsValid(skybox_texture) &&
      slot && !slot->diorama_skybox_periodic)
    current_plane_mask |= 1u << kDioramaFrameGenerationSkybox;
  if (!renderer || !slot || !slot->diorama_active ||
      !slot->interp_setting_enabled ||
      alpha < 0.0f ||
      s_pair_timestamp_ns != slot->timestamp_ns || !s_pair_mask)
    return 0;

  const float phase = PresentationFrameGeneration_PairPhase(
      alpha, slot->capture_ticks);
  ArSdlPresentationOutputState output_state;
  if (!ArSdlPresentation_PushFullOutput(renderer, &output_state)) return 0;
  SDL_Texture *old_target = SDL_GetRenderTarget(renderer);
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  if (!SDL_GetRenderDrawColor(renderer, &old_r, &old_g, &old_b, &old_a)) {
    ArSdlPresentation_PopFullOutput(renderer, &output_state);
    return 0;
  }
  const uint32_t gpu_mask = PrepareGpu(device, phase);
  s_gpu.phase = phase;
  s_gpu.presented_mask = gpu_mask;
  uint32_t generated_mask = 0;
  for (int plane = 0; plane < kFrameGenerationPlaneCount; plane++) {
    if (!((s_pair_mask & current_plane_mask) & (1u << plane)) ||
        !native_current_textures[plane])
      continue;
    const bool generated = (gpu_mask & (1u << plane)) || (!s_planes[plane].gpu_only && GeneratePlane(
        renderer, plane, old_target, phase));
    if (SDL_GetRenderTarget(renderer) != old_target) {
      (void)SDL_SetRenderTarget(renderer, old_target);
      break;
    }
    if (generated) {
      ArRenderTexture generated_texture = ArSdlRenderBackend_BorrowTexture(
          s_planes[plane].generated_texture);
      if (plane == kDioramaFrameGenerationSkybox)
        *resolved_skybox = generated_texture;
      else
        resolved_textures[plane] = generated_texture;
      generated_mask |= 1u << plane;
      if (!s_gpu.resident && !DioramaPlaneIsObjectPriority(plane) &&
          s_planes[plane].motion.uniform) {
        float dx, dy;
        PresentationFrameGeneration_MotionAt(&s_planes[plane].motion,false,0,0,&dx,&dy);
        ArRenderPointF offset = {dx*(1-phase),dy*(1-phase)};
        if (phase < .5f) {
          float forward_x, forward_y;
          PresentationFrameGeneration_MotionAt(
              &s_planes[plane].motion,true,0,0,&forward_x,&forward_y);
          offset = (ArRenderPointF){dx+forward_x*phase,dy+forward_y*phase};
        }
        s_present_offsets[plane] = offset;
      }
    }
  }
  SDL_SetRenderDrawColor(renderer, old_r, old_g, old_b, old_a);
  ArSdlPresentation_PopFullOutput(renderer, &output_state);
  s_generated_mask = generated_mask;
  return generated_mask;
}

void DioramaFrameGeneration_Capture(
    ArRenderDevice *device, const FrameSlot *slot,
    const ArRenderTexture textures[kDioramaPlane_Count],
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitches[kDioramaPlane_Count], uint32_t changed) {
  DioramaFrameGeneration_CaptureWithSkybox(
      device, slot, textures, pixels, pitches, changed,
      ArRenderTexture_Invalid(), false);
}

uint32_t DioramaFrameGeneration_Prepare(
    ArRenderDevice *device, const FrameSlot *slot, float alpha,
    const ArRenderTexture textures[kDioramaPlane_Count], uint32_t mask,
    ArRenderTexture resolved[kDioramaPlane_Count]) {
  return DioramaFrameGeneration_PrepareWithSkybox(
      device, slot, alpha, textures, mask, resolved,
      ArRenderTexture_Invalid(), NULL);
}

/* Motion confidence stays in the storage buffer. CPU masks identify candidates;
 * both the warp and projection shaders choose zero motion on rejection. */
static uint32_t SourceMotionSlots(void) {
  uint32_t slots = 0;
  for (unsigned i = 0; i < 8; ++i)
    if (s_gpu.presented_mask & (1u << kGpuPlanes[i])) slots |= 1u << i;
  return slots;
}

bool DioramaFrameGeneration_DrawSource(ArRenderDevice *device, const ActionEffectSourceBatch *batch,
    const DioramaProjection *view, const ActionMoonlightOcclusion *scenery,
    ArRenderBlendMode blend, float brightness) {
  if (!s_gpu.resident || s_gpu.effects_failed) return false;
  const bool ok = ArGpuEffectSource_Draw(&s_gpu.effects, device, SourceMotion(),
      SourceMotionSlots(), s_gpu.phase, batch, view, scenery, blend, brightness);
  if (ok && batch->count) { ++s_gpu.source_draws; s_gpu.source_primitives += batch->count; }
  if (!ok) s_gpu.effects_failed = true;
  return ok;
}

bool DioramaFrameGeneration_DrawSkybox(ArRenderDevice *device, ArRenderTexture texture,
    const DioramaSkyboxSourceDraw *draw) {
  if (!s_gpu.resident || s_gpu.effects_failed) return false;
  const unsigned plane = draw->motion_slot == 6 ? kDioramaFrameGenerationSkybox :
      draw->motion_slot == 0 ? SR_PPU_OVERLAY_BG1 : SR_PPU_OVERLAY_BG2;
  const int slot = draw->motion_slot >= 0 && (s_gpu.presented_mask & (1u << plane))
      ? draw->motion_slot : -1;
  const bool ok = ArGpuEffectSource_Skybox(&s_gpu.effects, device, SourceMotion(), slot,
      s_gpu.phase, texture, draw);
  if (!ok) s_gpu.effects_failed = true;
  return ok;
}

/* Exceptional recovery only: fetch this pair's motion, then redraw from the
 * untouched source textures. Normal resident presentation never downloads.
 * Latch the failure until reset to avoid alternating paths or allocation loops. */
void DioramaFrameGeneration_RecoverSourceProjection(ArRenderDevice *device) {
  if (!s_gpu.resident) return;
  DioramaFrameGeneration_FinishCapture();
  s_gpu.resident = false;
  s_gpu.effects_disabled = true;
  s_gpu.effects_failed = false;
  bool recovered = s_gpu.mask == 0;
  if (!recovered && ArSdlRenderBackend_SubmitPending(device)) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_gpu.motion.device);
    recovered = cmd && QueueGpuResults(cmd, false) && ApplyGpuResults(s_gpu.pending_analysis_mask);
  }
  if (recovered) s_gpu.mask &= s_pair_mask;
  else {
    s_gpu.mask = s_pair_mask = 0;
    s_gpu.endpoint = false;
    s_gpu.failed = true;
  }
  SDL_Log("[gpu-effect-projection] reference recovery; resident projection disabled until renderer reset");
}
