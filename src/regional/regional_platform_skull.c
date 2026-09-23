#include "regional_platform_skull.h"
#include <stddef.h>
static const ArRegionalPlatformSkullDescriptor kRules[]={
  {"aitos_skull_deflection",{0,1,0}},
  {"aitos_skull_death_reward",{0x20,0,0x20}},
  {"aitos_skull_range_x",{32,24,32}},
  {"aitos_skull_range_y",{64,24,64}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalPlatformSkull_Count,"describe every skull rule");
_Static_assert(kArRegionalPlatformSkull_Count<=8,"skull snapshot capacity");
const ArRegionalPlatformSkullDescriptor *ArRegionalPlatformSkull_Descriptor(unsigned rule) {
  return rule<kArRegionalPlatformSkull_Count?&kRules[rule]:NULL;
}
bool ArRegionalPlatformSkull_Init(ArRegionalPlatformSkullPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalPlatformSkull_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalPlatformSkull_Resolve(const ArRegionalPlatformSkullPolicy *policy,ArRegionalPlatformSkullSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t bits=0;
  for(unsigned i=0;i<kArRegionalPlatformSkull_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    if(policy->source[i]==kArRegionalSource_Japan)bits|=(uint8_t)(1u<<i);
  }
  *snapshot=bits;return true;
}
bool ArRegionalPlatformSkull_GroupSource(const ArRegionalPlatformSkullPolicy *policy,ArRegionalSource *source) {
  uint8_t bits;if(!source || !ArRegionalPlatformSkull_Resolve(policy,&bits))return false;
  if(bits && bits!=(1u<<kArRegionalPlatformSkull_Count)-1)return false;
  bool europe=true;
  for(unsigned i=0;i<kArRegionalPlatformSkull_Count;++i)europe&=policy->source[i]==kArRegionalSource_Europe;
  *source=bits?kArRegionalSource_Japan:europe?kArRegionalSource_Europe:kArRegionalSource_US;
  return true;
}
uint16_t ArRegionalPlatformSkull_Value(ArRegionalPlatformSkullSnapshot snapshot,unsigned rule) {
  if(rule>=kArRegionalPlatformSkull_Count || snapshot>>kArRegionalPlatformSkull_Count)return UINT16_MAX;
  return kRules[rule].value[(snapshot>>rule)&1];
}
