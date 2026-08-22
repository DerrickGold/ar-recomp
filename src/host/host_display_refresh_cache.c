#include "host_display_refresh_cache.h"

#include <stddef.h>

static HostDisplayRefreshCacheEntry *FindEntry(
    HostDisplayRefreshCache *cache, uint32_t display_id) {
  if (!cache || !display_id) return NULL;
  for (int index = 0; index < kHostDisplayRefreshCacheCapacity; index++) {
    if (cache->entries[index].display_id == display_id)
      return &cache->entries[index];
  }
  return NULL;
}

void HostDisplayRefreshCache_Remember(
    HostDisplayRefreshCache *cache, uint32_t display_id,
    int nominal_refresh_hz) {
  if (!cache || !display_id || nominal_refresh_hz <= 0) return;
  HostDisplayRefreshCacheEntry *entry = FindEntry(cache, display_id);
  if (!entry) {
    for (int index = 0; index < kHostDisplayRefreshCacheCapacity; index++) {
      if (!cache->entries[index].display_id) {
        entry = &cache->entries[index];
        break;
      }
    }
  }
  if (!entry) {
    entry = &cache->entries[
        cache->replacement_cursor % kHostDisplayRefreshCacheCapacity];
    cache->replacement_cursor++;
  }
  *entry = (HostDisplayRefreshCacheEntry){display_id, nominal_refresh_hz};
}

int HostDisplayRefreshCache_Get(
    const HostDisplayRefreshCache *cache, uint32_t display_id) {
  if (!cache || !display_id) return 0;
  for (int index = 0; index < kHostDisplayRefreshCacheCapacity; index++) {
    if (cache->entries[index].display_id == display_id)
      return cache->entries[index].nominal_refresh_hz;
  }
  return 0;
}

void HostDisplayRefreshCache_Forget(
    HostDisplayRefreshCache *cache, uint32_t display_id) {
  HostDisplayRefreshCacheEntry *entry = FindEntry(cache, display_id);
  if (entry) *entry = (HostDisplayRefreshCacheEntry){0};
}
