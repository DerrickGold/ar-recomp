#include "diorama_rom_skybox_resource.h"

#include "diorama_layer_order.h"
#include "diorama_rom_backdrop.h"

#include <stdio.h>

typedef struct DioramaRomSkyboxCache {
  uint32_t pixels[kDioramaRomBackdropPixels * kDioramaRomBackdropPixels];
  ArRenderTexture art_texture;
  ArRenderTexture target_texture;
  int source;
  uint32_t default_fill_argb;
  uint32_t transparent_fill_argb;
  bool transparent_fill_configured;
  bool composite_valid;
  bool resource_failed;
  bool available;
} DioramaRomSkyboxCache;

static const uint8_t *s_rom_data;
static size_t s_rom_size;
static DioramaRomSkyboxCache s_cache = {
  .source = -1,
};

static void FailResource(ArRenderDevice *device, const char *operation) {
  if (s_cache.resource_failed) return;
  s_cache.resource_failed = true;
  fprintf(stderr,
          "[diorama] ROM skybox %s failed (%s); using captured fallback\n",
          operation ? operation : "resource",
          ArRenderDevice_LastError(device));
}

static void DestroyTextures(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_cache.target_texture);
  s_cache.target_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(device, s_cache.art_texture);
  s_cache.art_texture = ArRenderTexture_Invalid();
  s_cache.composite_valid = false;
}

bool DioramaRomSkyboxResource_Init(const uint8_t *rom_data, size_t rom_size) {
  s_rom_data = rom_data;
  s_rom_size = rom_size;
  s_cache.available = false;
  s_cache.source = -1;
  return rom_data && rom_size > 0;
}

static bool DecodeSource(ArRenderDevice *device, int source) {
  if (s_cache.source == source) return s_cache.available;

  DestroyTextures(device);
  uint8_t group = 0, map = 0, bg = 0;
  s_cache.available =
      DioramaLayerOrder_DecodeActionBgSource(
          source, &group, &map, &bg) &&
      DioramaRomBackdrop_LoadActionBgSparse(
          s_rom_data, s_rom_size, group, map, bg, s_cache.pixels,
          sizeof(s_cache.pixels) / sizeof(s_cache.pixels[0]),
          &s_cache.default_fill_argb);
  s_cache.source = source;
  s_cache.resource_failed = false;
  fprintf(stderr, "[diorama] ROM skybox source=%s decoded=%d\n",
          DioramaLayerOrder_SourceToken(source),
          s_cache.available ? 1 : 0);
  return s_cache.available;
}

static bool EnsureTextures(ArRenderDevice *device) {
  const ArRenderTextureDesc art_desc = {
    .width = kDioramaRomBackdropPixels,
    .height = kDioramaRomBackdropPixels,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Static,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderTexture_IsValid(s_cache.art_texture)) {
    if (!ArRenderDevice_CreateTexture(
            device, &art_desc, &s_cache.art_texture) ||
        !ArRenderDevice_UpdateTexture(
            device, s_cache.art_texture, NULL, s_cache.pixels,
            kDioramaRomBackdropPixels *
                (int)sizeof(s_cache.pixels[0]))) {
      ArRenderDevice_DestroyTexture(device, s_cache.art_texture);
      s_cache.art_texture = ArRenderTexture_Invalid();
      FailResource(device, "art upload");
      return false;
    }
  }

  if (!ArRenderTexture_IsValid(s_cache.target_texture)) {
    const ArRenderTextureDesc target_desc = {
      .width = kDioramaRomBackdropPixels,
      .height = kDioramaRomBackdropPixels,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Target,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            device, &target_desc, &s_cache.target_texture)) {
      FailResource(device, "composite target creation");
      return false;
    }
    s_cache.composite_valid = false;
  }
  return true;
}

static ArRenderColorF ArgbColor(uint32_t argb) {
  const float scale = 1.0f / 255.0f;
  return (ArRenderColorF){
    .r = (float)((argb >> 16) & 0xff) * scale,
    .g = (float)((argb >> 8) & 0xff) * scale,
    .b = (float)(argb & 0xff) * scale,
    .a = (float)((argb >> 24) & 0xff) * scale,
  };
}

ArRenderTexture DioramaRomSkyboxResource_Resolve(
    ArRenderDevice *device, int source,
    bool transparent_fill_configured, uint32_t transparent_fill_argb,
    bool *state_restore_failed) {
  if (state_restore_failed) *state_restore_failed = false;
  if (!ArRenderDevice_IsReady(device) || !s_rom_data ||
      !DioramaLayerOrder_SourceIsValid(source) ||
      source == kDioramaLayerSource_Captured)
    return ArRenderTexture_Invalid();
  if (!DecodeSource(device, source) || s_cache.resource_failed ||
      !EnsureTextures(device))
    return ArRenderTexture_Invalid();

  const uint32_t fill_argb = transparent_fill_configured
      ? transparent_fill_argb : s_cache.default_fill_argb;
  if (!s_cache.composite_valid ||
      s_cache.transparent_fill_configured !=
          transparent_fill_configured ||
      s_cache.transparent_fill_argb != fill_argb) {
    ArRenderTargetState target_state;
    const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
        device, s_cache.target_texture, &target_state);
    if (begin != kArRenderTargetBegin_Ready) {
      FailResource(device, "composite target bind");
      if (state_restore_failed)
        *state_restore_failed = begin == kArRenderTargetBegin_StateLost;
      return ArRenderTexture_Invalid();
    }
    const bool composed =
        ArRenderDevice_Clear(device, ArgbColor(fill_argb)) &&
        ArRenderDevice_DrawTexture(
            device, s_cache.art_texture, NULL, NULL);
    const bool state_restored = ArRenderDevice_EndTarget(
        device, &target_state);
    if (!state_restored) {
      FailResource(device, "render-state restore");
      if (state_restore_failed) *state_restore_failed = true;
      return ArRenderTexture_Invalid();
    }
    if (!composed) {
      FailResource(device, "fill composition");
      return ArRenderTexture_Invalid();
    }
    s_cache.transparent_fill_configured =
        transparent_fill_configured;
    s_cache.transparent_fill_argb = fill_argb;
    s_cache.composite_valid = true;
  }
  return s_cache.target_texture;
}

void DioramaRomSkyboxResource_Reset(ArRenderDevice *device) {
  DestroyTextures(device);
  s_cache.resource_failed = false;
}
