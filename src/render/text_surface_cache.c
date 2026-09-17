#include "render/text_surface_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deterministic_hash.h"
#include "localization/text_boundaries.h"

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
  hash = HashU32(hash, request->band_rgb);
  hash = HashU32(hash, request->body_rgb);
  hash = HashU32(hash, request->accent_end_utf8_byte);
  hash = HashU32(hash, request->accent_rgb);
  hash = HashU32(hash, request->shadow_enabled);
  hash = HashU32(hash, request->shadow_rgb);
  hash = HashU32(hash, request->shadow_shape);
  hash = HashU32(hash, request->flags);
  hash = HashU32(hash, (uint32_t)request->direction);
  hash = HashU32(hash, request->bidi_source_offset);
  hash = HashU64(hash, request->bidi_span_count);
  for (size_t i = 0; i < request->bidi_span_count; ++i) {
    hash = HashU32(hash, request->bidi_spans[i].start);
    hash = HashU32(hash, request->bidi_spans[i].end);
    hash = HashU32(hash, request->bidi_spans[i].direction);
  }
  hash = HashU32(hash, (uint32_t)request->alignment);
  hash = HashU32(hash, (uint32_t)request->font_pixels);
  hash = HashU32(hash, (uint32_t)request->minimum_font_pixels);
  hash = HashU32(hash, (uint32_t)request->maximum_width);
  hash = HashU32(hash, (uint32_t)request->maximum_height);
  hash = HashU32(hash, (uint32_t)request->filter);
  hash = HashU32(hash, (uint32_t)request->pixelation);
  hash = HashU32(hash, (uint32_t)request->pixelation_size);
  hash = HashU32(hash, (uint32_t)request->raster_font_pixels);
  hash = HashU32(hash, request->align_pixelation_grid);
  hash = HashU32(hash, (uint32_t)request->pixelation_grid_x);
  hash = HashU32(hash, (uint32_t)request->pixelation_grid_y);
  hash = HashU64(hash, (uint64_t)request->language_bcp47_bytes);
  if (request->language_bcp47_bytes)
    hash = DeterministicHash_Fnv1a64(
        hash, request->language_bcp47, request->language_bcp47_bytes);
  hash = HashU64(hash, (uint64_t)request->utf8_bytes);
  /* Fold preferred-break semantics into the existing text scan. This keeps
   * cache lookup linear in text bytes rather than adding another full pass. */
  for (size_t i = 0; i < request->utf8_bytes; ++i) {
    hash = DeterministicHash_Fnv1a64Byte(hash, (uint8_t)request->utf8[i]);
    if (request->utf8[i] == ' ')
      hash = DeterministicHash_Fnv1a64Byte(
          hash, request->preferred_line_breaks &&
                        ArTextBoundary_Get(
                            request->preferred_line_breaks,
                            request->preferred_line_break_source_offset + i));
  }
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
    /* Nothing is pinned until a frame declares itself. */
    .frame_start = UINT64_MAX,
  };
  return true;
}

static void ReleaseSurfaceResources(ArTextSurface *surface,
                                    ArRenderDevice *device,
                                    bool release_texture) {
  if (!surface) return;
  if (release_texture && device)
    ArRenderDevice_DestroyTexture(device, surface->texture);
  free((void *)surface->reveal_clusters);
  free((void *)surface->cluster_ink_bounds);
  free((void *)surface->reveal_pieces);
  memset(surface, 0, sizeof(*surface));
}

