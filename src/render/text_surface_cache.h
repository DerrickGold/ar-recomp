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

typedef struct ArTextRevealPiece {
  ArRenderRectI source;
  uint32_t cluster_index;
} ArTextRevealPiece;

typedef struct ArTextSurface {
  ArRenderTexture texture;
  ArTextCacheKey key;
  int width;
  int height;
  int ascent;
  int descent;
  int line_advance;
  ArTextDirection paragraph_direction;
  /* Size the backend rasterized at after fitting, in its own pixels (before
   * low-resolution enlargement); zero when the backend did not report it. */
  int raster_font_pixels;
  /* Output pixels per rasterized pixel: the low-resolution enlargement, or 1. */
  int raster_scale;
  /* Mosaic block edge in output pixels, or 1 when no mosaic was applied. */
  int mosaic_block;
  /* Texture coordinate of the uncropped layout origin: negative after
   * whitespace cropping, positive after padding to a page's mosaic grid. */
  int origin_x;
  int origin_y;
  /* Actual post-pixelation ink, measured once on cache miss. Empty regions
   * represent spaces; never use padded line/reveal rectangles as ink bounds. */
  ArRenderRectI ink_bounds;
  /* Entry-owned immutable metadata. Valid until this cache entry is evicted
   * or the cache is destroyed. */
  const ArTextRevealCluster *reveal_clusters;
  const ArRenderRectI *cluster_ink_bounds;
  size_t reveal_cluster_count;
  /* Disjoint effect-aware rectangles; typographic advances stay unchanged. */
  const ArTextRevealPiece *reveal_pieces;
  size_t reveal_piece_count;
  const ArTextLineMetrics *lines;
  size_t line_count;
  const ArTextFontUse *font_uses;
  size_t font_use_count;
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
  /* Texture bytes the cache currently owns, and the most it has ever owned.
   * An entry count is not a memory budget: one large scrolling page at HiDPI
   * can outweigh a hundred menu labels. Peak is not reset by eviction. */
  uint64_t texture_bytes;
  uint64_t peak_texture_bytes;
  uint64_t effect_metadata_bytes;
  /* Requests refused before rasterization because their bitmap could not fit
   * the per-request ceiling. These never allocate. */
  uint64_t oversize_rejects;
} ArTextSurfaceCacheStats;

typedef struct ArTextSurfaceCacheEntry {
  ArTextSurface surface;
  uint64_t last_use;
  uint64_t texture_bytes;
  uint64_t effect_metadata_bytes;
  bool valid;
} ArTextSurfaceCacheEntry;

enum {
  kArTextNegativeCacheCapacity = 16,
  /* Backoff for a retryable failure, counted in cache lookups: the first
   * retry is immediate, and a backend that stays broken is asked at most once
   * every kArTextFailureRetryCeiling lookups instead of once per frame. */
  kArTextFailureRetryCeiling = 64,
};
typedef struct ArTextSurfaceFailure {
  ArTextCacheKey key;
  char error[kArTextRasterErrorCapacity];
  /* Deterministic failures are the permanent answer for this key. Retryable
   * ones are only remembered until `retry_at`, so a transient allocation or
   * font-resource failure cannot outlive itself and force the player to change
   * locale, font or window size to get their text back. */
  ArTextRasterFailure kind;
  uint64_t retry_at;
  uint32_t attempts;
  bool valid;
} ArTextSurfaceFailure;

/* A single request can never be worth more than this many texture bytes.
 * Fixed fields disable wrapping, so an accepted-but-impossible label would
 * otherwise be rendered at full width before anything rejected it. */
enum { kArTextSurfaceMaximumRequestBytes = 64u << 20 };

typedef struct ArTextSurfaceCache {
  ArTextSurfaceCacheEntry *entries;
  size_t capacity;
  /* Aggregate texture budget, in addition to the entry count. Zero disables
   * it. Eviction never touches an entry acquired since the current
   * frame began (see ArTextSurfaceCache_BeginFrame). */
  uint64_t byte_budget;
  /* Entries acquired at or after this clock value belong to the frame being
   * prepared and are never evicted. UINT64_MAX pins nothing. */
  uint64_t frame_start;
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
/* Optional aggregate texture + effect-metadata byte budget. Entries beyond it are evicted from
 * the least recently used end, never below one entry and never an entry the
 * current frame has already acquired. */
void ArTextSurfaceCache_SetByteBudget(ArTextSurfaceCache *cache,
                                      uint64_t budget);
/* Releases the previous frame's pins and pins subsequent acquisitions against
 * byte and entry eviction. A miss with all slots pinned fails without font
 * work; it is not negatively cached and can succeed after pins are released. */
void ArTextSurfaceCache_BeginFrame(ArTextSurfaceCache *cache);
/* Release pins after the caller has finished using every acquired surface.
 * Subsequent acquisitions are unpinned until BeginFrame (useful for fitting
 * probes, whose handles are consumed immediately rather than retained). */
void ArTextSurfaceCache_EndFrame(ArTextSurfaceCache *cache);
void ArTextSurfaceCache_Destroy(ArTextSurfaceCache *cache,
                                ArRenderDevice *device);

/* Presenter-thread API. On a hit this performs no font work, allocation, or
 * texture upload. On a miss the replacement texture is fully rasterized and
 * uploaded before an LRU entry is evicted, so failure leaves the old cache
 * usable. Failed raster requests have a separate bounded negative cache, so
 * static layout failures do not repeat font work or evict usable textures.
 * Only deterministic raster failures are remembered permanently: a retryable
 * one is retried on the next lookup and then with growing backoff, and any
 * successful rasterization clears every retryable entry. A changed
 * request/font/backend revision retries; destroy/init also clears failures
 * (including after device recreation). Upload failures are retried. */
bool ArTextSurfaceCache_Acquire(
    ArTextSurfaceCache *cache,
    ArRenderDevice *device,
    const ArTextRasterizer *rasterizer,
    const ArTextRasterRequest *request,
    ArTextSurface *out_surface,
    char *error, size_t error_capacity);

const ArTextSurfaceCacheStats *ArTextSurfaceCache_GetStats(
    const ArTextSurfaceCache *cache);
/* Reset activity counters, preserve live bytes, and restart the peak at the
 * current live size. Does not release resources, failures, or frame pins. */
void ArTextSurfaceCache_ResetStats(ArTextSurfaceCache *cache);

#endif /* AR_RENDER_TEXT_SURFACE_CACHE_H */
