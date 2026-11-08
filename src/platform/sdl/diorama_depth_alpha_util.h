#ifndef AR_DIORAMA_DEPTH_ALPHA_UTIL_H
#define AR_DIORAMA_DEPTH_ALPHA_UTIL_H

#include <SDL3/SDL_gpu.h>
#include <stdbool.h>

/* The shaders read only order.r. The fallback keeps the original 32-bit float
 * precision and negative sentinel value. */
static inline SDL_GPUTextureFormat DioramaDepthAlpha_SelectOrderFormat(bool supports_r32) {
  return supports_r32 ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
}

static inline bool DioramaDepthAlpha_NeedsInitialPass(SDL_GPULoadOp color_load, bool clear_depth) {
  return color_load != SDL_GPU_LOADOP_LOAD || clear_depth;
}

#endif
