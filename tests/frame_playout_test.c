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

int main(void) {
  TestCadence();
  HostFramePlayout p = HostFramePlayout_Create(16000000, false, 3000);
  assert(p.delay_ns == 0 && HostFramePlayout_Target(p, 123456) == 123456);
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
