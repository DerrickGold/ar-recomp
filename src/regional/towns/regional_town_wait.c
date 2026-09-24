#include "regional/towns/regional_town_wait.h"

/* US/EU/DE/FR $03:AA9E; JP $03:A866. The countdown body is identical.
 * docs/regional-differences-technical.md#simulation-rules-in-code */
static const ArRegionalTownWaitDescriptor kRule = {"town_wait_reload", {1, 150, 1}};

const ArRegionalTownWaitDescriptor *ArRegionalTownWait_Descriptor(void) { return &kRule; }

bool ArRegionalTownWait_Resolve(ArRegionalSource source, uint16_t *updates) {
  if (!updates || (unsigned)source >= kArRegionalSource_Count) return false;
  *updates = kRule.updates[source];
  return true;
}
