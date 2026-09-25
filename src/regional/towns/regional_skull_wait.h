#ifndef AR_REGIONAL_SKULL_WAIT_H
#define AR_REGIONAL_SKULL_WAIT_H
/* RegionalSkullWait: the frames to wait after a skull effect, per region.
 * Phase: pure.
 * Tests: tests/actraiser_platform_skull_test.c */

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalSkullWaitDescriptor {
  const char *key;
  uint16_t frames[kArRegionalSource_Count];
} ArRegionalSkullWaitDescriptor;
const ArRegionalSkullWaitDescriptor *ArRegionalSkullWait_Descriptor(void);
bool ArRegionalSkullWait_Resolve(ArRegionalSource source, uint16_t *frames);

#endif
