#ifndef AR_REGIONAL_ACTION_START_H
#define AR_REGIONAL_ACTION_START_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalActionStartRule {
  kArRegionalActionStart_Spares,
  kArRegionalActionStart_Health,
  kArRegionalActionStart_Count,
} ArRegionalActionStartRule;
typedef struct ArRegionalActionStartPolicy {
  ArRegionalSource source[kArRegionalActionStart_Count];
} ArRegionalActionStartPolicy;
typedef struct ArRegionalActionStartDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalActionStartDescriptor;
typedef struct ArRegionalActionStartSnapshot {
  uint8_t spares,health;
} ArRegionalActionStartSnapshot;
const ArRegionalActionStartDescriptor *ArRegionalActionStart_Descriptor(unsigned rule);
bool ArRegionalActionStart_Init(ArRegionalActionStartPolicy *policy,ArRegionalSource source);
bool ArRegionalActionStart_Resolve(const ArRegionalActionStartPolicy *policy,ArRegionalActionStartSnapshot *snapshot);
bool ArRegionalActionStart_GroupSource(const ArRegionalActionStartPolicy *policy,ArRegionalSource *source);
#endif
