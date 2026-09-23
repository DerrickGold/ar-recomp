#ifndef AR_REGIONAL_DEVELOPMENT_H
#define AR_REGIONAL_DEVELOPMENT_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalDevelopmentRule {
  kArRegionalDevelopment_ServiceDivider,
  kArRegionalDevelopment_LongCycle,
  kArRegionalDevelopment_EffectDivider,
  kArRegionalDevelopmentRule_Count,
} ArRegionalDevelopmentRule;
typedef struct ArRegionalDevelopmentDescriptor {
  const char *key;
  uint16_t updates[kArRegionalSource_Count];
} ArRegionalDevelopmentDescriptor;
typedef struct ArRegionalDevelopmentPolicy {
  ArRegionalSource source[kArRegionalDevelopmentRule_Count];
} ArRegionalDevelopmentPolicy;
typedef struct ArRegionalDevelopmentSnapshot {
  uint16_t service_divider, long_cycle, effect_divider;
} ArRegionalDevelopmentSnapshot;

const ArRegionalDevelopmentDescriptor *ArRegionalDevelopment_Descriptor(ArRegionalDevelopmentRule rule);
bool ArRegionalDevelopment_Init(ArRegionalDevelopmentPolicy *policy, ArRegionalSource source);
bool ArRegionalDevelopment_Resolve(const ArRegionalDevelopmentPolicy *policy,
                                   ArRegionalDevelopmentSnapshot *snapshot);
bool ArRegionalDevelopment_SnapshotValid(const ArRegionalDevelopmentSnapshot *snapshot);
bool ArRegionalDevelopment_GroupSource(const ArRegionalDevelopmentPolicy *policy, ArRegionalSource *source);
#endif
