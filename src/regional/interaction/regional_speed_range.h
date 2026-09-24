#ifndef AR_REGIONAL_SPEED_RANGE_H
#define AR_REGIONAL_SPEED_RANGE_H

#include "regional/regional_costs.h"

typedef struct ArRegionalSpeedRangeDescriptor {
  const char *key;
  uint16_t maximum[kArRegionalSource_Count];
} ArRegionalSpeedRangeDescriptor;

const ArRegionalSpeedRangeDescriptor *ArRegionalSpeedRange_Descriptor(void);
bool ArRegionalSpeedRange_Resolve(ArRegionalSource source, uint16_t *maximum);

#endif
