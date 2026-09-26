#ifndef AR_HOST_DISPLAY_REFRESH_CACHE_H
#define AR_HOST_DISPLAY_REFRESH_CACHE_H
/* HostDisplayRefreshCache: remembers each display's last valid refresh rate so
 * a transient failed query does not change pacing.
 * Phase: pure.
 * Tests: tests/host_display_refresh_cache_test.c */

#include <stdint.h>

enum { kHostDisplayRefreshCacheCapacity = 16 };

typedef struct HostDisplayRefreshCacheEntry {
  uint32_t display_id;
  int nominal_refresh_hz;
} HostDisplayRefreshCacheEntry;

typedef struct HostDisplayRefreshCache {
  HostDisplayRefreshCacheEntry entries[kHostDisplayRefreshCacheCapacity];
  unsigned replacement_cursor;
} HostDisplayRefreshCache;

/* Display ID zero and non-positive rates are invalid samples. Remembering one
 * is a no-op, which is the cache's central transient-failure contract. */
void HostDisplayRefreshCache_Remember(
    HostDisplayRefreshCache *cache, uint32_t display_id,
    int nominal_refresh_hz);
int HostDisplayRefreshCache_Get(
    const HostDisplayRefreshCache *cache, uint32_t display_id);
void HostDisplayRefreshCache_Forget(
    HostDisplayRefreshCache *cache, uint32_t display_id);

#endif /* AR_HOST_DISPLAY_REFRESH_CACHE_H */
