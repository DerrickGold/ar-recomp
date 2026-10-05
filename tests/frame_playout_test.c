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

/* Variable capture and present completion must not decide which tick is
 * shown. Exercise 90 seconds of NTSC/display drift, including fractional
 * refresh, below-source FPS limits, and above-source refresh holds. */
static void TestNativeTimeline(void) {
  const uint64_t period = 16639263, origin = 1000000000;
  const HostFramePlayout p = HostFramePlayout_Create(period, false, 1750);
  const uint64_t output_periods[] = {33333333, 20000000, 16683333, 16666667,
                                    11111111, 8333333};
  const int64_t jitter[] = {0, 2200000, -500000, 600000};
  for (unsigned r = 0; r < sizeof(output_periods) / sizeof(output_periods[0]); ++r) {
    const uint64_t output_period = output_periods[r];
    HostFrameRefreshClock clock = {0};
    uint64_t endpoint = origin, next = origin + period;
    unsigned holds = 0, drops = 0, produced = 0;
    for (unsigned i = 0; i * output_period < 90000000000ull; ++i) {
      const uint64_t ideal = origin + 3 * period + i * output_period;
      const uint64_t now = (uint64_t)((int64_t)ideal + jitter[i % 4]);
      HostFrameRefreshClock_Advance(&clock, now, output_period);
      const uint64_t sample = HostFrameRefreshClock_TimelineTime(clock, now);
      const uint64_t target = HostFramePlayout_Target(p, sample);
      const uint64_t previous = endpoint;
      for (;;) {
        const uint64_t cost = 3000000 + (next / period * 7919) % 6500000;
        if (next + cost <= now && HostFramePlayout_AcceptsPacket(p, next, target)) {
          endpoint = next; next += period; ++produced;
          continue;
        }
        break;
      }
      assert(!HostFramePlayout_AwaitEndpoint(p, endpoint, target, now, sample));
      assert(endpoint == origin + ((target - origin) / period) * period);
      assert(!HostFramePlayout_AcceptsPacket(p, next, target));
      assert(HostFramePlayout_Phase(p, endpoint, target) < 0);
      if (i) {
        holds += endpoint == previous;
        if (endpoint > previous) drops += (unsigned)((endpoint - previous) / period - 1);
      }
    }
    assert(produced >= 5407 && produced <= 5412);
    if (output_period > period) assert(drops > 0 && holds == 0);
    else assert(holds > 0 && drops == 0);
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

static void TestNativeNeverWaitsForFutureCapture(void) {
  const uint64_t period = 16639263, now = 1000000000;
  const HostFramePlayout p = HostFramePlayout_Create(period, false, 1750);
  /* Recorded Windows failure: a queued content clock predicts a source tick
   * 10.6 ms in the future. Waiting for it caused a missed output opportunity. */
  const uint64_t endpoint = now - 6000000, sample = now + 23200000;
  const uint64_t target = HostFramePlayout_Target(p, sample);
  assert(endpoint + period > now && target >= endpoint + period);
  assert(!HostFramePlayout_AwaitEndpoint(p, endpoint, target, now, sample));
  /* Even a late capture holds the image; it cannot stall the presenter.
   * Once production catches up, coalesce all due captures on the next slot. */
  const uint64_t later = now + 3 * period;
  const uint64_t later_target = HostFramePlayout_Target(p, later);
  uint64_t selected = endpoint;
  unsigned captures = 0;
  while (HostFramePlayout_AcceptsPacket(p, selected + period, later_target)) {
    selected += period;
    ++captures;
  }
  assert(captures >= 2 && selected <= later_target && selected + period > later_target);
  assert(!HostFramePlayout_AwaitEndpoint(p, selected, later_target, later, later));
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

/* The same native source must be chosen at a given refresh regardless of
 * early/late CPU return phases. Include fractional display rates and source
 * phase drift, then a real stall and a display-mode change. */
static void TestFixedRefreshTimeline(void) {
  const uint64_t origin = 1000000000, source_period = 16639263;
  const uint64_t periods[] = {16683333, 11110617, 8333333};
  const int64_t jitter[] = {0, 4400000, 600000, -500000, 2800000, 0};
  const HostFramePlayout policy = HostFramePlayout_Create(source_period, false, 1750);
  for (unsigned rate = 0; rate < 3; ++rate) {
    HostFrameRefreshClock clock = {0};
    const uint64_t period = periods[rate];
    for (unsigned i = 0; i < 12000; ++i) {
      const uint64_t ideal = origin + i * period;
      const uint64_t complete = (uint64_t)((int64_t)ideal + jitter[i % 6]);
      HostFrameRefreshClock_Advance(&clock, complete, period);
      const uint64_t sample = HostFrameRefreshClock_TimelineTime(clock, complete);
      const uint64_t target = HostFramePlayout_Target(policy, sample);
      const uint64_t expected = HostFramePlayout_Target(policy, ideal);
      assert(target / source_period == expected / source_period);
      assert(clock.period_ns == period);
    }
    const uint64_t after_stall = clock.completed_ns + 8 * period;
    HostFrameRefreshClock_Advance(&clock, after_stall, period);
    assert(HostFrameRefreshClock_Next(clock, after_stall) == after_stall + period);
    HostFrameRefreshClock_Advance(&clock, after_stall + 10000000, 10000000);
    assert(HostFrameRefreshClock_Next(clock, after_stall) == after_stall + 20000000);
    HostFrameRefreshClock_Advance(&clock, origin, period);
    assert(HostFrameRefreshClock_Next(clock, origin) == origin + period);
    HostFrameRefreshClock_Advance(&clock, origin + period, 0);
    assert(HostFrameRefreshClock_Next(clock, origin + period) == origin + period);
    /* A short disruption can drain queued output without losing its order.
     * Do not replace the next frame's content time with the late CPU time. */
    HostFrameRefreshClock_Advance(&clock, origin, period);
    const uint64_t late = origin + 2 * period + period / 2;
    HostFrameRefreshClock_Advance(&clock, late, period);
    assert(HostFrameRefreshClock_TimelineTime(clock, late) == origin + period);
    HostFrameRefreshClock_Advance(&clock, late + 1000000, period);
    assert(HostFrameRefreshClock_TimelineTime(clock, late + 1000000) == origin + 2 * period);
    /* Initial swapchain submissions need not block. They must not move the
     * content clock several frames into the future before Vsync takes over. */
    clock = (HostFrameRefreshClock){0};
    for (unsigned i = 0; i < 3; ++i) {
      const uint64_t now = origin + i * 1000000;
      HostFrameRefreshClock_Advance(&clock, now, period);
      assert(HostFrameRefreshClock_TimelineTime(clock, now) == now);
    }
    const uint64_t queued = clock.phase_ns;
    for (unsigned i = 1; i < 3000; ++i) {
      const uint64_t now = queued + i * period;
      HostFrameRefreshClock_Advance(&clock, now, period);
      assert(HostFrameRefreshClock_TimelineTime(clock, now) == now);
    }
    /* Sustained rendering below refresh must not accumulate unbounded lag
     * and eventually slow the producer behind a full packet queue. */
    clock = (HostFrameRefreshClock){0};
    for (unsigned i = 0; i < 3000; ++i) {
      const uint64_t now = origin + i * (period + period / 4);
      HostFrameRefreshClock_Advance(&clock, now, period);
      const uint64_t sample = HostFrameRefreshClock_TimelineTime(clock, now);
      assert(sample > now || now - sample <= 3 * period);
    }
  }
}

int main(void) {
  TestFixedRefreshTimeline();
  TestRefreshPhase();
  TestNativeTimeline();
  TestNativeNeverWaitsForFutureCapture();
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
