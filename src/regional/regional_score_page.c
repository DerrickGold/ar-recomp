#include "regional_score_page.h"

static const ArRegionalScorePageDescriptor kRule = {"master_score_page", {1, 0, 1}};
const ArRegionalScorePageDescriptor *ArRegionalScorePage_Descriptor(void) { return &kRule; }
bool ArRegionalScorePage_Resolve(ArRegionalSource source, bool *enabled) {
  if (!enabled || (unsigned)source >= kArRegionalSource_Count) return false;
  *enabled = kRule.enabled[source] != 0;
  return true;
}
