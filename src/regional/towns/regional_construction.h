#ifndef AR_REGIONAL_CONSTRUCTION_H
#define AR_REGIONAL_CONSTRUCTION_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Growth-unit price, separate from the construction clock, house-loss reward,
 * support coefficients and report threshold. Capture for the whole batch. */
typedef struct ArRegionalConstructionDescriptor {
  const char *key;
  uint16_t japanese[kArRegionalSource_Count];
} ArRegionalConstructionDescriptor;
const ArRegionalConstructionDescriptor *ArRegionalConstruction_Descriptor(void);
bool ArRegionalConstruction_Resolve(ArRegionalSource source, bool *japanese);
bool ArRegionalConstruction_Price(bool japanese, unsigned civilization, uint16_t *price);

#endif
