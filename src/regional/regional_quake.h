#ifndef AR_REGIONAL_QUAKE_H
#define AR_REGIONAL_QUAKE_H

#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Structure classes whose earthquake selectors differ. Bridges and class6
 * already share native behavior and are deliberately not duplicate switches. */
typedef enum ArRegionalQuakeRule {
  kArRegionalQuake_Houses, kArRegionalQuake_Fields,
  kArRegionalQuake_Class3, kArRegionalQuake_Class4, kArRegionalQuake_Class5,
  kArRegionalQuake_Count
} ArRegionalQuakeRule;
typedef struct ArRegionalQuakePolicy {
  ArRegionalSource source[kArRegionalQuake_Count];
} ArRegionalQuakePolicy;
typedef struct ArRegionalQuakeSnapshot {
  bool random[kArRegionalQuake_Count];
} ArRegionalQuakeSnapshot;
typedef struct ArRegionalQuakeDescriptor {
  const char *key;
  uint16_t random[kArRegionalSource_Count];
} ArRegionalQuakeDescriptor;
const ArRegionalQuakeDescriptor *ArRegionalQuake_Descriptor(ArRegionalQuakeRule rule);
bool ArRegionalQuake_Init(ArRegionalQuakePolicy *policy, ArRegionalSource source);
bool ArRegionalQuake_Resolve(const ArRegionalQuakePolicy *policy, ArRegionalQuakeSnapshot *out);
bool ArRegionalQuake_GroupSource(const ArRegionalQuakePolicy *policy, ArRegionalSource *source);

#endif
