#include "render/text_surface_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deterministic_hash.h"

enum { kMaximumTextCacheEntries = 1024 };

static uint64_t HashU32(uint64_t hash, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    hash = DeterministicHash_Fnv1a64Byte(hash, (uint8_t)(value >> shift));
  return hash;
}

static uint64_t HashU64(uint64_t hash, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    hash = DeterministicHash_Fnv1a64Byte(hash, (uint8_t)(value >> shift));
  return hash;
}

static uint64_t HashRequest(uint64_t hash,
                            const ArTextRasterizer *rasterizer,
                            const ArTextRasterRequest *request) {
  hash = HashU64(hash, rasterizer->implementation_revision);
  hash = HashU64(hash, request->source_revision);
  hash = HashU64(hash, request->font_revision);
  hash = HashU32(hash, request->style_id);
  hash = HashU32(hash, request->flags);
  hash = HashU32(hash, (uint32_t)request->direction);
  hash = HashU32(hash, (uint32_t)request->alignment);
  hash = HashU32(hash, (uint32_t)request->font_pixels);
  hash = HashU32(hash, (uint32_t)request->minimum_font_pixels);
  hash = HashU32(hash, (uint32_t)request->maximum_width);
  hash = HashU32(hash, (uint32_t)request->maximum_height);
  hash = HashU32(hash, (uint32_t)request->filter);
  hash = HashU32(hash, (uint32_t)request->pixelation);
  hash = HashU32(hash, (uint32_t)request->pixelation_size);
  hash = HashU64(hash, (uint64_t)request->language_bcp47_bytes);
  if (request->language_bcp47_bytes)
    hash = DeterministicHash_Fnv1a64(
        hash, request->language_bcp47, request->language_bcp47_bytes);
  hash = HashU64(hash, (uint64_t)request->utf8_bytes);
  hash = DeterministicHash_Fnv1a64(hash, request->utf8, request->utf8_bytes);
  hash = HashU64(hash, (uint64_t)request->font_stack_id_bytes);
  return DeterministicHash_Fnv1a64(
      hash, request->font_stack_id, request->font_stack_id_bytes);
}

ArTextCacheKey ArTextSurfaceCache_MakeKey(
    const ArTextRasterizer *rasterizer,
    const ArTextRasterRequest *request) {
  if (!ArTextRasterizer_IsReady(rasterizer) ||
      !ArTextRasterRequest_IsValid(request))
    return (ArTextCacheKey){0};
  return (ArTextCacheKey){
    .primary = HashRequest(
        DETERMINISTIC_HASH_FNV1A64_OFFSET, rasterizer, request),
    .secondary = HashRequest(
        UINT64_C(0x9e3779b97f4a7c15), rasterizer, request),
  };
}

bool ArTextCacheKey_Equals(ArTextCacheKey left, ArTextCacheKey right) {
  return left.primary == right.primary && left.secondary == right.secondary;
}

bool ArTextSurfaceCache_Init(ArTextSurfaceCache *cache, size_t capacity) {
  if (!cache || !capacity || capacity > kMaximumTextCacheEntries) return false;
  ArTextSurfaceCacheEntry *entries = (ArTextSurfaceCacheEntry *)calloc(
      capacity, sizeof(*entries));
  if (!entries) return false;
  *cache = (ArTextSurfaceCache){
    .entries = entries,
    .capacity = capacity,
  };
  return true;
}

void ArTextSurfaceCache_Destroy(ArTextSurfaceCache *cache,
                                ArRenderDevice *device) {
  if (!cache) return;
  ArRenderDevice *owner = cache->device ? cache->device : device;
  if (owner) {
    for (size_t i = 0; i < cache->capacity; ++i) {
      if (cache->entries[i].valid) {
        ArRenderDevice_DestroyTexture(
            owner, cache->entries[i].surface.texture);
        free((void *)cache->entries[i].surface.reveal_clusters);
      }
    }
  } else {
    for (size_t i = 0; i < cache->capacity; ++i)
      free((void *)cache->entries[i].surface.reveal_clusters);
  }
  free(cache->entries);
  memset(cache, 0, sizeof(*cache));
}

static void SetError(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
}

