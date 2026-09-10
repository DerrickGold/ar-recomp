#ifndef AR_RENDER_TEXT_SURFACE_CACHE_H
#define AR_RENDER_TEXT_SURFACE_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/text_rasterizer.h"
#include "render/render_device.h"

typedef struct ArTextCacheKey {
  uint64_t primary;
  uint64_t secondary;
} ArTextCacheKey;

typedef struct ArTextSurface {
  ArRenderTexture texture;
  ArTextCacheKey key;
  int width;
  int height;
  int ascent;
  int descent;
  int line_advance;
  /* Actual post-pixelation ink, measured once on cache miss. Empty regions
   * represent spaces; never use padded line/reveal rectangles as ink bounds. */
  ArRenderRectI ink_bounds;
  /* Entry-owned immutable metadata. Valid until this cache entry is evicted
   * or the cache is destroyed. */
  const ArTextRevealCluster *reveal_clusters;
  const ArRenderRectI *cluster_ink_bounds;
  size_t reveal_cluster_count;
} ArTextSurface;

typedef struct ArTextSurfaceCacheStats {
  uint64_t lookups;
  uint64_t hits;
  uint64_t misses;
  uint64_t rasterize_calls;
  uint64_t upload_calls;
  uint64_t evictions;
  uint64_t failures;
  uint64_t negative_hits;
} ArTextSurfaceCacheStats;

typedef struct ArTextSurfaceCacheEntry {
  ArTextSurface surface;
  uint64_t last_use;
  bool valid;
} ArTextSurfaceCacheEntry;

enum { kArTextNegativeCacheCapacity = 16 };
typedef struct ArTextSurfaceFailure {
  ArTextCacheKey key;
  char error[kArTextRasterErrorCapacity];
  bool valid;
} ArTextSurfaceFailure;

typedef struct ArTextSurfaceCache {
  ArTextSurfaceCacheEntry *entries;
  size_t capacity;
  uint64_t clock;
  ArRenderDevice *device;
  ArTextSurfaceCacheStats stats;
  ArTextSurfaceFailure failures[kArTextNegativeCacheCapacity];
  unsigned next_failure;
} ArTextSurfaceCache;

/* Cache keys include UTF-8 bytes, font-stack identity/revision, shaping and
 * layout options, style, and rasterizer implementation revision. They are
 * deterministic across supported host endianness and never persisted. */
ArTextCacheKey ArTextSurfaceCache_MakeKey(
    const ArTextRasterizer *rasterizer,
    const ArTextRasterRequest *request);
bool ArTextCacheKey_Equals(ArTextCacheKey left, ArTextCacheKey right);

bool ArTextSurfaceCache_Init(ArTextSurfaceCache *cache, size_t capacity);
void ArTextSurfaceCache_Destroy(ArTextSurfaceCache *cache,
                                ArRenderDevice *device);

/* Presenter-thread API. On a hit this performs no font work, allocation, or
 * texture upload. On a miss the replacement texture is fully rasterized and
 * uploaded before an LRU entry is evicted, so failure leaves the old cache
 * usable. Failed raster requests have a separate bounded negative cache, so
 * static layout failures do not repeat font work or evict usable textures.
 * A changed request/font/backend revision retries; destroy/init also clears
 * failures (including after device recreation). Upload failures are retried. */
bool ArTextSurfaceCache_Acquire(
    ArTextSurfaceCache *cache,
    ArRenderDevice *device,
    const ArTextRasterizer *rasterizer,
    const ArTextRasterRequest *request,
    ArTextSurface *out_surface,
    char *error, size_t error_capacity);

const ArTextSurfaceCacheStats *ArTextSurfaceCache_GetStats(
    const ArTextSurfaceCache *cache);
void ArTextSurfaceCache_ResetStats(ArTextSurfaceCache *cache);

#endif /* AR_RENDER_TEXT_SURFACE_CACHE_H */