void ArTextSurfaceCache_Destroy(ArTextSurfaceCache *cache,
                                ArRenderDevice *device) {
  if (!cache) return;
  ArRenderDevice *owner = cache->device ? cache->device : device;
  for (size_t i = 0; i < cache->capacity; ++i)
    ReleaseSurfaceResources(&cache->entries[i].surface, owner,
                            cache->entries[i].valid);
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
  const size_t pixel_bytes = (size_t)bytes_per_pixel;
  /* Every pixel of a block repeats one sample, so each band's first row is
   * filled block by block (one pixel, then doubling copies) and copied down
   * the band. A 4-byte memcpy call per output pixel was a full-page pass on
   * every enhanced text change; the bytes written are unchanged. */
  for (int block_y = 0; block_y < source->height;
       block_y += block_size) {
    const int sample_y = block_y + block_size / 2 < source->height
        ? block_y + block_size / 2 : source->height - 1;
    const int end_y = block_y + block_size < source->height
        ? block_y + block_size : source->height;
    const uint8_t *sample_row =
        input + (size_t)sample_y * (size_t)source->pitch_bytes;
    uint8_t *band_row = output + (size_t)block_y * (size_t)output_pitch;
    for (int block_x = 0; block_x < source->width;
         block_x += block_size) {
      const int sample_x = block_x + block_size / 2 < source->width
          ? block_x + block_size / 2 : source->width - 1;
      const int end_x = block_x + block_size < source->width
          ? block_x + block_size : source->width;
      uint8_t *block = band_row + (size_t)block_x * pixel_bytes;
      const size_t span = (size_t)(end_x - block_x) * pixel_bytes;
      memcpy(block, sample_row + (size_t)sample_x * pixel_bytes, pixel_bytes);
      for (size_t filled = pixel_bytes; filled < span;) {
        const size_t chunk = filled < span - filled ? filled : span - filled;
        memcpy(block + filled, block, chunk);
        filled += chunk;
      }
    }
    for (int y = block_y + 1; y < end_y; ++y)
      memcpy(output + (size_t)y * (size_t)output_pitch, band_row,
             (size_t)source->width * pixel_bytes);
  }
  *pixels = output;
  *pitch_bytes = output_pitch;
  return true;
}

/* A copy of a bitmap moved onto another surface's mosaic grid and padded out
 * to whole blocks. Mosaic blocks sample their centre, so a line mosaicked on
 * its own only matches the same line inside its page when every block covers
 * the same pixels: the leading padding restores the page's block phase, and
 * the trailing padding stops a partial last block from sampling its own edge
 * where the page's block would have sampled beyond the line. */
typedef struct GridAlignedBitmap {
  ArTextBitmap bitmap;
  uint8_t *pixels;
  uint32_t *owners;
  ArTextRevealCluster *clusters;
  int pad_left;
  int pad_top;
} GridAlignedBitmap;

static void ReleaseGridAligned(GridAlignedBitmap *aligned) {
  free(aligned->pixels);
  free(aligned->owners);
  free(aligned->clusters);
  memset(aligned, 0, sizeof(*aligned));
}

static int PositiveModulo(int64_t value, int modulus) {
  const int64_t remainder = value % modulus;
  return (int)(remainder < 0 ? remainder + modulus : remainder);
}

static bool AlignToPixelationGrid(const ArTextBitmap *source, int block,
                                  int grid_x, int grid_y,
                                  GridAlignedBitmap *out) {
  memset(out, 0, sizeof(*out));
  const int bytes_per_pixel = BytesPerPixel(source->format);
  /* The grid is stated against the uncropped layout; this bitmap starts
   * crop_left/crop_top pixels into it. */
  const int pad_left = PositiveModulo(
      (int64_t)source->crop_left - grid_x, block);
  const int pad_top = PositiveModulo(
      (int64_t)source->crop_top - grid_y, block);
  int64_t width = (int64_t)pad_left + source->width;
  int64_t height = (int64_t)pad_top + source->height;
  width += PositiveModulo(-width, block);
  height += PositiveModulo(-height, block);
  if (bytes_per_pixel <= 0 || width > 65536 || height > 65536 ||
      (source->reveal_cluster_count &&
       source->reveal_cluster_count > SIZE_MAX / sizeof(*out->clusters)))
    return false;
  const size_t pitch = (size_t)width * (size_t)bytes_per_pixel;
  out->pixels = (uint8_t *)calloc((size_t)height, pitch);
  if (source->pixel_owners)
    out->owners = (uint32_t *)calloc((size_t)width * (size_t)height,
                                     sizeof(*out->owners));
  if (source->reveal_cluster_count)
    out->clusters = (ArTextRevealCluster *)malloc(
        source->reveal_cluster_count * sizeof(*out->clusters));
  if (!out->pixels || (source->pixel_owners && !out->owners) ||
      (source->reveal_cluster_count && !out->clusters)) {
    ReleaseGridAligned(out);
    return false;
  }
  for (int y = 0; y < source->height; ++y) {
    memcpy(out->pixels + (size_t)(y + pad_top) * pitch +
               (size_t)pad_left * (size_t)bytes_per_pixel,
           (const uint8_t *)source->pixels +
               (size_t)y * (size_t)source->pitch_bytes,
           (size_t)source->width * (size_t)bytes_per_pixel);
    if (out->owners)
      memcpy(out->owners + (size_t)(y + pad_top) * (size_t)width + pad_left,
             source->pixel_owners + (size_t)y * (size_t)source->width,
             (size_t)source->width * sizeof(*out->owners));
  }
  for (size_t index = 0; index < source->reveal_cluster_count; ++index) {
    out->clusters[index] = source->reveal_clusters[index];
    out->clusters[index].x += pad_left;
    out->clusters[index].y += pad_top;
  }
  out->bitmap = *source;
  out->bitmap.pixels = out->pixels;
  out->bitmap.width = (int)width;
  out->bitmap.height = (int)height;
  out->bitmap.pitch_bytes = (int)pitch;
  out->bitmap.pixel_owners = out->owners;
  out->bitmap.reveal_clusters = out->clusters;
  out->pad_left = pad_left;
  out->pad_top = pad_top;
  return true;
}

