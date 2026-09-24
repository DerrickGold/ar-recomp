#ifndef AR_REGIONAL_LIVES_DISPLAY_H
#define AR_REGIONAL_LIVES_DISPLAY_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalLivesDisplayDescriptor {
  const char *key;
  uint16_t zero_based[kArRegionalSource_Count];
} ArRegionalLivesDisplayDescriptor;
const ArRegionalLivesDisplayDescriptor *ArRegionalLivesDisplay_Descriptor(void);
bool ArRegionalLivesDisplay_Resolve(ArRegionalSource source, bool *zero_based);

#endif
