#ifndef AR_GPU_BLOCK_MOTION_SDL_H
#define AR_GPU_BLOCK_MOTION_SDL_H
#include <SDL3/SDL.h>

/* Four OBJ priorities. Analysis, neighbour seeding, confidence, motion halos
 * and the forward-warp mesh stay on the GPU. No gameplay readback is needed. */
typedef struct ArGpuBlockMotion {
  SDL_GPUDevice *device;
  SDL_GPUComputePipeline *cost, *search, *validate, *build_vertices;
  SDL_GPUGraphicsPipeline *warp;
  SDL_GPUBuffer *costs, *directions, *motion, *vertices, *indices;
  SDL_GPUSampler *sampler;
  SDL_GPUTexture *output, *attributes; /* Same winner/warp, companion material data. */
  unsigned width, height;
  bool stationary_reject; /* Opt-in exact confidence-bound shortcut. */
  float extents[4][4]; /* width, height, source atlas slot, analyze flag */
} ArGpuBlockMotion;

bool ArGpuBlockMotion_Init(ArGpuBlockMotion *, SDL_GPUDevice *);
void ArGpuBlockMotion_Destroy(ArGpuBlockMotion *);
bool ArGpuBlockMotion_Resize(ArGpuBlockMotion *, unsigned width, unsigned height);
/* Allocate atlas color/material output only for reference/fallback synthesis. */
bool ArGpuBlockMotion_EnsureOutput(ArGpuBlockMotion *);
bool ArGpuBlockMotion_Analyze(ArGpuBlockMotion *, SDL_GPUCommandBuffer *, SDL_GPUTexture *previous,
                              SDL_GPUTexture *current);
bool ArGpuBlockMotion_Warp(ArGpuBlockMotion *, SDL_GPUCommandBuffer *, SDL_GPUTexture *previous,
                           SDL_GPUTexture *current, float phase, unsigned mask);
/* Final layer targets: color and companion metadata use the same capture
 * rectangle at (x, 0). The whole target is cleared each warp, including padding. */
typedef struct ArGpuBlockMotionTarget {
  SDL_GPUTexture *color, *attributes;
  unsigned width, height, x;
} ArGpuBlockMotionTarget;
bool ArGpuBlockMotion_WarpLayers(ArGpuBlockMotion *, SDL_GPUCommandBuffer *,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, float phase, unsigned mask,
    const ArGpuBlockMotionTarget targets[4]);
#endif