static uint32_t TreatedOwner(const ArTextBitmap *b, int x, int y,
                              int scale, int mosaic) {
  x /= scale;
  y /= scale;
  if (mosaic > 1) {
    x = x / mosaic * mosaic + mosaic / 2;
    y = y / mosaic * mosaic + mosaic / 2;
    if (x >= b->width) x = b->width - 1;
    if (y >= b->height) y = b->height - 1;
  }
  return b->pixel_owners[(size_t)y * b->width + x];
}

/* Cache-miss only. Pixelation samples owner and color at the identical source
 * pixel. Merge equal horizontal runs vertically; the resulting rectangles
 * are disjoint, so partial/shifted draws cannot double-blend or leak a neighbor.
 * Both scratch and retained metadata are bounded, independently of the font. */
static bool BuildRevealPieces(const ArTextBitmap *b, int scale, int mosaic,
    ArTextRevealPiece **out, size_t *count, size_t *allocation,
    ArRenderRectI *ink) {
  *out = NULL;
  *count = 0;
  *allocation = 0;
  if (!b->pixel_owners) return true;
  const int width = b->width * scale, height = b->height * scale;
  if (width <= 0 || width > 65536 || height <= 0) return false;
  size_t *previous = malloc((size_t)width * sizeof(*previous));
  size_t *current = malloc((size_t)width * sizeof(*current));
  ArTextRevealPiece *pieces = NULL;
  size_t used = 0, capacity = 0;
  if (!previous || !current) goto fail;
  memset(previous, 0xff, (size_t)width * sizeof(*previous));
  for (int y = 0; y < height; ++y) {
    memset(current, 0xff, (size_t)width * sizeof(*current));
    for (int x = 0; x < width;) {
      const uint32_t owner = TreatedOwner(b, x, y, scale, mosaic);
      const int left = x++;
      while (x < width && TreatedOwner(b, x, y, scale, mosaic) == owner) ++x;
      if (!owner) continue;
      if (owner > b->reveal_cluster_count) goto fail;
      const int w = x - left;
      size_t index = previous[left];
      if (index != SIZE_MAX && pieces[index].cluster_index == owner - 1 &&
          pieces[index].source.w == w) {
        ++pieces[index].source.h;
      } else {
        if (used == capacity) {
          if (capacity == 65536) goto fail;
          const size_t next = capacity ? capacity * 2 : 64;
          ArTextRevealPiece *grown = realloc(pieces, next * sizeof(*pieces));
          if (!grown) goto fail;
          pieces = grown;
          capacity = next;
        }
        index = used++;
        pieces[index] = (ArTextRevealPiece){{left, y, w, 1}, owner - 1};
      }
      current[left] = index;
      ArRenderRectI *bounds = &ink[owner - 1];
      if (!bounds->w) *bounds = (ArRenderRectI){left, y, w, 1};
      else {
        const int right = bounds->x + bounds->w > x ? bounds->x + bounds->w : x;
        if (left < bounds->x) bounds->x = left;
        bounds->w = right - bounds->x;
        bounds->h = y - bounds->y + 1;
      }
    }
    size_t *swap = previous;
    previous = current;
    current = swap;
  }
  free(previous);
  free(current);
  *out = pieces;
  *count = used;
  *allocation = capacity * sizeof(*pieces);
  return true;
fail:
  free(previous);
  free(current);
  free(pieces);
  return false;
}

