#include "platform/sdl/diorama_depth_alpha_util.h"
#include "support/test_assert.h"
#include <stdio.h>

int main(void) {
  assert(DioramaDepthAlpha_SelectOrderFormat(true)==SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
  assert(DioramaDepthAlpha_SelectOrderFormat(false)==SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT);
  assert(!DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_LOAD,false));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_LOAD,true));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_CLEAR,false));
  assert(DioramaDepthAlpha_NeedsInitialPass(SDL_GPU_LOADOP_CLEAR,true));
  printf("PASS: order format fallback and initial-pass policy\n");
}
