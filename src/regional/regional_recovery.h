#ifndef AR_REGIONAL_RECOVERY_H
#define AR_REGIONAL_RECOVERY_H

#include "regional_costs.h"

typedef enum ArRegionalRecoveryRule {
  kArRegionalRecovery_SP,
  kArRegionalRecovery_Angel,
  kArRegionalRecovery_Count
} ArRegionalRecoveryRule;
typedef struct ArRegionalRecoveryPolicy {
  ArRegionalSource source[kArRegionalRecovery_Count];
} ArRegionalRecoveryPolicy;
typedef struct ArRegionalRecoverySnapshot {
  bool cycle_sp;
  uint16_t angel_calls; /* zero: native cycle queue; otherwise eligible calls */
} ArRegionalRecoverySnapshot;
typedef struct ArRegionalRecoveryDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalRecoveryDescriptor;

const ArRegionalRecoveryDescriptor *ArRegionalRecovery_Descriptor(ArRegionalRecoveryRule rule);
bool ArRegionalRecovery_Init(ArRegionalRecoveryPolicy *policy, ArRegionalSource source);
bool ArRegionalRecovery_Resolve(const ArRegionalRecoveryPolicy *policy, ArRegionalRecoverySnapshot *snapshot);
bool ArRegionalRecovery_GroupSource(const ArRegionalRecoveryPolicy *policy, ArRegionalSource *source);

#endif
