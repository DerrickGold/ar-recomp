#include "regional/action/regional_placement_policy.h"
#include <stddef.h>
static const ArRegionalPlacementDescriptor kDescriptors[] = {
  {"enemy_placements",{0,1,2}},
  {"pickup_placements",{0,1,2}},
};
const ArRegionalPlacementDescriptor *ArRegionalPlacements_Descriptor(unsigned rule) {
  return rule < kArRegionalPlacement_Count ? &kDescriptors[rule] : NULL;
}
bool ArRegionalPlacements_Valid(const ArRegionalPlacementPolicy *policy) {
  return policy && (unsigned)policy->enemies < kArRegionalSource_Count &&
      (unsigned)policy->pickups < kArRegionalSource_Count;
}
