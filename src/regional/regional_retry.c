#include "regional_retry.h"

static const ArRegionalRetryDescriptor kRule = {"retry_clear_score", {0, 1, 0}};

const ArRegionalRetryDescriptor *ArRegionalRetry_Descriptor(void) { return &kRule; }

bool ArRegionalRetry_Resolve(ArRegionalSource source, bool *clear_score) {
  if (!clear_score || (unsigned)source >= kArRegionalSource_Count) return false;
  *clear_score = kRule.clear_score[source] != 0;
  return true;
}
