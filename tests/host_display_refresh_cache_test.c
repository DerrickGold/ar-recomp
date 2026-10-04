#include <stdio.h>

#include "host/host_display_refresh_cache.h"

static int s_failures;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failures++; \
  } \
} while (0)

int main(void) {
  HostDisplayRefreshCache cache = {0};
  CHECK(HostDisplayRefreshCache_Get(&cache, 7) == 0);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 7) == 0);

  HostDisplayRefreshCache_Remember(&cache, 7, 120, 8333333);
  CHECK(HostDisplayRefreshCache_Get(&cache, 7) == 120);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 7) == 8333333);
  HostDisplayRefreshCache_Remember(&cache, 7, 90, 11110617);
  CHECK(HostDisplayRefreshCache_Get(&cache, 7) == 90);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 7) == 11110617);

  /* Transient SDL failures and unspecified VRR modes must preserve the last
   * valid value for this connected display. */
  HostDisplayRefreshCache_Remember(&cache, 7, 0, 0);
  HostDisplayRefreshCache_Remember(&cache, 7, -1, 16666667);
  HostDisplayRefreshCache_Remember(&cache, 7, 60, 0);
  HostDisplayRefreshCache_Remember(&cache, 0, 240, 4166667);
  CHECK(HostDisplayRefreshCache_Get(&cache, 7) == 90);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 7) == 11110617);
  CHECK(HostDisplayRefreshCache_Get(&cache, 0) == 0);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 0) == 0);

  /* A fractional 59.94 Hz display has the same rounded rate as 60 Hz, but
   * selecting sources at the rounded period would drift by 1 ms/second. */
  HostDisplayRefreshCache_Remember(&cache, 9, 60, 16683333);
  CHECK(HostDisplayRefreshCache_Get(&cache, 9) == 60);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 9) == 16683333);
  HostDisplayRefreshCache_Forget(&cache, 7);
  CHECK(HostDisplayRefreshCache_Get(&cache, 7) == 0);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 7) == 0);
  CHECK(HostDisplayRefreshCache_Get(&cache, 9) == 60);
  CHECK(HostDisplayRefreshCache_IntervalNs(&cache, 9) == 16683333);

  /* Capacity overflow evicts one old session entry but always publishes the
   * new display and never creates an invalid zero-ID entry. */
  for (uint32_t id = 100;
       id < 100 + kHostDisplayRefreshCacheCapacity + 1; id++)
    HostDisplayRefreshCache_Remember(&cache, id, (int)id, 1000000000 / id);
  CHECK(HostDisplayRefreshCache_Get(
            &cache, 100 + kHostDisplayRefreshCacheCapacity) ==
        100 + kHostDisplayRefreshCacheCapacity);
  CHECK(HostDisplayRefreshCache_IntervalNs(
            &cache, 100 + kHostDisplayRefreshCacheCapacity) ==
        1000000000 / (100 + kHostDisplayRefreshCacheCapacity));

  if (s_failures) return 1;
  puts("host display refresh cache tests: pass");
  return 0;
}
