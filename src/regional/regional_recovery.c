#include "regional_recovery.h"
#include <stddef.h>

static const ArRegionalRecoveryDescriptor kRules[kArRegionalRecovery_Count] = {
  {"recovery_sp_cycle_queue", {1, 0, 1}},
  {"recovery_angel_eligible_calls", {0, 60, 0}},
};

const ArRegionalRecoveryDescriptor *ArRegionalRecovery_Descriptor(ArRegionalRecoveryRule rule) {
  return (unsigned)rule < kArRegionalRecovery_Count ? &kRules[rule] : NULL;
}

bool ArRegionalRecovery_Init(ArRegionalRecoveryPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) policy->source[i] = source;
  return true;
}

bool ArRegionalRecovery_Resolve(const ArRegionalRecoveryPolicy *policy, ArRegionalRecoverySnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i)
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
  *snapshot = (ArRegionalRecoverySnapshot){kRules[0].value[policy->source[0]] != 0,
      kRules[1].value[policy->source[1]]};
  return true;
}

bool ArRegionalRecovery_GroupSource(const ArRegionalRecoveryPolicy *policy, ArRegionalSource *source) {
  ArRegionalRecoverySnapshot unused;
  if (!source || !ArRegionalRecovery_Resolve(policy, &unused)) return false;
  if (policy->source[0] == policy->source[1]) {
    *source = policy->source[0];
    return true;
  }
  for (unsigned candidate = 0; candidate < kArRegionalSource_Count; ++candidate) {
    bool equal = true;
    for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i)
      equal &= kRules[i].value[policy->source[i]] == kRules[i].value[candidate];
    if (equal) { *source = (ArRegionalSource)candidate; return true; }
  }
  return false;
}
