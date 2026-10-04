#ifndef AR_FRAME_PLAYOUT_H
#define AR_FRAME_PLAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Immutable for a production batch. Host settings changes require ownership
 * back from the worker before replacing this policy. Native playback uses a
 * shorter schedule-based delay so capture-time jitter cannot select the tick. */
typedef struct HostFramePlayout {
  uint64_t interval_ns;
  uint64_t delay_ns;
  bool interpolate;
} HostFramePlayout;

/* Fixed source schedule for synchronous native rendering. Presentation may
 * release several ticks, but never changes their interval. A genuine stall
 * retains the loop's bounded catch-up policy and starts a new trace epoch. */
typedef struct HostFrameTickSchedule {
  uint64_t next_ns, interval_ns, epoch;
} HostFrameTickSchedule;

static inline unsigned HostFrameTickSchedule_Release(HostFrameTickSchedule *clock,
    uint64_t target_ns, uint64_t interval_ns, unsigned maximum, uint64_t *source_ns) {
  if (!interval_ns || !maximum) return 0;
  if (!clock->next_ns || clock->interval_ns != interval_ns) {
    clock->next_ns = target_ns;
    clock->interval_ns = interval_ns;
    ++clock->epoch;
  } else if (target_ns > clock->next_ns &&
             (target_ns - clock->next_ns) / interval_ns >= maximum) {
    clock->next_ns = target_ns - (maximum - 1) * interval_ns;
    ++clock->epoch;
  }
  unsigned ticks = 0;
  while (clock->next_ns <= target_ns && ticks < maximum) {
    *source_ns = clock->next_ns;
    clock->next_ns += interval_ns;
    ++ticks;
  }
  return ticks;
}

/* Present return is an estimate of refresh, not a physical scanout timestamp.
 * Filter both period and phase on single-refresh samples. Present-return
 * jitter must not move the source-selection boundary at the NTSC/display beat.
 * Stalls and discontinuities resynchronize instead of teaching a slower rate. */
typedef struct HostFrameRefreshClock {
  uint64_t completed_ns, period_ns, phase_ns, nominal_ns;
} HostFrameRefreshClock;

static inline void HostFrameRefreshClock_Observe(
    HostFrameRefreshClock *clock, uint64_t completed_ns, uint64_t nominal_ns) {
  if (!nominal_ns) { *clock = (HostFrameRefreshClock){0}; return; }
  if (clock->nominal_ns != nominal_ns || completed_ns <= clock->completed_ns)
    *clock = (HostFrameRefreshClock){.nominal_ns = nominal_ns};
  if (!clock->period_ns) clock->period_ns = nominal_ns;
  if (completed_ns > clock->completed_ns && clock->completed_ns) {
    const uint64_t elapsed = completed_ns - clock->completed_ns;
    if (elapsed > nominal_ns * 9 / 10 && elapsed < nominal_ns * 11 / 10)
      clock->period_ns = (clock->period_ns * 7 + elapsed) / 8;
  }
  const uint64_t predicted = clock->phase_ns + clock->period_ns;
  const uint64_t error = completed_ns > predicted
      ? completed_ns - predicted : predicted - completed_ns;
  if (!clock->phase_ns || error > clock->period_ns / 2)
    clock->phase_ns = completed_ns;
  else
    clock->phase_ns = completed_ns > predicted
        ? predicted + error / 8 : predicted - error / 8;
  clock->completed_ns = completed_ns;
}

static inline uint64_t HostFrameRefreshClock_Next(
    HostFrameRefreshClock clock, uint64_t now_ns) {
  if (!clock.completed_ns || !clock.period_ns) return now_ns;
  const uint64_t next = clock.phase_ns + clock.period_ns;
  return next > now_ns ? next : now_ns;
}

/* Interpolation delay was measured across forest, cave, castle, lake and lava
 * scenes on Deck. Native selection only needs one due endpoint, so its delay
 * is shorter. Keep both policies separate from pipeline enablement. */
enum {
  kHostFramePlayoutDefaultDelayPermille = 1750,
  kHostFramePlayoutNativeDelayPermille = 750,
};

static inline HostFramePlayout HostFramePlayout_Create(
    uint64_t interval_ns, bool interpolate, unsigned delay_permille) {
  if (delay_permille < 1000) delay_permille = 1000;
  if (delay_permille > 3000) delay_permille = 3000;
  /* Source periods are small, but reject overflow rather than wrapping a
   * malformed clock into a future target. */
  if (!interval_ns || interval_ns > UINT64_MAX / 3)
    return (HostFramePlayout){0};
  if (!interpolate) delay_permille = kHostFramePlayoutNativeDelayPermille;
  const uint64_t delay_ns = (interval_ns / 1000) * delay_permille +
      (interval_ns % 1000) * delay_permille / 1000;
  return (HostFramePlayout){interval_ns, delay_ns, interpolate};
}

static inline uint64_t HostFramePlayout_Target(
    HostFramePlayout policy, uint64_t now_ns) {
  return now_ns > policy.delay_ns ? now_ns - policy.delay_ns : 0;
}

/* Prepare the pair needed by the next scheduled presentation while its GPU
 * work can overlap idle time. This does not move the presentation clock or
 * increase playback delay. Native playback keeps future endpoints queued. */
static inline uint64_t HostFramePlayout_PreparationTime(
    HostFramePlayout policy, uint64_t now_ns, uint64_t deadline_ns) {
  return policy.delay_ns && deadline_ns > now_ns ? deadline_ns : now_ns;
}

static inline bool HostFramePlayout_AcceptsPacket(
    HostFramePlayout policy, uint64_t source_ns, uint64_t target_ns) {
  return policy.interpolate || !policy.delay_ns || source_ns <= target_ns;
}

static inline bool HostFramePlayout_NeedsEndpoint(
    HostFramePlayout policy, uint64_t endpoint_ns, uint64_t target_ns) {
  return !policy.interpolate || !endpoint_ns || endpoint_ns < target_ns;
}

/* Early drawing is optional; publishing a stale pair for a future sample is
 * not. Wait only while there is still time before that planned sample. A late
 * source at the actual deadline retains the ordinary bounded hold policy. */
static inline bool HostFramePlayout_AwaitEndpoint(
    HostFramePlayout policy, uint64_t endpoint_ns, uint64_t target_ns,
    uint64_t now_ns, uint64_t sample_ns) {
  if (policy.interpolate)
    return sample_ns > now_ns &&
        HostFramePlayout_NeedsEndpoint(policy, endpoint_ns, target_ns);
  /* Reserve drawing time before an estimated refresh. This bounded wait is
   * only for a tick already due on the source timeline, never a future tick. */
  return policy.delay_ns && endpoint_ns && target_ns > endpoint_ns &&
      target_ns - endpoint_ns >= policy.interval_ns &&
      sample_ns > now_ns && sample_ns - now_ns > 2000000;
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
