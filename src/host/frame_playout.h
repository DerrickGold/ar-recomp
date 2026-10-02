#ifndef AR_FRAME_PLAYOUT_H
#define AR_FRAME_PLAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Immutable for a production batch. Host settings changes require ownership
 * back from the worker before replacing this policy. Delay is meaningful only
 * for interpolation; ordinary playback consumes the latest completed frame. */
typedef struct HostFramePlayout {
  uint64_t interval_ns;
  uint64_t delay_ns;
  bool interpolate;
} HostFramePlayout;

/* Measured across forest, cave, castle, lake and lava scenes on Deck. Keep
 * playback delay separate from pipeline enablement. */
enum { kHostFramePlayoutDefaultDelayPermille = 1750 };

static inline HostFramePlayout HostFramePlayout_Create(
    uint64_t interval_ns, bool interpolate, unsigned delay_permille) {
  if (delay_permille < 1000) delay_permille = 1000;
  if (delay_permille > 3000) delay_permille = 3000;
  /* Source periods are small, but reject overflow rather than wrapping a
   * malformed clock into a future target. */
  if (!interval_ns || interval_ns > UINT64_MAX / 3)
    return (HostFramePlayout){0};
  const uint64_t delay_ns = (interval_ns / 1000) * delay_permille +
      (interval_ns % 1000) * delay_permille / 1000;
  return (HostFramePlayout){interval_ns, interpolate ? delay_ns : 0, interpolate};
}

static inline uint64_t HostFramePlayout_Target(
    HostFramePlayout policy, uint64_t now_ns) {
  return now_ns > policy.delay_ns ? now_ns - policy.delay_ns : 0;
}

static inline bool HostFramePlayout_NeedsEndpoint(
    HostFramePlayout policy, uint64_t endpoint_ns, uint64_t target_ns) {
  return !policy.interpolate || !endpoint_ns || endpoint_ns < target_ns;
}

static inline float HostFramePlayout_Phase(
    HostFramePlayout policy, uint64_t endpoint_ns, uint64_t target_ns) {
  if (!policy.interpolate) return -1.0f; /* No frame generation. */
  if (!endpoint_ns || !policy.interval_ns) return 0.9999f;
  const uint64_t previous_ns = endpoint_ns > policy.interval_ns
      ? endpoint_ns - policy.interval_ns : 0;
  if (target_ns <= previous_ns) return 0.0f;
  const double phase = (double)(target_ns - previous_ns) / policy.interval_ns;
  return phase < .9999 ? (float)phase : .9999f;
}

#endif
