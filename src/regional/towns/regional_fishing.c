#include "regional/towns/regional_fishing.h"

/* Fillmore event10, not Marahna's separate counter or Northwall's lake search.
 * US/PAL $03:E888, JP $03:E37C. Counts eligible callback visits, not frames. */
static const ArRegionalFishingDescriptor kRule = {"fillmore_fishing_target", {255, 128, 255}};
const ArRegionalFishingDescriptor *ArRegionalFishing_Descriptor(void) { return &kRule; }
bool ArRegionalFishing_Resolve(ArRegionalSource source, uint16_t *updates) {
  if (!updates || (unsigned)source >= kArRegionalSource_Count) return false;
  *updates = kRule.updates[source];
  return true;
}
