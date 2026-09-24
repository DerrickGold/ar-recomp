#ifndef AR_REGIONAL_VOLLEY_H
#define AR_REGIONAL_VOLLEY_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Bloodpool firing-statue behavior only. Placement, damage and artwork have
 * independent owners. Capture at complete room/retry initialization. */
typedef struct ArRegionalVolleyDescriptor {
  const char *key;
  uint16_t shots[kArRegionalSource_Count];
} ArRegionalVolleyDescriptor;
const ArRegionalVolleyDescriptor *ArRegionalVolley_Descriptor(void);
bool ArRegionalVolley_Resolve(ArRegionalSource source, bool *double_shot);
#endif
