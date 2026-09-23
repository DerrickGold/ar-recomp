#ifndef AR_REGIONAL_FISHING_H
#define AR_REGIONAL_FISHING_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalFishingDescriptor {
  const char *key;
  uint16_t updates[kArRegionalSource_Count];
} ArRegionalFishingDescriptor;
const ArRegionalFishingDescriptor *ArRegionalFishing_Descriptor(void);
bool ArRegionalFishing_Resolve(ArRegionalSource source, uint16_t *updates);
#endif