/* Texture bytes for a surface, saturating rather than wrapping so an absurd
 * request is rejected instead of appearing free. */
static uint64_t SurfaceBytes(int width, int height,
                             ArRenderPixelFormat format) {
  if (width <= 0 || height <= 0) return 0;
  const uint64_t pixels = (uint64_t)width * (uint64_t)height;
  if (pixels > UINT64_MAX / 8u) return UINT64_MAX;
  return pixels * (uint64_t)BytesPerPixel(format);
}

static void ReleaseEntry(ArTextSurfaceCache *cache, ArRenderDevice *device,
                         ArTextSurfaceCacheEntry *entry) {
  if (!entry->valid) return;
  ReleaseSurfaceResources(&entry->surface, device, true);
  cache->stats.texture_bytes -= entry->texture_bytes;
  cache->stats.effect_metadata_bytes -= entry->effect_metadata_bytes;
  entry->valid = false;
  entry->texture_bytes = 0;
  ++cache->stats.evictions;
}

/* Least recently used entry that the frame being prepared is not already
 * holding, or capacity when every entry is pinned. */
static size_t SelectByteVictim(const ArTextSurfaceCache *cache,
                               size_t protected_index) {
  size_t victim = cache->capacity;
  for (size_t i = 0; i < cache->capacity; ++i) {
    if (i == protected_index || !cache->entries[i].valid ||
        cache->entries[i].last_use >= cache->frame_start)
      continue;
    if (victim == cache->capacity ||
        cache->entries[i].last_use < cache->entries[victim].last_use)
      victim = i;
  }
  return victim;
}

static void EnforceByteBudget(ArTextSurfaceCache *cache,
                              ArRenderDevice *device, size_t protected_index) {
  if (!cache->byte_budget) return;
  while (cache->stats.texture_bytes + cache->stats.effect_metadata_bytes > cache->byte_budget) {
    const size_t victim = SelectByteVictim(cache, protected_index);
    if (victim >= cache->capacity) return;
    ReleaseEntry(cache, device, &cache->entries[victim]);
  }
}

void ArTextSurfaceCache_SetByteBudget(ArTextSurfaceCache *cache,
                                      uint64_t budget) {
  if (cache) cache->byte_budget = budget;
}

void ArTextSurfaceCache_BeginFrame(ArTextSurfaceCache *cache) {
  if (cache) cache->frame_start = cache->clock + 1u;
}

void ArTextSurfaceCache_EndFrame(ArTextSurfaceCache *cache) {
  if (cache) cache->frame_start = UINT64_MAX;
}

static size_t SelectVictim(const ArTextSurfaceCache *cache) {
  for (size_t i = 0; i < cache->capacity; ++i) {
    if (!cache->entries[i].valid) return i;
  }
  return SelectByteVictim(cache, cache->capacity);
}

static ArTextSurfaceFailure *FindFailure(ArTextSurfaceCache *cache,
                                        ArTextCacheKey key) {
  for (size_t i = 0; i < kArTextNegativeCacheCapacity; ++i) {
    ArTextSurfaceFailure *failure = &cache->failures[i];
    if (failure->valid && ArTextCacheKey_Equals(failure->key, key))
      return failure;
  }
  return NULL;
}

/* Re-records `existing` in place when this key has failed before, so a request
 * that keeps failing cannot fill the ring and evict other keys' answers. */
