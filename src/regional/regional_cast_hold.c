#include "regional_cast_hold.h"
#include <stddef.h>
static const ArRegionalCastHoldDescriptor kRules[]={
  {"fillmore_left_prop_cast_hold",{0,0,1}},
  {"fillmore_upper_prop_cast_hold",{0,0,1}},
  {"fillmore_right_prop_cast_hold",{0,0,1}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalCastHold_Count,"describe every linked prop");
const ArRegionalCastHoldDescriptor *ArRegionalCastHold_Descriptor(unsigned rule) {
  return rule<kArRegionalCastHold_Count?&kRules[rule]:NULL;
}
bool ArRegionalCastHold_Init(ArRegionalCastHoldPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalCastHold_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalCastHold_Resolve(const ArRegionalCastHoldPolicy *policy,ArRegionalCastHoldSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t bits=0;
  for(unsigned i=0;i<kArRegionalCastHold_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    if(policy->source[i]==kArRegionalSource_Europe)bits|=(uint8_t)(1u<<i);
  }
  *snapshot=bits;return true;
}
bool ArRegionalCastHold_GroupSource(const ArRegionalCastHoldPolicy *policy,ArRegionalSource *source) {
  uint8_t bits;if(!source || !ArRegionalCastHold_Resolve(policy,&bits))return false;
  if(bits && bits!=7)return false;
  bool jp=true;for(unsigned i=0;i<kArRegionalCastHold_Count;++i)jp&=policy->source[i]==kArRegionalSource_Japan;
  *source=bits?kArRegionalSource_Europe:jp?kArRegionalSource_Japan:kArRegionalSource_US;return true;
}
