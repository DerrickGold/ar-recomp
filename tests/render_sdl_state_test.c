#include "platform/sdl/render_sdl.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool NearlyEqual(float left, float right) {
  return fabsf(left - right) < 0.001f;
}

static void AssertTextureState(SDL_Texture *texture,
                               float r, float g, float b, float a,
                               SDL_BlendMode blend) {
  float actual_r = 0.0f;
  float actual_g = 0.0f;
  float actual_b = 0.0f;
  float actual_a = 0.0f;
  SDL_BlendMode actual_blend = SDL_BLENDMODE_INVALID;
  assert(SDL_GetTextureColorModFloat(
      texture, &actual_r, &actual_g, &actual_b));
  assert(SDL_GetTextureAlphaModFloat(texture, &actual_a));
  assert(SDL_GetTextureBlendMode(texture, &actual_blend));
  assert(NearlyEqual(actual_r, r));
  assert(NearlyEqual(actual_g, g));
  assert(NearlyEqual(actual_b, b));
  assert(NearlyEqual(actual_a, a));
  assert(actual_blend == blend);
}

int main(void) {
  SDL_Surface *surface = SDL_CreateSurface(
      16, 16, SDL_PIXELFORMAT_ARGB8888);
  assert(surface);
  SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
  assert(renderer);

  ArSdlRenderBackend backend = {0};
  ArRenderDevice device = {0};
  assert(ArSdlRenderBackend_Bind(&device, &backend, renderer));

  const ArRenderTextureDesc desc = {
    .width = 2,
    .height = 2,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Static,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture texture = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(&device, &desc, &texture));
  const uint32_t pixels[4] = {
    UINT32_C(0xffffffff), UINT32_C(0xffffffff),
    UINT32_C(0xffffffff), UINT32_C(0xffffffff),
  };
  assert(ArRenderDevice_UpdateTexture(
      &device, texture, NULL, pixels, 2 * (int)sizeof(uint32_t)));

  SDL_Texture *native = ArSdlRenderBackend_UnwrapTexture(texture);
  assert(SDL_SetTextureColorModFloat(native, 0.8f, 0.7f, 0.6f));
  assert(SDL_SetTextureAlphaModFloat(native, 0.5f));
  assert(SDL_SetTextureBlendMode(native, SDL_BLENDMODE_BLEND));

  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Tint | kArRenderDrawState_Blend,
    .tint = {0.25f, 0.5f, 0.75f, 1.0f},
    .blend = kArRenderBlendMode_Add,
  };
  const ArRenderRectF destination = {0.0f, 0.0f, 8.0f, 8.0f};
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, NULL, &destination, &state));
  AssertTextureState(
      native, 0.8f, 0.7f, 0.6f, 0.5f, SDL_BLENDMODE_BLEND);

  const ArRenderVertex2D vertices[3] = {
    {{0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
    {{8.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    {{0.0f, 8.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},
  };
  const int32_t indices[3] = {0, 1, 2};
  const ArRenderDrawState geometry_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_Add,
  };
  assert(ArRenderDevice_DrawGeometryWithState(
      &device, texture, vertices, 3, indices, 3, &geometry_state));
  AssertTextureState(
      native, 0.8f, 0.7f, 0.6f, 0.5f, SDL_BLENDMODE_BLEND);

  assert(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
  assert(ArRenderDevice_DrawGeometryWithState(
      &device, ArRenderTexture_Invalid(),
      vertices, 3, indices, 3, &geometry_state));
  SDL_BlendMode renderer_blend = SDL_BLENDMODE_INVALID;
  assert(SDL_GetRenderDrawBlendMode(renderer, &renderer_blend));
  assert(renderer_blend == SDL_BLENDMODE_BLEND);

  ArRenderDevice_DestroyTexture(&device, texture);
  ArRenderDevice_Reset(&device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  puts("render_sdl_state_test: ok");
  return 0;
}
