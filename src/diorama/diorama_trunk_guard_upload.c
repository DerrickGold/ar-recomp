#include "diorama_trunk_guard_upload.h"
#include "action/action_trunk.h"
#include <stdlib.h>
#include <string.h>

void DioramaTrunkGuardUpload_Reset(DioramaTrunkGuardUpload *cache, ArRenderDevice *device) {
  if (!cache) return;
  for (unsigned i = 0; i < 3; ++i) {
    ArRenderDevice_DestroyTexture(device, cache->view.textures[i]);
    PresentationUploadMirror_Reset(&cache->mirrors[i]);
    free(cache->pixels[i]);
  }
  memset(cache, 0, sizeof(*cache));
}

bool DioramaTrunkGuardUpload_Update(DioramaTrunkGuardUpload *cache, ArRenderDevice *device,
    const ActionTrunkAssets *assets, const DioramaTrunkGuardSource *source, unsigned brightness) {
  if (!cache) return false;
  if (!source || !source->pixels || !source->capture_generation) {
    cache->view.capture_generation = 0;
    cache->source_pixels = NULL;
    return true;
  }
  if (!assets || !device || brightness > 15) goto failed;
  DioramaTrunkGuardLayout expected;
  if (source->pitch_pixels != kDioramaTrunkGuardWidth ||
      source->pixel_count < kDioramaTrunkGuardPixels ||
      !DioramaTrunkGuard_Resolve(source->layout.strips[0].world_x,
          source->layout.primary_y, source->layout.strips[0].width,
          source->layout.primary_height, source->layout.source, &expected) ||
      memcmp(&expected, &source->layout, sizeof(expected))) goto failed;
  if (cache->view.capture_generation == source->capture_generation &&
      cache->view.source_revision == assets->revision &&
      cache->source_pixels == source->pixels && cache->brightness == brightness &&
      !memcmp(&cache->view.layout, &source->layout, sizeof(source->layout))) return true;
  cache->view.capture_generation = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (!cache->pixels[i]) cache->pixels[i] = malloc(kDioramaTrunkGuardPixels * sizeof(uint32_t));
    if (!cache->pixels[i]) goto failed;
    if (!ArRenderTexture_IsValid(cache->view.textures[i])) {
      const ArRenderTextureDesc desc = {.width = kDioramaTrunkGuardWidth,
        .height = kDioramaTrunkGuardHeight, .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming, .filter = kArRenderFilter_Nearest,
        .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &cache->view.textures[i])) goto failed;
    }
  }
  const size_t capacities[3] = {kDioramaTrunkGuardPixels, kDioramaTrunkGuardPixels, kDioramaTrunkGuardPixels};
  if (!DioramaTrunkGuard_Split(assets, source, cache->pixels, capacities, brightness)) goto failed;
  /* Exact mirrors suppress unchanged animation-free pixels on repeated native
   * frames. Resource identity is part of each mirror; recreations reupload. */
  for (unsigned i = 0; i < 3; ++i)
    if (!PresentationUploadMirror_UploadArgb8888(&cache->mirrors[i], device,
        cache->view.textures[i], (const uint8_t *)cache->pixels[i],
        kDioramaTrunkGuardWidth, kDioramaTrunkGuardHeight,
        kDioramaTrunkGuardWidth * sizeof(uint32_t), 0, 0, NULL)) goto failed;
  cache->view.layout = source->layout;
  cache->view.capture_generation = source->capture_generation;
  cache->view.source_revision = assets->revision;
  cache->source_pixels = source->pixels;
  cache->brightness = brightness;
  return true;
failed:
  cache->view.capture_generation = 0;
  cache->source_pixels = NULL;
  return false;
}
