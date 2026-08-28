#include "platform/sdl/render_sdl.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
  assert(SDL_SetRenderLogicalPresentation(
      renderer, 8, 8, SDL_LOGICAL_PRESENTATION_LETTERBOX));
  assert(ArRenderDevice_UseOutputCoordinates(&device));
  int output_width = 0;
  int output_height = 0;
  assert(ArRenderDevice_GetOutputSize(
      &device, &output_width, &output_height));
  assert(output_width == 16 && output_height == 16);
  int logical_width = -1;
  int logical_height = -1;
  SDL_RendererLogicalPresentation logical_mode =
      SDL_LOGICAL_PRESENTATION_LETTERBOX;
  assert(SDL_GetRenderLogicalPresentation(
      renderer, &logical_width, &logical_height, &logical_mode));
  assert(logical_width == 0 && logical_height == 0);
  assert(logical_mode == SDL_LOGICAL_PRESENTATION_DISABLED);

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

  assert(SDL_SetRenderTextureAddressMode(
      renderer, SDL_TEXTURE_ADDRESS_AUTO, SDL_TEXTURE_ADDRESS_AUTO));
  const ArRenderDrawState address_state = {
    .flags = kArRenderDrawState_Address,
    .address_u = kArRenderTextureAddressMode_Wrap,
    .address_v = kArRenderTextureAddressMode_Clamp,
  };
  assert(ArRenderDevice_DrawGeometryWithState(
      &device, texture, vertices, 3, indices, 3, &address_state));
  SDL_TextureAddressMode address_u = SDL_TEXTURE_ADDRESS_INVALID;
  SDL_TextureAddressMode address_v = SDL_TEXTURE_ADDRESS_INVALID;
  assert(SDL_GetRenderTextureAddressMode(
      renderer, &address_u, &address_v));
  assert(address_u == SDL_TEXTURE_ADDRESS_AUTO);
  assert(address_v == SDL_TEXTURE_ADDRESS_AUTO);

  assert(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
  assert(ArRenderDevice_DrawGeometryWithState(
      &device, ArRenderTexture_Invalid(),
      vertices, 3, indices, 3, &geometry_state));
  SDL_BlendMode renderer_blend = SDL_BLENDMODE_INVALID;
  assert(SDL_GetRenderDrawBlendMode(renderer, &renderer_blend));
  assert(renderer_blend == SDL_BLENDMODE_BLEND);

  const ArRenderTextureDesc target_desc = {
    .width = 8,
    .height = 8,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture target = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(&device, &target_desc, &target));
  ArRenderTextureDesc premultiplied_desc = target_desc;
  premultiplied_desc.blend = kArRenderBlendMode_AddPremultiplied;
  ArRenderTexture premultiplied_target = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(
      &device, &premultiplied_desc, &premultiplied_target));
  AssertTextureState(
      ArSdlRenderBackend_UnwrapTexture(premultiplied_target),
      1.0f, 1.0f, 1.0f, 1.0f, SDL_BLENDMODE_ADD_PREMULTIPLIED);
  const SDL_Rect saved_viewport = {1, 2, 12, 11};
  const SDL_Rect saved_clip = {3, 4, 6, 5};
  assert(SDL_SetRenderViewport(renderer, &saved_viewport));
  assert(SDL_SetRenderClipRect(renderer, &saved_clip));
  assert(SDL_SetRenderDrawColor(renderer, 17, 34, 51, 68));

  ArRenderTargetState target_state = {0};
  assert(ArRenderDevice_BeginTarget(&device, target, &target_state) ==
         kArRenderTargetBegin_Ready);
  assert(SDL_GetRenderTarget(renderer) ==
         ArSdlRenderBackend_UnwrapTexture(target));
  assert(!SDL_RenderViewportSet(renderer));
  assert(!SDL_RenderClipEnabled(renderer));
  assert(ArRenderDevice_UseOutputCoordinates(&device));
  assert(ArRenderDevice_GetOutputSize(
      &device, &output_width, &output_height));
  assert(output_width == 8 && output_height == 8);
  assert(ArRenderDevice_Clear(
      &device, (ArRenderColorF){0.5f, 0.25f, 0.75f, 1.0f}));
  Uint8 draw_r = 0, draw_g = 0, draw_b = 0, draw_a = 0;
  assert(SDL_GetRenderDrawColor(
      renderer, &draw_r, &draw_g, &draw_b, &draw_a));
  assert(draw_r == 17 && draw_g == 34 && draw_b == 51 && draw_a == 68);

  assert(ArRenderDevice_EndTarget(&device, &target_state));
  assert(SDL_GetRenderTarget(renderer) == NULL);
  SDL_Rect restored = {0};
  assert(SDL_RenderViewportSet(renderer));
  assert(SDL_GetRenderViewport(renderer, &restored));
  assert(!memcmp(&restored, &saved_viewport, sizeof(restored)));
  assert(SDL_RenderClipEnabled(renderer));
  assert(SDL_GetRenderClipRect(renderer, &restored));
  assert(!memcmp(&restored, &saved_clip, sizeof(restored)));

  ArRenderDevice_DestroyTexture(&device, target);
  ArRenderDevice_DestroyTexture(&device, premultiplied_target);
  ArRenderDevice_DestroyTexture(&device, texture);
  ArRenderDevice_Reset(&device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  puts("render_sdl_state_test: ok");
  return 0;
}
