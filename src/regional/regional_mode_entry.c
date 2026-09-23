#include "regional_mode_entry.h"
#include <stddef.h>
static const ArRegionalModeDescriptor kRules[]={
  {"action_mode_unlocked",{0,0,1}},
  {"action_game_over_title",{0,0,1}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalMode_Count,"name every mode rule");
const ArRegionalModeDescriptor *ArRegionalMode_Descriptor(unsigned rule) {
  return rule<kArRegionalMode_Count?&kRules[rule]:NULL;
}
bool ArRegionalMode_Init(ArRegionalModePolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalMode_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalMode_Resolve(const ArRegionalModePolicy *policy,uint8_t *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t result=0;
  for(unsigned i=0;i<kArRegionalMode_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    result|=kRules[i].value[policy->source[i]]<<i;
  }
  *snapshot=result;return true;
}
bool ArRegionalMode_GroupSource(const ArRegionalModePolicy *policy,ArRegionalSource *source) {
  uint8_t snapshot;
  if(!source || !ArRegionalMode_Resolve(policy,&snapshot))return false;
  if(policy->source[0]==policy->source[1]) {*source=policy->source[0];return true;}
  if(snapshot==0 || snapshot==3) {*source=snapshot?2:0;return true;}
  return false;
}
uint8_t ArRegionalMode_NextChoice(uint8_t choice,bool can_continue) {
  return choice==0?2:choice==2 && can_continue?1:0;
}
