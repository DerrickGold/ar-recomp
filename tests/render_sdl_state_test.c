#include "platform/sdl/render_sdl_internal.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool NearlyEqual(float left, float right) {
  return fabsf(left - right) < 0.001f;
}

static bool ByteNear(Uint8 left, Uint8 right) {
  return abs((int)left - (int)right) <= 2;
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
  ArSdlRenderBackend duplicate_backend = {0};
  const ArRenderBackendOps *bound_ops = device.ops;
  void *bound_context = device.context;
  assert(!ArSdlRenderBackend_Bind(
      &device, &duplicate_backend, renderer));
  assert(device.ops == bound_ops && device.context == bound_context);
  assert(duplicate_backend.renderer == NULL);
  assert(SDL_SetRenderLogicalPresentation(
      renderer, 8, 8, SDL_LOGICAL_PRESENTATION_LETTERBOX));
  int output_width = 0;
  int output_height = 0;
  assert(ArRenderDevice_GetOutputSize(
      &device, &output_width, &output_height));
  assert(output_width == 16 && output_height == 16);
  assert(ArRenderDevice_UseOutputCoordinates(&device));
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
  const ArRenderDrawState mask_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_DestinationAlphaMask,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, NULL, &destination, &mask_state));
  AssertTextureState(
      native, 0.8f, 0.7f, 0.6f, 0.5f, SDL_BLENDMODE_BLEND);
  const ArRenderDrawState accumulate_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_AlphaAccumulate,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, NULL, &destination, &accumulate_state));
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
  ArRenderTextureDesc alpha_premultiplied_desc = target_desc;
  alpha_premultiplied_desc.blend = kArRenderBlendMode_AlphaPremultiplied;
  ArRenderTexture alpha_premultiplied_target = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(
      &device, &alpha_premultiplied_desc, &alpha_premultiplied_target));
  const SDL_BlendMode alpha_premultiplied = SDL_ComposeCustomBlendMode(
      SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
      SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
      SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
  AssertTextureState(
      ArSdlRenderBackend_UnwrapTexture(alpha_premultiplied_target),
      1.0f, 1.0f, 1.0f, 1.0f, alpha_premultiplied);
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

  /* The shadow fallback depends on adding weighted source alpha while leaving
   * the mask's colour untouched. Exercise the composed blend numerically, not
   * just its temporary texture-state restoration. */
  assert(ArRenderDevice_Clear(
      &device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f}));
  const ArRenderDrawState weighted_accumulate = {
    .flags = kArRenderDrawState_Tint | kArRenderDrawState_Blend,
    .tint = {1.0f, 1.0f, 1.0f, 64.0f / 255.0f},
    .blend = kArRenderBlendMode_AlphaAccumulate,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, NULL, &destination, &weighted_accumulate));
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, NULL, &destination, &weighted_accumulate));
  SDL_Surface *accumulated = SDL_RenderReadPixels(renderer, NULL);
  assert(accumulated);
  Uint8 accumulated_r = 255, accumulated_g = 255;
  Uint8 accumulated_b = 255, accumulated_a = 0;
  assert(SDL_ReadSurfacePixel(
      accumulated, 0, 0, &accumulated_r, &accumulated_g,
      &accumulated_b, &accumulated_a));
  assert(accumulated_r == 0 && accumulated_g == 0 && accumulated_b == 0);
  assert(accumulated_a == 128);
  SDL_DestroySurface(accumulated);

  /* A stack group is rendered over transparent with ordinary alpha, which
   * leaves premultiplied RGB in its target. Compositing that target once with
   * premultiplied-alpha-over must match the original ordered direct draws. */
  assert(ArRenderDevice_Clear(
      &device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f}));
  const ArRenderRectF group_rect = {0.0f, 0.0f, 8.0f, 8.0f};
  const ArRenderColorF group_red = {1.0f, 0.0f, 0.0f, 0.5f};
  const ArRenderColorF group_blue = {0.0f, 0.0f, 1.0f, 0.25f};
  assert(ArRenderDevice_DrawSolidRect(
      &device, &group_rect, group_red, kArRenderBlendMode_Alpha));
  assert(ArRenderDevice_DrawSolidRect(
      &device, &group_rect, group_blue, kArRenderBlendMode_Alpha));

  assert(ArRenderDevice_EndTarget(&device, &target_state));
  assert(SDL_GetRenderTarget(renderer) == NULL);
  SDL_Rect restored = {0};
  assert(SDL_RenderViewportSet(renderer));
  assert(SDL_GetRenderViewport(renderer, &restored));

  const ArRenderColorF group_background = {0.0f, 0.75f, 0.0f, 1.0f};
  assert(ArRenderDevice_Clear(&device, group_background));
  const ArRenderDrawState premultiplied_over_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_AlphaPremultiplied,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, target, NULL, &group_rect, &premultiplied_over_state));
  Uint8 grouped_r = 0, grouped_g = 0, grouped_b = 0, grouped_a = 0;
  assert(SDL_ReadSurfacePixel(
      surface, 1, 2, &grouped_r, &grouped_g, &grouped_b, &grouped_a));

  assert(ArRenderDevice_Clear(&device, group_background));
  assert(ArRenderDevice_DrawSolidRect(
      &device, &group_rect, group_red, kArRenderBlendMode_Alpha));
  assert(ArRenderDevice_DrawSolidRect(
      &device, &group_rect, group_blue, kArRenderBlendMode_Alpha));
  Uint8 direct_r = 0, direct_g = 0, direct_b = 0, direct_a = 0;
  assert(SDL_ReadSurfacePixel(
      surface, 1, 2, &direct_r, &direct_g, &direct_b, &direct_a));
  assert(ByteNear(grouped_r, direct_r));
  assert(ByteNear(grouped_g, direct_g));
  assert(ByteNear(grouped_b, direct_b));
  assert(ByteNear(grouped_a, direct_a));
  assert(!memcmp(&restored, &saved_viewport, sizeof(restored)));
  assert(SDL_RenderClipEnabled(renderer));
  assert(SDL_GetRenderClipRect(renderer, &restored));
  assert(!memcmp(&restored, &saved_clip, sizeof(restored)));

  ArRenderDevice_DestroyTexture(&device, target);
  ArRenderDevice_DestroyTexture(&device, premultiplied_target);
  ArRenderDevice_DestroyTexture(&device, alpha_premultiplied_target);
  ArRenderDevice_DestroyTexture(&device, texture);
  ArSdlRenderBackend_Destroy(&device);
  assert(!ArRenderDevice_IsReady(&device));
  /* Bind borrows focused-test renderers; destroying the adapter must leave the
   * externally owned renderer alive. */
  assert(SDL_SetRenderDrawColor(renderer, 1, 2, 3, 255));
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  puts("render_sdl_state_test: ok");
  return 0;
}
