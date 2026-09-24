#include "regional/towns/regional_support.h"
#include <stddef.h>
static const ArRegionalSupportDescriptor kRules[kArRegionalSupport_Count]={
  {"support_regular_fields",{32,16,32}},
  {"support_upgraded_fields",{48,24,48}},
  {"support_factory_class3",{72,32,72}},
  {"support_factory_class4",{72,32,72}},
  {"support_other_structures",{32,16,32}},
};
const ArRegionalSupportDescriptor *ArRegionalSupport_Descriptor(ArRegionalSupportRule rule) {
  return (unsigned)rule<kArRegionalSupport_Count?&kRules[rule]:NULL;
}
bool ArRegionalSupport_Init(ArRegionalSupportPolicy *policy,ArRegionalSource source) {
  if (!policy || (unsigned)source>=kArRegionalSource_Count) return false;
  for(unsigned i=0;i<kArRegionalSupport_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalSupport_Resolve(const ArRegionalSupportPolicy *policy,ArRegionalSupportSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  ArRegionalSupportSnapshot next;
  for(unsigned i=0;i<kArRegionalSupport_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    next.amount[i]=kRules[i].amount[policy->source[i]];
  }
  *snapshot=next;return true;
}
bool ArRegionalSupport_Valid(const ArRegionalSupportSnapshot *snapshot) {
  if(!snapshot)return false;
  for(unsigned i=0;i<kArRegionalSupport_Count;++i)
    if(snapshot->amount[i]!=kRules[i].amount[0] && snapshot->amount[i]!=kRules[i].amount[1])return false;
  return true;
}
bool ArRegionalSupport_GroupSource(const ArRegionalSupportPolicy *policy,ArRegionalSource *source) {
  ArRegionalSupportSnapshot snapshot;
  if(!source || !ArRegionalSupport_Resolve(policy,&snapshot))return false;
  bool uniform=true,us=true,jp=true;
  for(unsigned i=0;i<kArRegionalSupport_Count;++i) {
    uniform &= policy->source[i]==policy->source[0];
    us &= snapshot.amount[i]==kRules[i].amount[0];jp &= snapshot.amount[i]==kRules[i].amount[1];
  }
  if(uniform)*source=policy->source[0];
  else if(us)*source=kArRegionalSource_US;
  else if(jp)*source=kArRegionalSource_Japan;
  else return false;
  return true;
}