static bool TextureExtentSupported(const ArRenderDevice *device,
                                   int width, int height) {
  const ArRenderCapabilities *capabilities =
      ArRenderDevice_Capabilities(device);
  if (!capabilities) return false;
  return (capabilities->maximum_texture_width <= 0 ||
          width <= capabilities->maximum_texture_width) &&
      (capabilities->maximum_texture_height <= 0 ||
         height <= capabilities->maximum_texture_height);
}

static int BytesPerPixel(ArRenderPixelFormat format) {
  switch (format) {
    case kArRenderPixelFormat_Argb8888:
    case kArRenderPixelFormat_Abgr8888:
    case kArRenderPixelFormat_Rgba8888: return 4;
    case kArRenderPixelFormat_Rgb565:
    case kArRenderPixelFormat_Rgba4444: return 2;
    case kArRenderPixelFormat_A8: return 1;
  }
  return 0;
}

static bool AllocatePixelBuffer(int width, int height, int bytes_per_pixel,
                                uint8_t **pixels, int *pitch_bytes) {
  if (!pixels || !pitch_bytes || width <= 0 || height <= 0 ||
      bytes_per_pixel <= 0 || width > INT32_MAX / bytes_per_pixel)
    return false;
  const int pitch = width * bytes_per_pixel;
  if ((size_t)height > SIZE_MAX / (size_t)pitch) return false;
  uint8_t *allocated = (uint8_t *)malloc((size_t)height * (size_t)pitch);
  if (!allocated) return false;
  *pixels = allocated;
  *pitch_bytes = pitch;
  return true;
}

static bool UpscaleNearest(const ArTextBitmap *source, int scale,
                           uint8_t **pixels, int *width, int *height,
                           int *pitch_bytes) {
  const int bytes_per_pixel = BytesPerPixel(source->format);
  if (scale < 2 || source->width > INT32_MAX / scale ||
      source->height > INT32_MAX / scale)
    return false;
  const int output_width = source->width * scale;
  const int output_height = source->height * scale;
  uint8_t *output = NULL;
  int output_pitch = 0;
  if (!AllocatePixelBuffer(output_width, output_height, bytes_per_pixel,
                           &output, &output_pitch))
    return false;
  const uint8_t *input = (const uint8_t *)source->pixels;
  for (int y = 0; y < output_height; ++y) {
    const uint8_t *source_row =
        input + (size_t)(y / scale) * (size_t)source->pitch_bytes;
    uint8_t *output_row = output + (size_t)y * (size_t)output_pitch;
    for (int x = 0; x < output_width; ++x)
      memcpy(output_row + (size_t)x * (size_t)bytes_per_pixel,
             source_row + (size_t)(x / scale) *
                 (size_t)bytes_per_pixel,
             (size_t)bytes_per_pixel);
  }
  *pixels = output;
  *width = output_width;
  *height = output_height;
  *pitch_bytes = output_pitch;
  return true;
}

static bool ApplyMosaic(const ArTextBitmap *source, int block_size,
                        uint8_t **pixels, int *pitch_bytes) {
  const int bytes_per_pixel = BytesPerPixel(source->format);
  uint8_t *output = NULL;
  int output_pitch = 0;
  if (block_size < 2 ||
      !AllocatePixelBuffer(source->width, source->height, bytes_per_pixel,
                           &output, &output_pitch))
    return false;
  const uint8_t *input = (const uint8_t *)source->pixels;
  for (int block_y = 0; block_y < source->height;
       block_y += block_size) {
    const int sample_y = block_y + block_size / 2 < source->height
        ? block_y + block_size / 2 : source->height - 1;
    for (int block_x = 0; block_x < source->width;
         block_x += block_size) {
      const int sample_x = block_x + block_size / 2 < source->width
          ? block_x + block_size / 2 : source->width - 1;
      const uint8_t *sample =
          input + (size_t)sample_y * (size_t)source->pitch_bytes +
          (size_t)sample_x * (size_t)bytes_per_pixel;
      const int end_y = block_y + block_size < source->height
          ? block_y + block_size : source->height;
      const int end_x = block_x + block_size < source->width
          ? block_x + block_size : source->width;
      for (int y = block_y; y < end_y; ++y) {
        uint8_t *row = output + (size_t)y * (size_t)output_pitch;
        for (int x = block_x; x < end_x; ++x)
          memcpy(row + (size_t)x * (size_t)bytes_per_pixel,
                 sample, (size_t)bytes_per_pixel);
      }
    }
  }
  *pixels = output;
  *pitch_bytes = output_pitch;
  return true;
}

