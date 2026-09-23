#ifndef AR_REGIONAL_ARRIVAL_H
#define AR_REGIONAL_ARRIVAL_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* One coherent final-island transition. Artwork and score settlement have
 * separate owners. Japanese flow returns to town and unlocks at the Palace;
 * Western flow unlocks at departure and runs the native world-map reveal. */
typedef struct ArRegionalArrivalDescriptor {
  const char *key;
  uint16_t japanese[kArRegionalSource_Count];
} ArRegionalArrivalDescriptor;
const ArRegionalArrivalDescriptor *ArRegionalArrival_Descriptor(void);
bool ArRegionalArrival_Resolve(ArRegionalSource source,bool *japanese);
#endif
