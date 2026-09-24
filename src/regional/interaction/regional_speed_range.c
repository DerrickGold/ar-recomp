#include "regional/interaction/regional_speed_range.h"

static const ArRegionalSpeedRangeDescriptor kRule = {"message_speed_range", {9, 7, 9}};
const ArRegionalSpeedRangeDescriptor *ArRegionalSpeedRange_Descriptor(void) { return &kRule; }
bool ArRegionalSpeedRange_Resolve(ArRegionalSource source, uint16_t *maximum) {
  if (!maximum || (unsigned)source >= kArRegionalSource_Count) return false;
  *maximum = kRule.maximum[source];
  return true;
}
