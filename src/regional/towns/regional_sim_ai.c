#include "regional/towns/regional_sim_ai.h"
#include <stddef.h>
static const ArRegionalSimAiDescriptor kRules[kArRegionalSimAi_Count]={
  {"sim_dragon_search_interval",{1,8,1}},
  {"sim_dragon_extra_actor_pass",{1,0,1}},
  {"sim_target_wide_coordinates",{0,1,0}},
  {"sim_target_full_pool",{0,1,0}},
  {"sim_bat_fallback_threshold",{253,250,253}},
  {"sim_bat_abduction_wait",{1,60,1}},
};
enum { kMask=0x3f };
_Static_assert(kArRegionalSimAi_Count==6,"extend the versioned AI snapshot for new leaves");
const ArRegionalSimAiDescriptor *ArRegionalSimAi_Descriptor(ArRegionalSimAiRule rule) {
  return (unsigned)rule<kArRegionalSimAi_Count?&kRules[rule]:NULL;
}
bool ArRegionalSimAi_Init(ArRegionalSimAiPolicy *policy,ArRegionalSource source) {
  if (!policy || (unsigned)source>=kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalSimAi_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalSimAi_Resolve(const ArRegionalSimAiPolicy *policy,ArRegionalSimAiSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  uint16_t next=0;
  for (unsigned i=0;i<kArRegionalSimAi_Count;++i) {
    if ((unsigned)policy->source[i]>=kArRegionalSource_Count) return false;
    if (policy->source[i]==kArRegionalSource_Japan) next|=(uint16_t)(1u<<i);
  }
  *snapshot=next;return true;
}
bool ArRegionalSimAi_GroupSource(const ArRegionalSimAiPolicy *policy,ArRegionalSource *source) {
  uint16_t snapshot;
  if (!source || !ArRegionalSimAi_Resolve(policy,&snapshot)) return false;
  bool uniform=true;
  for (unsigned i=1;i<kArRegionalSimAi_Count;++i) uniform &= policy->source[i]==policy->source[0];
  if (uniform) *source=policy->source[0];
  else if (!snapshot) *source=kArRegionalSource_US;
  else if (snapshot==kMask) *source=kArRegionalSource_Japan;
  else return false;
  return true;
}
bool ArRegionalSimAi_Value(uint16_t snapshot,ArRegionalSimAiRule rule,uint16_t *value) {
  if (!value || (snapshot & ~kMask) || (unsigned)rule>=kArRegionalSimAi_Count) return false;
  *value=kRules[rule].value[(snapshot & (1u<<rule))!=0];return true;
}
