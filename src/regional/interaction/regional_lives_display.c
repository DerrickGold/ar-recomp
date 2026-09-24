#include "regional/interaction/regional_lives_display.h"

static const ArRegionalLivesDisplayDescriptor kRule = {"lives_zero_based", {0, 1, 0}};
const ArRegionalLivesDisplayDescriptor *ArRegionalLivesDisplay_Descriptor(void) { return &kRule; }
bool ArRegionalLivesDisplay_Resolve(ArRegionalSource source, bool *zero_based) {
  if (!zero_based || (unsigned)source >= kArRegionalSource_Count) return false;
  *zero_based = kRule.zero_based[source] != 0;
  return true;
}
