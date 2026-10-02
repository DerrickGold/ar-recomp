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
  SDL_GPUTexture *output;
  unsigned width, height;
  float extents[4][4]; /* width, height, source atlas slot, analyze flag */
} ArGpuBlockMotion;

bool ArGpuBlockMotion_Init(ArGpuBlockMotion *, SDL_GPUDevice *);
void ArGpuBlockMotion_Destroy(ArGpuBlockMotion *);
bool ArGpuBlockMotion_Resize(ArGpuBlockMotion *, unsigned width, unsigned height);
bool ArGpuBlockMotion_Analyze(ArGpuBlockMotion *, SDL_GPUCommandBuffer *, SDL_GPUTexture *previous,
                              SDL_GPUTexture *current);
bool ArGpuBlockMotion_Warp(ArGpuBlockMotion *, SDL_GPUCommandBuffer *, SDL_GPUTexture *previous,
                           SDL_GPUTexture *current, float phase, unsigned mask);
#endif
