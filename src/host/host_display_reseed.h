#ifndef AR_HOST_DISPLAY_RESEED_H
#define AR_HOST_DISPLAY_RESEED_H

#include <stdbool.h>
#include <stdint.h>

/* Main-thread reset scope. Only invalidation inside a completed, eligible
 * camera reset can request one replacement capture. Any later invalidation
 * or rejected owner-side guard expires the request. */
typedef struct HostDisplayReseedState {
  uint64_t serial;
  bool active, eligible, invalidated, pending;
} HostDisplayReseedState;

static inline uint64_t HostDisplayReseed_Begin(
    HostDisplayReseedState *state, bool eligible) {
  if (!++state->serial) ++state->serial;
  state->active = true;
  state->eligible = eligible;
  state->invalidated = state->pending = false;
  return state->serial;
}

static inline void HostDisplayReseed_Invalidate(HostDisplayReseedState *state) {
  state->pending = false;
  if (state->active) state->invalidated = true;
}

static inline void HostDisplayReseed_End(
    HostDisplayReseedState *state, uint64_t token, bool eligible) {
  state->pending = state->active && token == state->serial &&
      state->eligible && state->invalidated && eligible;
  state->active = false;
}

static inline bool HostDisplayReseed_Take(
    HostDisplayReseedState *state, bool eligible) {
  const bool pending = state->pending && eligible;
  state->pending = false;
  return pending;
}

#endif
