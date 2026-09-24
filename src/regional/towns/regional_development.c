#include "regional/towns/regional_development.h"
#include <stddef.h>

/* Eligible service counts at the existing host clock; no PAL-rate conversion.
 * docs/regional-differences-technical.md#simulation-rules-in-code */
static const ArRegionalDevelopmentDescriptor kRules[kArRegionalDevelopmentRule_Count] = {
  {"development_service_divider", {1,5,1}},
  {"development_long_cycle", {720,480,720}},
  {"development_effect_divider", {1,5,1}},
};
const ArRegionalDevelopmentDescriptor *ArRegionalDevelopment_Descriptor(ArRegionalDevelopmentRule rule) {
  return (unsigned)rule < kArRegionalDevelopmentRule_Count ? &kRules[rule] : NULL;
}
bool ArRegionalDevelopment_Init(ArRegionalDevelopmentPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalDevelopment_Resolve(const ArRegionalDevelopmentPolicy *policy,
                                   ArRegionalDevelopmentSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  for (unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i)
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
  *snapshot = (ArRegionalDevelopmentSnapshot){kRules[0].updates[policy->source[0]],
      kRules[1].updates[policy->source[1]],kRules[2].updates[policy->source[2]]};
  return true;
}
bool ArRegionalDevelopment_SnapshotValid(const ArRegionalDevelopmentSnapshot *snapshot) {
  return snapshot && (snapshot->service_divider==1 || snapshot->service_divider==5) &&
      (snapshot->long_cycle==720 || snapshot->long_cycle==480) &&
      (snapshot->effect_divider==1 || snapshot->effect_divider==5);
}
bool ArRegionalDevelopment_GroupSource(const ArRegionalDevelopmentPolicy *policy, ArRegionalSource *source) {
  ArRegionalDevelopmentSnapshot unused;
  if(!source || !ArRegionalDevelopment_Resolve(policy,&unused))return false;
  bool uniform=true;
  for(unsigned i=1;i<kArRegionalDevelopmentRule_Count;++i)uniform &= policy->source[i]==policy->source[0];
  if(uniform){*source=policy->source[0];return true;}
  for(unsigned candidate=0;candidate<kArRegionalSource_Count;++candidate) {
    bool equal=true;
    for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i)
      equal &= kRules[i].updates[policy->source[i]]==kRules[i].updates[candidate];
    if(equal){*source=(ArRegionalSource)candidate;return true;}
  }
  return false;
}
