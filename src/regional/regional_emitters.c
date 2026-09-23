#include "regional_emitters.h"
#include <stddef.h>
static const ArRegionalEmitterDescriptor kRules[]={
  {"cave_emitter_interval",{360,180,255}},
  {"cave_emitter_pal_offset",{0,0,1}},
};
_Static_assert(kArRegionalEmitter_Count==2 && kArRegionalSource_US==0 &&
    kArRegionalSource_Japan==1 && kArRegionalSource_Europe==2,"preserve emitter snapshot encoding");
const ArRegionalEmitterDescriptor *ArRegionalEmitter_Descriptor(unsigned rule) {
  return rule<kArRegionalEmitter_Count?&kRules[rule]:NULL;
}
bool ArRegionalEmitter_Init(ArRegionalEmitterPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalEmitter_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalEmitter_Resolve(const ArRegionalEmitterPolicy *policy,ArRegionalEmitterSnapshot *out) {
  if(!policy || !out)return false;
  for(unsigned i=0;i<kArRegionalEmitter_Count;++i)if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
  *out=(uint8_t)(policy->source[0]|((policy->source[1]==kArRegionalSource_Europe)?4:0));return true;
}
bool ArRegionalEmitter_GroupSource(const ArRegionalEmitterPolicy *policy,ArRegionalSource *source) {
  uint8_t value;if(!source || !ArRegionalEmitter_Resolve(policy,&value))return false;
  if(value==0)*source=kArRegionalSource_US;
  else if(value==1)*source=kArRegionalSource_Japan;
  else if(value==6)*source=kArRegionalSource_Europe;
  else return false;
  return true;
}
bool ArRegionalEmitter_Row(ArRegionalEmitterSnapshot snapshot,unsigned state,unsigned row,uint16_t *duration,int16_t dx) {
  if(snapshot>6 || (snapshot&3)!=2 || !duration || state!=36 || row>=2 || *duration!=89 || dx)return false;
  /* Western rows are identical stationary poses. One 127+128-update cycle
   * reproduces PAL's single 255-update hold without swapping an asset blob. */
  *duration=(uint16_t)(126+row);return true;
}
