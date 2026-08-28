#ifndef AR_HOST_CLOCK_H
#define AR_HOST_CLOCK_H

#include <stdint.h>

/* Monotonic host time for presentation, input, and persistence policy. The
 * platform supplies the clock; game-side code must not depend on its windowing
 * or rendering API merely to debounce work. */
uint64_t HostClock_Milliseconds(void);
uint64_t HostClock_Nanoseconds(void);

#endif /* AR_HOST_CLOCK_H */
