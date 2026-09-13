#ifndef PRESENT_WORLD_NAV_TEST_H
#define PRESENT_WORLD_NAV_TEST_H
#if AR_WORLD_NAV_CACHE_TESTING
#include <stdbool.h>
#include <stddef.h>
/* Fault-injection seam, excluded from the game/builder. Exercise the real
 * retained-view policy with tiny budgets instead of constructing a huge ROM. */
typedef struct WorldNavigationCacheTestState {
  bool model_rejected, receivers_rejected;
  size_t model_vertices, receivers;
} WorldNavigationCacheTestState;
void PresentWorldNav_TestCacheBudgets(size_t model_bytes, size_t receiver_bytes);
WorldNavigationCacheTestState PresentWorldNav_TestCacheState(void);
#endif
#endif
