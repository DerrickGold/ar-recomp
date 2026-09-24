#ifndef AR_REGIONAL_MOSAIC_H
#define AR_REGIONAL_MOSAIC_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct ArRegionalMosaicDescriptor {
  const char *key;
  uint16_t profile[kArRegionalSource_Count];
} ArRegionalMosaicDescriptor;
const ArRegionalMosaicDescriptor *ArRegionalMosaic_Descriptor(void);
bool ArRegionalMosaic_Resolve(ArRegionalSource source,uint8_t *pattern);
#endif
