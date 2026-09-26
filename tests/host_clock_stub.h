#ifndef AR_HOST_CLOCK_STUB_H
#define AR_HOST_CLOCK_STUB_H

#include "host/host_clock.h"

/* Test-owned monotonic clock. UI input, expiry and drawing see the same time. */
void HostClockStub_SetMilliseconds(uint64_t milliseconds);

#endif
