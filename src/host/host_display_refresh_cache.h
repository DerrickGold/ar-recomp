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
  uint64_t interval_ns;
} HostDisplayRefreshCacheEntry;

typedef struct HostDisplayRefreshCache {
  HostDisplayRefreshCacheEntry entries[kHostDisplayRefreshCacheCapacity];
  unsigned replacement_cursor;
} HostDisplayRefreshCache;

/* Display ID zero, non-positive rates and zero intervals are invalid samples.
 * Preserve both the rounded UI rate and precise content-clock period when a
 * display query fails; substituting the rounded rate would introduce drift. */
void HostDisplayRefreshCache_Remember(
    HostDisplayRefreshCache *cache, uint32_t display_id,
    int nominal_refresh_hz, uint64_t interval_ns);
int HostDisplayRefreshCache_Get(
    const HostDisplayRefreshCache *cache, uint32_t display_id);
uint64_t HostDisplayRefreshCache_IntervalNs(
    const HostDisplayRefreshCache *cache, uint32_t display_id);
void HostDisplayRefreshCache_Forget(
    HostDisplayRefreshCache *cache, uint32_t display_id);

#endif /* AR_HOST_DISPLAY_REFRESH_CACHE_H */
