#include "host/frame_playout.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>

/* A 60 Hz source completes after 12 ms; a 90 Hz presenter consumes it.
 * Check temporal continuity over many phase alignments, rather than just
 * reproducing the phase formula for a single endpoint. */
static void TestCadence(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  HostFramePlayout p = HostFramePlayout_Create(period, true,
      kHostFramePlayoutDefaultDelayPermille);
  uint64_t endpoint = origin, next = endpoint + period;
  double last = 0;
  unsigned uploads = 0;
  for (unsigned i = 1; i < 900; ++i) {
    uint64_t now = origin + 3 * period + (uint64_t)i * 11111111;
    uint64_t target = HostFramePlayout_Target(p, now);
    while (next + 12000000 <= now &&
           HostFramePlayout_NeedsEndpoint(p, endpoint, target)) {
      endpoint = next; next += period; ++uploads;
    }
    float phase = HostFramePlayout_Phase(p, endpoint, target);
    double timeline = (double)(endpoint - period) + phase * period;
    assert(phase >= 0 && phase < 1 && timeline > last);
    assert(fabs(timeline - (double)target) < 2000);
    last = timeline;
  }
  assert(uploads > 599 && uploads < 610);
  /* An overdue source is held, never extrapolated. */
  assert(HostFramePlayout_Phase(p, endpoint, endpoint + period) == .9999f);
}

/* Early preparation must select the same pair at the draw deadline, rather
 * than advancing the displayed timeline or adding another buffered frame. */
static void TestPrepareAhead(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  const HostFramePlayout p = HostFramePlayout_Create(period, true, 1750);
  uint64_t early_endpoint = origin, early_next = origin + period;
  uint64_t ordinary_endpoint = origin, ordinary_next = origin + period;
  unsigned early_uploads = 0;
  for (unsigned i = 1; i < 900; ++i) {
    const uint64_t deadline = origin + 3 * period + (uint64_t)i * 11111111;
    const uint64_t target = HostFramePlayout_Target(p, deadline);
    for (uint64_t now = deadline - 10000000; now <= deadline; now += 1000000) {
      const uint64_t preparation_target = HostFramePlayout_Target(p,
          HostFramePlayout_PreparationTime(p, now, deadline));
      while (early_next + 12000000 <= now &&
          HostFramePlayout_NeedsEndpoint(p, early_endpoint, preparation_target)) {
        early_endpoint = early_next; early_next += period;
        if (now < deadline) ++early_uploads;
      }
    }
    while (ordinary_next + 12000000 <= deadline &&
        HostFramePlayout_NeedsEndpoint(p, ordinary_endpoint, target)) {
      ordinary_endpoint = ordinary_next; ordinary_next += period;
    }
    assert(early_endpoint == ordinary_endpoint);
    assert(HostFramePlayout_Phase(p, early_endpoint, target) ==
        HostFramePlayout_Phase(p, ordinary_endpoint, target));
    assert(HostFramePlayout_PreparationTime(p, deadline + 1, deadline) == deadline + 1);
    assert(HostFramePlayout_PreparationTime(p, deadline, 0) == deadline);
  }
  assert(early_uploads > 500);
  const HostFramePlayout raw = HostFramePlayout_Create(period, false, 1750);
  assert(HostFramePlayout_PreparationTime(raw, origin, origin + period) == origin + period);
}

static void TestEarlyDrawWaitsForItsPair(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  const HostFramePlayout p = HostFramePlayout_Create(period, true, 1750);
  uint64_t endpoint = origin, next = origin + period;
  unsigned delayed_draws = 0;
  double last_timeline = 0;
  for (unsigned i = 1; i < 900; ++i) {
    const uint64_t deadline = origin + 3 * period + (uint64_t)i * 11111111;
    const uint64_t target = HostFramePlayout_Target(p, deadline);
    uint64_t draw = deadline - 5500000;
    bool waited = false;
    for (;;) {
      /* Capture and upload finish after 12 ms, across all relative phases of
       * the 60 Hz producer and 90 Hz presenter. */
      while (next + 12000000 <= draw &&
          HostFramePlayout_NeedsEndpoint(p, endpoint, target)) {
        endpoint = next;
        next += period;
      }
      if (!HostFramePlayout_AwaitEndpoint(p, endpoint, target, draw, deadline)) break;
      waited = true;
      draw += 200000;
    }
    delayed_draws += waited;
    const float phase = HostFramePlayout_Phase(p, endpoint, target);
    const double timeline = (double)(endpoint - period) + phase * period;
    assert(draw <= deadline);
    assert(phase > 0 && phase < .9999f);
    assert(fabs(timeline - (double)target) < 2000);
    assert(timeline > last_timeline);
    last_timeline = timeline;
  }
  assert(delayed_draws > 100);
  assert(!HostFramePlayout_AwaitEndpoint(p, 1, 2, 100, 100));
  assert(!HostFramePlayout_AwaitEndpoint(p, 1, 2, 101, 100));
  const HostFramePlayout raw = HostFramePlayout_Create(period, false, 1750);
  assert(!HostFramePlayout_AwaitEndpoint(raw, 0, 2, 90, 100));
}

/* Variable completion time must not decide which source tick is shown.
 * Include full NTSC/display phase drift, 30 Hz drops and 90/120 Hz holds. */
