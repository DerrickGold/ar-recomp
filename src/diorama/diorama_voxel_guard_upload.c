#include "diorama_voxel_guard_upload.h"
#include "action/action_voxel.h"
#include "action/action_room_scene.h"
#include <stdlib.h>
#include <string.h>

void DioramaVoxelGuardUpload_Reset(DioramaVoxelGuardUpload *cache, ArRenderDevice *device) {
  if (!cache) return;
  for (unsigned i = 0; i < 3; ++i) {
    ArRenderDevice_DestroyTexture(device, cache->view.textures[i]);
    PresentationUploadMirror_Reset(&cache->mirrors[i]);
    free(cache->pixels[i]);
  }
  memset(cache, 0, sizeof(*cache));
}

bool DioramaVoxelGuardUpload_Update(DioramaVoxelGuardUpload *cache, ArRenderDevice *device,
    const ActionVoxelGeometry *geometry, const DioramaVoxelGuardSource *source,
    const ActionRoomSceneFrameState *frame, const DioramaVoxelGuardPrefix *prefix) {
  if (!cache) return false;
  if (!source || !source->capture_generation) {
    cache->view.capture_generation = 0;
    memset(cache->source_pixels, 0, sizeof(cache->source_pixels));
    return true;
  }
  if (cache->unavailable || !device || !geometry || !frame || frame->brightness > 15 ||
      !DioramaVoxelGuard_Valid(source) || !prefix ||
      prefix->capture_generation != source->capture_generation ||
      prefix->width != source->layout.lower.width ||
      source->layout.primary_height < kDioramaVoxelGuardPrefixRows) goto failed;
  for (unsigned i = 0; i < 3; ++i)
    if (prefix->pixels[i] && prefix->pitch_pixels[i] < (size_t)prefix->width) goto failed;
  const uint64_t revision = ActionVoxel_Revision(geometry);
  if (cache->view.capture_generation == source->capture_generation &&
      cache->view.source_revision == revision &&
      !memcmp(cache->source_pixels, source->pixels, sizeof(source->pixels)) &&
      !memcmp(&cache->view.layout, &source->layout, sizeof(source->layout)) &&
      cache->brightness == frame->brightness && cache->cgwsel == frame->cgwsel &&
      cache->cgadsub == frame->cgadsub && cache->fixed_color == frame->fixed_color) return true;
  cache->view.capture_generation = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (!cache->pixels[i]) cache->pixels[i] = calloc(kDioramaVoxelGuardTexturePixels, sizeof(uint32_t));
    if (!cache->pixels[i]) { cache->unavailable = true; goto failed; }
    if (!ArRenderTexture_IsValid(cache->view.textures[i])) {
      const ArRenderTextureDesc desc = {.width = kDioramaVoxelGuardWidth,
        .height = kDioramaVoxelGuardTextureHeight, .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming, .filter = kArRenderFilter_Nearest,
        .blend = kArRenderBlendMode_Alpha};
      if (!ArRenderDevice_CreateTexture(device, &desc, &cache->view.textures[i])) {
        cache->unavailable = true; goto failed;
      }
    }
    memset(cache->pixels[i], 0, kDioramaVoxelGuardTexturePixels * sizeof(uint32_t));
    if (prefix->pixels[i]) for (unsigned y = 0; y < kDioramaVoxelGuardPrefixRows; ++y)
      memcpy(cache->pixels[i] + (size_t)y * kDioramaVoxelGuardWidth,
          prefix->pixels[i] + (size_t)y * prefix->pitch_pixels[i],
          (size_t)prefix->width * sizeof(uint32_t));
  }
  const DioramaTrunkGuardStrip *strip = &source->layout.lower;
  const DioramaTrunkGuardBounds *valid = &strip->valid;
  if (valid->width && valid->height) {
    const size_t start = (size_t)valid->y * kDioramaVoxelGuardWidth + valid->x;
    const size_t destination = start + kDioramaVoxelGuardPrefixRows * kDioramaVoxelGuardWidth;
    /* Apply repairs only within actual source bounds, exactly like the finite
     * primary. Split receives native Low only; overlapping High is untouched. */
    if (!ActionVoxel_Split(geometry, strip->world_x + valid->x, strip->world_y + valid->y,
        valid->width, valid->height, kDioramaVoxelGuardWidth, frame,
        source->pixels[0] + start, source->pitch_pixels,
        cache->pixels[0] + destination, cache->pixels[2] + destination)) goto failed;
    for (int y = 0; y < valid->height; ++y) {
      const size_t at = start + (size_t)y * kDioramaVoxelGuardWidth;
      memcpy(cache->pixels[1] + kDioramaVoxelGuardPrefixRows * kDioramaVoxelGuardWidth + at,
          source->pixels[1] + at, (size_t)valid->width * sizeof(uint32_t));
    }
  }
  for (unsigned i = 0; i < 3; ++i)
    if (!PresentationUploadMirror_UploadArgb8888(&cache->mirrors[i], device,
        cache->view.textures[i], (const uint8_t *)cache->pixels[i],
        kDioramaVoxelGuardWidth, kDioramaVoxelGuardTextureHeight,
        kDioramaVoxelGuardWidth * sizeof(uint32_t), 0, 0, NULL)) goto failed;
  cache->view.layout = source->layout;
  cache->view.capture_generation = source->capture_generation;
  cache->view.source_revision = revision;
  memcpy(cache->source_pixels, source->pixels, sizeof(source->pixels));
  cache->brightness = frame->brightness; cache->cgwsel = frame->cgwsel;
  cache->cgadsub = frame->cgadsub; cache->fixed_color = frame->fixed_color;
  return true;
failed:
  cache->view.capture_generation = 0;
  memset(cache->source_pixels, 0, sizeof(cache->source_pixels));
  return false;
}
