#include "regional_score_lives.h"

static const ArRegionalScoreLivesDescriptor kRule={"action_score_lives",{0,0,1}};
const ArRegionalScoreLivesDescriptor *ArRegionalScoreLives_Descriptor(void) {return &kRule;}
bool ArRegionalScoreLives_Resolve(ArRegionalSource source, bool *enabled) {
  if(!enabled || (unsigned)source>=kArRegionalSource_Count)return false;
  *enabled=kRule.enabled[source]!=0;return true;
}
bool ArRegionalScoreLives_Award(uint16_t before, uint16_t after, bool action_mode) {
  return action_mode && ((before^after)&0xe000)!=0;
}
uint8_t ArRegionalScoreLives_Increment(uint8_t bcd_spares) {
  unsigned low=(bcd_spares&15u)+1u,carry=low>9u;
  if(carry)low+=6;
  unsigned high=(bcd_spares>>4)+carry;
  if(high>9u)high+=6;
  return (uint8_t)((high<<4)|(low&15u));
}
