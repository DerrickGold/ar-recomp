#include "regional/action/regional_fire_enemy.h"
#include <stddef.h>
static const ArRegionalFireDescriptor kRules[]={
  {"kasandora_fire_curve",{0,1,0}},
  {"kasandora_fire_close_strategy",{0,1,0}},
  {"kasandora_fire_child_threshold",{160,128,160}},
  {"kasandora_fire_bounce_threshold",{242,210,242}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalFire_Count,"describe every fire-enemy rule");
const ArRegionalFireDescriptor *ArRegionalFire_Descriptor(unsigned rule) {
  return rule<kArRegionalFire_Count?&kRules[rule]:NULL;
}
bool ArRegionalFire_Init(ArRegionalFirePolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalFire_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalFire_Resolve(const ArRegionalFirePolicy *policy,ArRegionalFireSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t bits=0;
  for(unsigned i=0;i<kArRegionalFire_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    if(policy->source[i]==kArRegionalSource_Japan)bits|=(uint8_t)(1u<<i);
  }
  *snapshot=bits;return true;
}
bool ArRegionalFire_GroupSource(const ArRegionalFirePolicy *policy,ArRegionalSource *source) {
  uint8_t bits;if(!source || !ArRegionalFire_Resolve(policy,&bits))return false;
  if(bits && bits!=15)return false;
  bool eu=true;for(unsigned i=0;i<kArRegionalFire_Count;++i)eu&=policy->source[i]==kArRegionalSource_Europe;
  *source=bits?kArRegionalSource_Japan:eu?kArRegionalSource_Europe:kArRegionalSource_US;return true;
}
uint16_t ArRegionalFire_Value(ArRegionalFireSnapshot snapshot,unsigned rule) {
  return snapshot>15 || rule>=kArRegionalFire_Count?UINT16_MAX:kRules[rule].value[(snapshot>>rule)&1];
}
bool ArRegionalFire_CurveRow(ArRegionalFireSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t duration,int16_t *dx,int16_t *dy) {
  static const int8_t native[2][8][2]={
    {{-2,-1},{-2,-2},{-2,-3},{-2,-3},{-2,-3},{-1,-2},{-1,-1},{-1,0}},
    {{1,0},{1,1},{1,2},{2,3},{2,3},{2,3},{2,2},{2,1}}};
  static const int8_t japanese_x[]={-2,-1,-1,0,0,1,1,2};
  static const int8_t japanese_y[]={2,2,4,4,4,4,2,2};
  if(!dx || !dy || ArRegionalFire_Value(snapshot,kArRegionalFire_Curve)!=1 ||
      (state!=13 && state!=14) || row>=8 || visual!=32+row/4 || duration!=1)return false;
  if(*dx!=native[state-13][row][0] || *dy!=native[state-13][row][1])return false;
  *dx=japanese_x[row];*dy=(int16_t)(japanese_y[row]*(state==13?-1:1));return true;
}
