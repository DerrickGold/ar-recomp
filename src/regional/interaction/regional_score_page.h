#ifndef AR_REGIONAL_SCORE_PAGE_H
#define AR_REGIONAL_SCORE_PAGE_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalScorePageDescriptor {
  const char *key;
  uint16_t enabled[kArRegionalSource_Count];
} ArRegionalScorePageDescriptor;
const ArRegionalScorePageDescriptor *ArRegionalScorePage_Descriptor(void);
bool ArRegionalScorePage_Resolve(ArRegionalSource source, bool *enabled);

#endif
