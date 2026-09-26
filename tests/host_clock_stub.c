#include "host_clock_stub.h"

static uint64_t s_milliseconds = 1000;

uint64_t HostClock_Milliseconds(void) { return s_milliseconds; }
uint64_t HostClock_Nanoseconds(void) { return s_milliseconds * 1000000; }
void HostClockStub_SetMilliseconds(uint64_t milliseconds) { s_milliseconds = milliseconds; }
