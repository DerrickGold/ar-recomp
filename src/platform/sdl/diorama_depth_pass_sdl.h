#ifndef AR_DIORAMA_DEPTH_PASS_SDL_H
#define AR_DIORAMA_DEPTH_PASS_SDL_H

#include <SDL3/SDL.h>
#include "diorama/diorama_depth_pass.h"

typedef struct ArGpuDioramaDepthPass {
  SDL_GPUDevice *device;
  /* Basic, scenery, actor; depth mode and blend follow. */
  SDL_GPUGraphicsPipeline *pipelines[3][kDioramaDepth_Count][3];
  SDL_GPUTextureFormat color_format;
  SDL_GPUSampler *sampler, *nearest_sampler;
  SDL_GPUTexture *depth, *white, *black, *neutral_attributes;
  SDL_GPUBuffer *vertices, *indices;
  SDL_GPUTransferBuffer *upload;
  unsigned width, height, vertex_capacity, index_capacity;
  bool depth_initialized;
  struct ArGpuDioramaDepthAlpha *alpha;
} ArGpuDioramaDepthPass;

typedef struct ArGpuDioramaDepthMaterial {
  const struct DioramaSceneryLightEffectParams *scenery;
  SDL_GPUTexture *illumination; /* NULL supplies zero incoming light. */
  SDL_GPUTexture *attributes, *enemy_illumination;
  bool actor; /* Native receiver/rim material, independent of depth policy. */
} ArGpuDioramaDepthMaterial;

/* Cutout world materials, independent of SIM despite reusing its generic
 * compiled vertex and basic texture/color shaders. Scenery materials reuse
 * the ordinary illumination, DOF, grading, heat and priority-pair math, with
 * discard based on final filtered coverage. Native actors retain receiver
 * attributes, separate player/enemy light, rim light and DOF. Their explicit
 * NativeActor mode preserves native priority; modeled props use world depth.
 * Fractional alpha remains cutout coverage, not transparency sorting.
 * No shader compiler is needed at build/runtime.
 *
 * The caller owns command buffers, submissions and source texture lifetimes.
 * A whole batch uses one upload and render pass. Depth persists across batches
 * until clear_depth, resize or Destroy; clear it at every frame/scene boundary.
 * Resizing requires clear_depth. No implicit readback, fence or window present.
 * Textures are single-sample. Init uses an RGBA8 target; InitFormat also accepts
 * BGRA8. Scenery/actor UVs retain clamp-to-edge sampling outside [0,1]; the basic
 * shader reserves negative U for untextured geometry. NULL sources are allowed
 * only for untextured draws. On failure cancel the caller's command buffer and
 * clear depth before retrying. One thread owns an instance at a time. */
bool ArGpuDioramaDepthPass_Init(ArGpuDioramaDepthPass *, SDL_GPUDevice *);
bool ArGpuDioramaDepthPass_InitFormat(ArGpuDioramaDepthPass *, SDL_GPUDevice *, SDL_GPUTextureFormat);
void ArGpuDioramaDepthPass_Destroy(ArGpuDioramaDepthPass *);
/* Optional preparation outside gameplay: build every material pipeline and
 * reserve geometry/target storage. Idempotent within the retained capacities.
 * New/resized depth still requires clear_depth on the next Encode. Existing
 * depth is preserved when dimensions match. Failure remains retryable; the
 * caller must not encode concurrently with preparation on this instance. */
bool ArGpuDioramaDepthPass_Prepare(ArGpuDioramaDepthPass *, unsigned width, unsigned height,
    unsigned vertices, unsigned indices);
bool ArGpuDioramaDepthPass_Encode(ArGpuDioramaDepthPass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *, SDL_GPUTexture *const *sources, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth);

/* Optional per-draw scenery bindings. NULL array/entry.scenery selects the
 * basic material. Borrowed resources and parameters must outlive Encode only;
 * GPU source textures must remain valid until the commands finish. */
bool ArGpuDioramaDepthPass_EncodeMaterials(ArGpuDioramaDepthPass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *, SDL_GPUTexture *const *sources,
    const ArGpuDioramaDepthMaterial *, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth);

#endif