static size_t SelectVictim(const ArTextSurfaceCache *cache) {
  size_t victim = 0;
  for (size_t i = 0; i < cache->capacity; ++i) {
    if (!cache->entries[i].valid) return i;
    if (cache->entries[i].last_use < cache->entries[victim].last_use)
      victim = i;
  }
  return victim;
}

bool ArTextSurfaceCache_Acquire(
    ArTextSurfaceCache *cache,
    ArRenderDevice *device,
    const ArTextRasterizer *rasterizer,
    const ArTextRasterRequest *request,
    ArTextSurface *out_surface,
    char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  if (out_surface) memset(out_surface, 0, sizeof(*out_surface));
  if (!cache || !cache->entries || !cache->capacity || !device ||
      !ArRenderDevice_IsReady(device) || !rasterizer || !out_surface ||
      !ArTextRasterRequest_IsValid(request)) {
    SetError(error, error_capacity, "invalid text surface cache request");
    return false;
  }
  if (cache->device && cache->device != device) {
    SetError(error, error_capacity,
             "text surface cache is bound to another render device");
    return false;
  }
  cache->device = device;
  ++cache->stats.lookups;
  ++cache->clock;
  if (!cache->clock) cache->clock = 1;
  const ArTextCacheKey key = ArTextSurfaceCache_MakeKey(rasterizer, request);
  if (!key.primary && !key.secondary) {
    ++cache->stats.failures;
    SetError(error, error_capacity, "cannot create text cache key");
    return false;
  }
  for (size_t i = 0; i < cache->capacity; ++i) {
    ArTextSurfaceCacheEntry *entry = &cache->entries[i];
    if (entry->valid && ArTextCacheKey_Equals(entry->surface.key, key)) {
      entry->last_use = cache->clock;
      ++cache->stats.hits;
      *out_surface = entry->surface;
      return true;
    }
  }

  ++cache->stats.misses;
  ++cache->stats.rasterize_calls;
  ArTextRasterRequest raster_request = *request;
  int metric_scale = 1;
  if (request->pixelation == kArTextPixelation_LowResolution) {
    metric_scale = request->pixelation_size;
    raster_request.font_pixels =
        (request->font_pixels + metric_scale / 2) / metric_scale;
    if (raster_request.font_pixels < 1) raster_request.font_pixels = 1;
    raster_request.minimum_font_pixels =
        (request->minimum_font_pixels + metric_scale / 2) / metric_scale;
    if (raster_request.minimum_font_pixels < 1)
      raster_request.minimum_font_pixels = 1;
    if (raster_request.minimum_font_pixels > raster_request.font_pixels)
      raster_request.minimum_font_pixels = raster_request.font_pixels;
    raster_request.maximum_width = request->maximum_width / metric_scale;
    raster_request.maximum_height = request->maximum_height / metric_scale;
    raster_request.pixelation = kArTextPixelation_None;
    raster_request.pixelation_size = 0;
    if (raster_request.maximum_width <= 0 ||
        raster_request.maximum_height <= 0) {
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "low-resolution text bounds are smaller than one pixel");
      return false;
    }
  }
  ArTextBitmap bitmap;
  char raster_error[kArTextRasterErrorCapacity] = {0};
  if (!ArTextRasterizer_Rasterize(
          rasterizer, &raster_request, &bitmap,
          raster_error, sizeof(raster_error))) {
    ++cache->stats.failures;
    SetError(error, error_capacity,
             raster_error[0] ? raster_error : "text rasterization failed");
    return false;
  }

  uint8_t *treated_pixels = NULL;
  const void *upload_pixels = bitmap.pixels;
  int upload_width = bitmap.width;
  int upload_height = bitmap.height;
  int upload_pitch = bitmap.pitch_bytes;
  if (request->pixelation == kArTextPixelation_LowResolution) {
    if (!UpscaleNearest(&bitmap, metric_scale, &treated_pixels,
                        &upload_width, &upload_height, &upload_pitch) ||
        upload_width > request->maximum_width ||
        upload_height > request->maximum_height) {
      free(treated_pixels);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "cannot upscale low-resolution text within requested bounds");
      return false;
    }
    upload_pixels = treated_pixels;
  } else if (request->pixelation == kArTextPixelation_Mosaic) {
    if (!ApplyMosaic(&bitmap, request->pixelation_size,
                     &treated_pixels, &upload_pitch)) {
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "cannot apply text mosaic treatment");
      return false;
    }
    upload_pixels = treated_pixels;
  }

  if (!TextureExtentSupported(device, upload_width, upload_height)) {
    free(treated_pixels);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
    ++cache->stats.failures;
    SetError(error, error_capacity,
             "rasterized text exceeds render-device texture limits");
    return false;
  }

  const ArRenderTextureDesc descriptor = {
    .width = upload_width,
    .height = upload_height,
    .format = bitmap.format,
    .usage = kArRenderTextureUsage_Static,
    .filter = request->filter,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture texture = ArRenderTexture_Invalid();
  ArTextRevealCluster *reveal_clusters = NULL;
  if (bitmap.reveal_cluster_count) {
    if (bitmap.reveal_cluster_count >
        SIZE_MAX / sizeof(*reveal_clusters)) {
      free(treated_pixels);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity, "text reveal metadata is too large");
      return false;
    }
    reveal_clusters = (ArTextRevealCluster *)malloc(
        bitmap.reveal_cluster_count * sizeof(*reveal_clusters));
    if (!reveal_clusters) {
      free(treated_pixels);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "out of memory caching text reveal metadata");
      return false;
    }
    memcpy(reveal_clusters, bitmap.reveal_clusters,
           bitmap.reveal_cluster_count * sizeof(*reveal_clusters));
    if (metric_scale > 1) {
      for (size_t index = 0;
           index < bitmap.reveal_cluster_count; ++index) {
        reveal_clusters[index].x *= metric_scale;
        reveal_clusters[index].y *= metric_scale;
        reveal_clusters[index].width *= metric_scale;
        reveal_clusters[index].height *= metric_scale;
      }
    }
  }
  ++cache->stats.upload_calls;
  const bool created = ArRenderDevice_CreateTexture(
      device, &descriptor, &texture);
  const bool uploaded = created && ArRenderDevice_UpdateTexture(
      device, texture, NULL, upload_pixels, upload_pitch);
  const ArTextSurface replacement = {
    .texture = texture,
    .key = key,
    .width = upload_width,
    .height = upload_height,
    .ascent = bitmap.ascent * metric_scale,
    .descent = bitmap.descent * metric_scale,
    .line_advance = bitmap.line_advance * metric_scale,
    .reveal_clusters = reveal_clusters,
    .reveal_cluster_count = bitmap.reveal_cluster_count,
  };
  free(treated_pixels);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  if (!created || !uploaded) {
    ArRenderDevice_DestroyTexture(device, texture);
    free(reveal_clusters);
    ++cache->stats.failures;
    SetError(error, error_capacity,
             ArRenderDevice_LastError(device));
    return false;
  }

  const size_t victim_index = SelectVictim(cache);
  ArTextSurfaceCacheEntry *victim = &cache->entries[victim_index];
  if (victim->valid) {
    ArRenderDevice_DestroyTexture(device, victim->surface.texture);
    free((void *)victim->surface.reveal_clusters);
    ++cache->stats.evictions;
  }
  *victim = (ArTextSurfaceCacheEntry){
    .surface = replacement,
    .last_use = cache->clock,
    .valid = true,
  };
  *out_surface = replacement;
  return true;
}

const ArTextSurfaceCacheStats *ArTextSurfaceCache_GetStats(
    const ArTextSurfaceCache *cache) {
  return cache ? &cache->stats : NULL;
}

void ArTextSurfaceCache_ResetStats(ArTextSurfaceCache *cache) {
  if (cache) memset(&cache->stats, 0, sizeof(cache->stats));
}