static void TestNativeTimeline(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  const HostFramePlayout p = HostFramePlayout_Create(period, false, 1750);
  const unsigned rates[] = {30, 60, 90, 120};
  for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    uint64_t endpoint = origin, next = origin + period;
    for (unsigned i = 1; i < rates[r] * 90; ++i) {
      const uint64_t sample = origin + 3 * period + (uint64_t)i * 1000000000 / rates[r];
      const uint64_t target = HostFramePlayout_Target(p, sample);
      uint64_t now = sample - 8000000;
      for (;;) {
        const uint64_t cost = 3000000 + (next / period * 7919) % 6500000;
        if (next + cost <= now && HostFramePlayout_AcceptsPacket(p, next, target)) {
          endpoint = next; next += period;
          continue;
        }
        if (!HostFramePlayout_AwaitEndpoint(p, endpoint, target, now, sample)) break;
        now += 100000;
      }
      assert(endpoint == origin + ((target - origin) / period) * period);
      assert(!HostFramePlayout_AcceptsPacket(p, next, target));
      assert(HostFramePlayout_Phase(p, endpoint, target) < 0);
    }
  }
  assert(!HostFramePlayout_AwaitEndpoint(p, origin, origin + period, origin + 2 * period, origin + 2 * period));
  HostFramePlayout latest = p; latest.delay_ns = 0;
  assert(HostFramePlayout_AcceptsPacket(latest, origin + period, origin));
  HostFrameRefreshClock clock = {0};
  assert(HostFrameRefreshClock_Next(clock, origin) == origin);
  HostFrameRefreshClock_Observe(&clock, origin, 16666667);
  HostFrameRefreshClock_Observe(&clock, origin + 50000001, 16666667);
  assert(clock.period_ns == 16666667);
  assert(HostFrameRefreshClock_Next(clock, origin + 50000001) == origin + 66666668);
  HostFrameRefreshClock_Observe(&clock, origin + 66666668, 0);
  assert(!clock.period_ns && !clock.completed_ns);
}

static void TestRefreshPhase(void) {
  const uint64_t origin = 1000000000, period = 16666667;
  HostFrameRefreshClock clock = {0};
  uint64_t filtered_error = 0, raw_error = 0;
  for (unsigned i = 0; i < 6000; ++i) {
    const uint64_t ideal = origin + i * period;
    const int64_t jitter = (i % 4 == 0 ? 1200000 : i % 4 == 2 ? -1200000 : 0);
    const uint64_t complete = (uint64_t)((int64_t)ideal + jitter);
    HostFrameRefreshClock_Observe(&clock, complete, period);
    if (i > 100) {
      const uint64_t prediction = HostFrameRefreshClock_Next(clock, complete);
      const uint64_t target = ideal + period;
      filtered_error += prediction > target ? prediction - target : target - prediction;
      raw_error += jitter < 0 ? -jitter : jitter;
    }
  }
  assert(filtered_error < raw_error / 2);
  uint64_t stall = clock.completed_ns + 8 * period;
  HostFrameRefreshClock_Observe(&clock, stall, period);
  assert(clock.phase_ns == stall);
  HostFrameRefreshClock_Observe(&clock, stall + 11111111, 11111111);
  assert(clock.period_ns == 11111111 && clock.phase_ns == stall + 11111111);
  HostFrameRefreshClock_Observe(&clock, origin, period);
  assert(clock.phase_ns == origin);
}

static void TestSynchronousRelease(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  for (unsigned rate = 60; rate <= 120; rate += 30) {
    HostFrameTickSchedule clock = {0};
    uint64_t source = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < rate * 90; ++i) {
      uint64_t target = origin + (uint64_t)i * 1000000000 / rate;
      total += HostFrameTickSchedule_Release(&clock, target, period, 3, &source);
      assert(source <= target && source + period > target);
      assert(clock.epoch == 1);
    }
    assert(total >= 5407 && total <= 5409);
    const uint64_t next = clock.next_ns;
    assert(HostFrameTickSchedule_Release(&clock, source - 1000, period, 3, &source) == 0);
    assert(clock.next_ns == next);
    assert(HostFrameTickSchedule_Release(&clock, next + period * 30, period, 3, &source) == 3);
    assert(clock.epoch == 2 && source == next + period * 30);
  }
}

int main(void) {
  TestRefreshPhase();
  TestSynchronousRelease();
  TestNativeTimeline();
  TestCadence();
  TestPrepareAhead();
  TestEarlyDrawWaitsForItsPair();
  HostFramePlayout p = HostFramePlayout_Create(16000000, false, 3000);
  assert(p.delay_ns == 12000000 && HostFramePlayout_Target(p, 123456) == 0);
  assert(HostFramePlayout_NeedsEndpoint(p, 20000000, 123456));
  assert(HostFramePlayout_Phase(p, 20000000, 123456) < 0);
  p = HostFramePlayout_Create(16000000, true, 1750);
  assert(p.delay_ns == 28000000);
  assert(!HostFramePlayout_NeedsEndpoint(p, 48000000, 44000000));
  assert(HostFramePlayout_Phase(p, 48000000, 44000000) == .75f);
  assert(HostFramePlayout_Target(p, 1000) == 0);
  assert(HostFramePlayout_Phase(p, 48000000, 0) == 0);
  assert(HostFramePlayout_Create(0, true, 2000).interval_ns == 0);
  assert(HostFramePlayout_Create(UINT64_MAX, true, 2000).interval_ns == 0);
  assert(HostFramePlayout_Create(16000000, true, 0).delay_ns == 16000000);
  assert(HostFramePlayout_Create(16000000, true, 4000).delay_ns == 48000000);
  puts("frame_playout_test: PASS");
  return 0;
}
