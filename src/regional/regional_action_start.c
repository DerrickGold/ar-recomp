#include "regional_action_start.h"
#include <stddef.h>

static const ArRegionalActionStartDescriptor kRules[]={
  {"action_start_spares",{4,2,4}},
  {"action_start_health",{24,24,8}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalActionStart_Count,"name every start rule");
const ArRegionalActionStartDescriptor *ArRegionalActionStart_Descriptor(unsigned rule) {
  return rule<kArRegionalActionStart_Count?&kRules[rule]:NULL;
}
bool ArRegionalActionStart_Init(ArRegionalActionStartPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalActionStart_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalActionStart_Resolve(const ArRegionalActionStartPolicy *policy,ArRegionalActionStartSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  for(unsigned i=0;i<kArRegionalActionStart_Count;++i)if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
  *snapshot=(ArRegionalActionStartSnapshot){
    .spares=kRules[0].value[policy->source[0]],.health=kRules[1].value[policy->source[1]]};
  return true;
}
bool ArRegionalActionStart_GroupSource(const ArRegionalActionStartPolicy *policy,ArRegionalSource *source) {
  ArRegionalActionStartSnapshot snapshot;
  if(!source || !ArRegionalActionStart_Resolve(policy,&snapshot))return false;
  if(policy->source[0]==policy->source[1]) {*source=policy->source[0];return true;}
  for(unsigned i=0;i<kArRegionalSource_Count;++i)
    if(snapshot.spares==kRules[0].value[i] && snapshot.health==kRules[1].value[i]) {*source=i;return true;}
  return false;
}
