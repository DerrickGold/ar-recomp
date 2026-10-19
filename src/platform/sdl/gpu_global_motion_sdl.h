#ifndef AR_GPU_GLOBAL_MOTION_SDL_H
#define AR_GPU_GLOBAL_MOTION_SDL_H
#include <SDL3/SDL.h>

/* Up to eight global-motion planes in an atlas. Pixels stay GPU-resident.
 * The live compositor currently downloads the small vector result for CPU
 * effect/skybox projection; probes and validation may also read it. */
typedef struct ArGpuGlobalMotion {
  SDL_GPUDevice *device;
  SDL_GPUComputePipeline *bound, *cost, *refine, *validate, *warp, *warp_layers;
  SDL_GPUBuffer *bounds, *costs, *directions, *motion;
  SDL_GPUSampler *sampler;
  SDL_GPUTexture *output;
  unsigned width, height, plane_count, configured_plane_count, input_plane_count;
  /* x: width, y: analyze this pair (0 skips unchanged/absent bands).
   * All bands share atlas height. Skipped bands select the current endpoint. */
  float extents[8][4];
} ArGpuGlobalMotion;
typedef struct ArGpuGlobalMotionResult {
  int32_t forward_x, forward_y, backward_x, backward_y;
  int32_t valid, reserved[3];
} ArGpuGlobalMotionResult;
_Static_assert(sizeof(ArGpuGlobalMotionResult) == 32, "GPU motion result layout");

bool ArGpuGlobalMotion_Init(ArGpuGlobalMotion *p, SDL_GPUDevice *device);
void ArGpuGlobalMotion_Destroy(ArGpuGlobalMotion *p); /* Caller must retire in-flight work. */
bool ArGpuGlobalMotion_Resize(ArGpuGlobalMotion *p, unsigned width, unsigned height);
/* Resize prepares analysis dimensions; the atlas is allocated only for the
 * reference/fallback warp. Direct layer output never needs this allocation. */
bool ArGpuGlobalMotion_EnsureOutput(ArGpuGlobalMotion *p);
bool ArGpuGlobalMotion_Analyze(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *previous, SDL_GPUTexture *current);
bool ArGpuGlobalMotion_Warp(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, float phase);

/* Direct compositor output. All eight bindings must be distinct RGBA8
 * SAMPLER | COLOR_TARGET | COMPUTE_STORAGE_WRITE textures. Inactive bindings
 * are not written. Active regions have extents[i].x/z pixels at (x, 0).
 * Padding is preserved after its initial clear, so these targets never cycle. */
typedef struct ArGpuGlobalMotionTarget {
  SDL_GPUTexture *texture;
  unsigned width, height, x;
  bool clear_padding;
} ArGpuGlobalMotionTarget;
bool ArGpuGlobalMotion_PrepareLayerWarp(ArGpuGlobalMotion *p);
bool ArGpuGlobalMotion_WarpLayers(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, float phase,
    const ArGpuGlobalMotionTarget targets[8], unsigned mask);

#endif
