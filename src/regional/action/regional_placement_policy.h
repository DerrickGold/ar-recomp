#ifndef AR_REGIONAL_PLACEMENT_POLICY_H
#define AR_REGIONAL_PLACEMENT_POLICY_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalPlacementPolicy {
  ArRegionalSource enemies, pickups;
} ArRegionalPlacementPolicy;
typedef struct ArRegionalPlacementDescriptor {
  const char *key;
  uint16_t profile[kArRegionalSource_Count];
} ArRegionalPlacementDescriptor;
enum { kArRegionalPlacement_Enemies, kArRegionalPlacement_Pickups, kArRegionalPlacement_Count };
const ArRegionalPlacementDescriptor *ArRegionalPlacements_Descriptor(unsigned rule);
bool ArRegionalPlacements_Valid(const ArRegionalPlacementPolicy *policy);
#endif