static void RememberFailure(ArTextSurfaceCache *cache,
                            ArTextSurfaceFailure *existing, ArTextCacheKey key,
                            ArTextRasterFailure kind, const char *message) {
  ArTextSurfaceFailure *failure = existing
      ? existing
      : &cache->failures[cache->next_failure++ % kArTextNegativeCacheCapacity];
  const uint32_t attempts =
      existing && existing->kind == kind && existing->attempts < UINT32_MAX
          ? existing->attempts + 1u : 1u;
  memset(failure, 0, sizeof(*failure));
  failure->valid = true;
  failure->key = key;
  failure->kind = kind;
  failure->attempts = attempts;
  if (kind == kArTextRasterFailure_Retryable) {
    /* 1, 2, 4, ... lookups: the first retry is immediate, and a backend that
     * stays broken is not asked again every frame. */
    uint64_t delay = UINT64_C(1) << (attempts - 1u < 31u ? attempts - 1u : 31u);
    if (delay > (uint64_t)kArTextFailureRetryCeiling)
      delay = (uint64_t)kArTextFailureRetryCeiling;
    failure->retry_at = cache->clock + delay;
  }
  SetError(failure->error, sizeof(failure->error), message);
}

static void ForgetRetryableFailures(ArTextSurfaceCache *cache) {
  for (size_t i = 0; i < kArTextNegativeCacheCapacity; ++i) {
    if (cache->failures[i].valid &&
        cache->failures[i].kind == kArTextRasterFailure_Retryable)
      cache->failures[i].valid = false;
  }
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

  ArTextSurfaceFailure *remembered = FindFailure(cache, key);
  if (remembered && (remembered->kind != kArTextRasterFailure_Retryable ||
                     cache->clock < remembered->retry_at)) {
    ++cache->stats.hits;
    ++cache->stats.negative_hits;
    SetError(error, error_capacity, remembered->error);
    return false;
  }

  ++cache->stats.misses;
  /* Refuse entry pressure before doing any font work. A prepared frame owns
   * every pinned handle until EndFrame/BeginFrame, even if bytes would fit. */
  const size_t victim_index = SelectVictim(cache);
  if (victim_index >= cache->capacity) {
    ++cache->stats.failures;
    SetError(error, error_capacity, "text surface cache entries are all in use");
    return false;
  }
  ++cache->stats.rasterize_calls;
  ArTextRasterRequest raster_request = *request;
  int metric_scale = 1;
  if (request->pixelation == kArTextPixelation_LowResolution) {
    metric_scale = request->pixelation_size;
    /* The requested block is an upper bound: retain at least eight source
     * samples per em in small windows/accessibility-size combinations. */
    int maximum_scale = request->font_pixels / 8;
    if (maximum_scale < 1) maximum_scale = 1;
    if (metric_scale > maximum_scale) metric_scale = maximum_scale;
    /* Keep the reduced wrap box from becoming proportionally narrower than
     * the reduced font. Rounding an odd font size up while flooring the box
     * can introduce an artificial early wrap that the full-size request did
     * not have. Nearest upscaling still remains inside the original bounds. */
    raster_request.font_pixels = request->font_pixels / metric_scale;
    if (raster_request.font_pixels < 1) raster_request.font_pixels = 1;
    raster_request.minimum_font_pixels =
        request->minimum_font_pixels / metric_scale;
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
  /* The backend sees an ordinary request: an exact size is a fit with no
   * room, and grid alignment happens here, after rasterization. */
  if (request->raster_font_pixels)
    raster_request.font_pixels = raster_request.minimum_font_pixels =
        request->raster_font_pixels;
  raster_request.raster_font_pixels = 0;
  raster_request.align_pixelation_grid = false;
  raster_request.pixelation_grid_x = raster_request.pixelation_grid_y = 0;
  /* Refuse an impossible field before any font work: the backend would
   * otherwise render it at full size and only then discover it cannot fit. */
  if (SurfaceBytes(request->maximum_width, request->maximum_height,
                   kArRenderPixelFormat_Rgba8888) >
      (uint64_t)kArTextSurfaceMaximumRequestBytes) {
    ++cache->stats.failures;
    ++cache->stats.oversize_rejects;
    SetError(error, error_capacity,
             "requested text bounds exceed the per-request texture ceiling");
    return false;
  }
  ArTextBitmap bitmap;
  char raster_error[kArTextRasterErrorCapacity] = {0};
  ArTextRasterFailure kind = kArTextRasterFailure_Deterministic;
  if (!ArTextRasterizer_Rasterize(
          rasterizer, &raster_request, &bitmap, &kind,
          raster_error, sizeof(raster_error))) {
    ++cache->stats.failures;
    RememberFailure(cache, remembered, key, kind,
                    raster_error[0] ? raster_error : "text rasterization failed");
    SetError(error, error_capacity,
             raster_error[0] ? raster_error : "text rasterization failed");
    return false;
  }
  /* The backend just served a request, so nothing that failed for a transient
   * reason is still entitled to its remembered answer. */
  ForgetRetryableFailures(cache);

  int mosaic_block = 1;
  if (request->pixelation == kArTextPixelation_Mosaic) {
    /* Use actual fitted metrics, not the requested size: an automatically
     * fitted heading can be much smaller than its surrounding dialogue. */
    int block = bitmap.line_advance / 8;
    if (block > request->pixelation_size) block = request->pixelation_size;
    if (block > 1) mosaic_block = block;
  }
  GridAlignedBitmap aligned = {0};
  const ArTextBitmap *source = &bitmap;
  if (mosaic_block > 1 && request->align_pixelation_grid) {
    if (!AlignToPixelationGrid(&bitmap, mosaic_block,
                               request->pixelation_grid_x,
                               request->pixelation_grid_y, &aligned)) {
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "cannot align text to its page's mosaic grid");
      return false;
    }
    source = &aligned.bitmap;
  }

  uint8_t *treated_pixels = NULL;
  const void *upload_pixels = source->pixels;
  int upload_width = source->width;
  int upload_height = source->height;
  int upload_pitch = source->pitch_bytes;
  if (request->pixelation == kArTextPixelation_LowResolution && metric_scale > 1) {
    if (!UpscaleNearest(source, metric_scale, &treated_pixels,
                        &upload_width, &upload_height, &upload_pitch) ||
        upload_width > request->maximum_width ||
        upload_height > request->maximum_height) {
      free(treated_pixels);
      ReleaseGridAligned(&aligned);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "cannot upscale low-resolution text within requested bounds");
      return false;
    }
    upload_pixels = treated_pixels;
  } else if (mosaic_block > 1) {
    if (!ApplyMosaic(source, mosaic_block, &treated_pixels, &upload_pitch)) {
      ReleaseGridAligned(&aligned);
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
    ReleaseGridAligned(&aligned);
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
  ArRenderRectI *cluster_ink_bounds = NULL;
  ArTextRevealPiece *reveal_pieces = NULL;
  size_t reveal_piece_count = 0, effect_metadata_bytes = 0;
  if (source->reveal_cluster_count) {
    if (source->reveal_cluster_count >
        SIZE_MAX / sizeof(*reveal_clusters)) {
      free(treated_pixels);
      ReleaseGridAligned(&aligned);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity, "text reveal metadata is too large");
      return false;
    }
    reveal_clusters = (ArTextRevealCluster *)malloc(
        source->reveal_cluster_count * sizeof(*reveal_clusters));
    cluster_ink_bounds = (ArRenderRectI *)calloc(
        source->reveal_cluster_count, sizeof(*cluster_ink_bounds));
    if (!reveal_clusters || !cluster_ink_bounds) {
      free(reveal_clusters);
      free(cluster_ink_bounds);
      free(treated_pixels);
      ReleaseGridAligned(&aligned);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      ++cache->stats.failures;
      SetError(error, error_capacity,
               "out of memory caching text reveal metadata");
      return false;
    }
    memcpy(reveal_clusters, source->reveal_clusters,
           source->reveal_cluster_count * sizeof(*reveal_clusters));
    if (metric_scale > 1) {
      for (size_t index = 0;
           index < source->reveal_cluster_count; ++index) {
        reveal_clusters[index].x *= metric_scale;
        reveal_clusters[index].y *= metric_scale;
        reveal_clusters[index].width *= metric_scale;
        reveal_clusters[index].height *= metric_scale;
      }
    }
  }
  const ArTextBitmap treated_bitmap = {
    .pixels = upload_pixels, .width = upload_width, .height = upload_height,
    .pitch_bytes = upload_pitch, .format = bitmap.format,
  };
  const ArRenderRectI ink_bounds = ArTextBitmap_InkBounds(
      &treated_bitmap, (ArRenderRectI){0, 0, upload_width, upload_height});
  for (size_t index = 0; !source->pixel_owners && index < source->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &reveal_clusters[index];
    cluster_ink_bounds[index] = ArTextBitmap_InkBounds(&treated_bitmap,
        (ArRenderRectI){cluster->x, cluster->y, cluster->width, cluster->height});
  }
  if (!BuildRevealPieces(source, metric_scale, mosaic_block, &reveal_pieces,
        &reveal_piece_count, &effect_metadata_bytes, cluster_ink_bounds)) {
    free(treated_pixels);free(reveal_clusters);free(cluster_ink_bounds);
    ReleaseGridAligned(&aligned);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
    ++cache->stats.failures;
    SetError(error, error_capacity, "cannot build bounded text effect reveal geometry");
    return false;
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
    .ascent = (bitmap.ascent + aligned.pad_top) * metric_scale,
    .descent = bitmap.descent * metric_scale,
    .line_advance = bitmap.line_advance * metric_scale,
    .paragraph_direction = bitmap.paragraph_direction,
    .raster_font_pixels = bitmap.font_pixels,
    .raster_scale = metric_scale,
    .mosaic_block = mosaic_block,
    .origin_x = (aligned.pad_left - bitmap.crop_left) * metric_scale,
    .origin_y = (aligned.pad_top - bitmap.crop_top) * metric_scale,
    .ink_bounds = ink_bounds,
    .reveal_clusters = reveal_clusters,
    .cluster_ink_bounds = cluster_ink_bounds,
    .reveal_cluster_count = source->reveal_cluster_count,
    .reveal_pieces = reveal_pieces,
    .reveal_piece_count = reveal_piece_count,
  };
  free(treated_pixels);
  ReleaseGridAligned(&aligned);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  if (!created || !uploaded) {
    ArRenderDevice_DestroyTexture(device, texture);
    free(reveal_clusters);
    free(cluster_ink_bounds);
    free(reveal_pieces);
    ++cache->stats.failures;
    SetError(error, error_capacity,
             ArRenderDevice_LastError(device));
    return false;
  }

  ArTextSurfaceCacheEntry *victim = &cache->entries[victim_index];
  ReleaseEntry(cache, device, victim);
  *victim = (ArTextSurfaceCacheEntry){
    .surface = replacement,
    .last_use = cache->clock,
    .texture_bytes = SurfaceBytes(upload_width, upload_height, descriptor.format),
    .effect_metadata_bytes = effect_metadata_bytes,
    .valid = true,
  };
  cache->stats.texture_bytes += victim->texture_bytes;
  cache->stats.effect_metadata_bytes += victim->effect_metadata_bytes;
  if (cache->stats.texture_bytes > cache->stats.peak_texture_bytes)
    cache->stats.peak_texture_bytes = cache->stats.texture_bytes;
  /* Evicting for bytes happens after the replacement is installed, so the
   * entry we just handed the caller is never the one released. */
  EnforceByteBudget(cache, device, victim_index);
  *out_surface = replacement;
  return true;
}

const ArTextSurfaceCacheStats *ArTextSurfaceCache_GetStats(
    const ArTextSurfaceCache *cache) {
  return cache ? &cache->stats : NULL;
}

void ArTextSurfaceCache_ResetStats(ArTextSurfaceCache *cache) {
  if (!cache) return;
  /* Live ownership is not a resettable counter. Start the new measurement
   * interval's high-water mark at the resources already owned. */
  const uint64_t live_bytes = cache->stats.texture_bytes;
  cache->stats = (ArTextSurfaceCacheStats){
    .texture_bytes = live_bytes,
    .peak_texture_bytes = live_bytes,
    .effect_metadata_bytes = cache->stats.effect_metadata_bytes,
  };
}
