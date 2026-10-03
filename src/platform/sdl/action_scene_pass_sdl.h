#ifndef AR_ACTION_SCENE_PASS_SDL_H
#define AR_ACTION_SCENE_PASS_SDL_H

#include <SDL3/SDL.h>
#include "diorama/diorama_scene_pass.h"

typedef struct ArGpuActionSceneTexture {
  SDL_GPUTexture *texture;
  unsigned width, height;
  ArRenderRectF source; /* Pixel rectangle within a resident texture/atlas. */
} ArGpuActionSceneTexture;

typedef struct ArGpuActionScenePass {
  SDL_GPUDevice *device;
  SDL_GPUGraphicsPipeline *pipelines[3]; /* opaque, alpha, additive */
  SDL_GPUSampler *sampler, *nearest_sampler;
  SDL_GPUBuffer *vertices, *indices;
  SDL_GPUTransferBuffer *upload;
  unsigned vertex_capacity, index_capacity;
} ArGpuActionScenePass;

/* No renderer, swapchain, endpoint copy or fence is owned here. The caller
 * records analysis/warp first, Encode next, then its final presentation in
 * submission order. Motion must support GRAPHICS_STORAGE_READ, including
 * when every draw has motion_slot=-1 (a valid binding is still required).
 * The target is a single-sample R8G8B8A8_UNORM texture. Bindings and coordinates
 * must describe the same frame pair; the caller owns epoch/reset validation.
 * One bounded upload and one ordered render pass cover the whole draw list.
 * A pass instance has one CPU owner at a time: Encode cycles/maps its upload
 * storage and may grow buffers. Offscreen work may run on a worker, which must
 * acquire and submit its own command buffer. Publish submission before a
 * different thread records/submits dependent consumers; join before Destroy.
 * On failure cancel the caller's command buffer; do not submit partial work. */
bool ArGpuActionScenePass_Init(ArGpuActionScenePass *, SDL_GPUDevice *);
void ArGpuActionScenePass_Destroy(ArGpuActionScenePass *);
bool ArGpuActionScenePass_Encode(ArGpuActionScenePass *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *target, unsigned width, unsigned height,
    SDL_GPUBuffer *motion, float phase,
    const DioramaSceneDraw *, const ArGpuActionSceneTexture *, unsigned count,
    SDL_GPULoadOp load, ArRenderColorF clear);

#endif
