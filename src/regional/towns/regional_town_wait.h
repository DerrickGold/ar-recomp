#ifndef AR_REGIONAL_TOWN_WAIT_H
#define AR_REGIONAL_TOWN_WAIT_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalTownWaitDescriptor {
  const char *key;
  uint16_t updates[kArRegionalSource_Count];
} ArRegionalTownWaitDescriptor;

/* State-3 service calls, not frames/seconds or the master development cycle. */
const ArRegionalTownWaitDescriptor *ArRegionalTownWait_Descriptor(void);
bool ArRegionalTownWait_Resolve(ArRegionalSource source, uint16_t *updates);

#endif
