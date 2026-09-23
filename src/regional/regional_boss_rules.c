#include "regional_boss_rules.h"
#include <stddef.h>
static const ArRegionalBossDescriptor kRules[] = {
  {"minotaur_idle_delay", {47,15,47},1},
  {"minotaur_throw_end_delay", {3,2,3},9},
  {"minotaur_throw_windup_delay", {23,23,15},6},
  {"minotaur_jump_windup_delay", {19,19,15},35},
  {"minotaur_axe_offset", {72,48,72},0},
  {"wizard_post_spread_pause", {31,0,31},0},
  {"ice_dragon_rematch_windup", {118,106,118},0},
  {"tanzra_closing_delay", {31,3,31},33},
  {"tanzra_second_form_clock", {1,0,1},0},
  {"tanzra_upper_turn_delay", {10,11,10},27},
  {"tanzra_minion_turn", {16,16,8},0},
};
_Static_assert(kArRegionalBoss_Count>0 && kArRegionalBoss_Count<=32,"boss snapshot capacity");
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalBoss_Count,"describe every boss rule");
const ArRegionalBossDescriptor *ArRegionalBoss_Descriptor(unsigned rule) {
  return rule<kArRegionalBoss_Count?&kRules[rule]:NULL;
}
bool ArRegionalBoss_Init(ArRegionalBossPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalBoss_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalBoss_Resolve(const ArRegionalBossPolicy *policy,ArRegionalBossSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint64_t result=0;
  for(unsigned i=0;i<kArRegionalBoss_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    const uint16_t value=kRules[i].value[policy->source[i]];
    unsigned canonical=0;while(kRules[i].value[canonical]!=value)++canonical;
    result|=(uint64_t)canonical<<(2*i);
  }
  *snapshot=result;return true;
}
uint16_t ArRegionalBoss_Value(ArRegionalBossSnapshot snapshot,unsigned rule) {
  const uint64_t known=UINT64_MAX>>(64-2*kArRegionalBoss_Count);
  if(rule>=kArRegionalBoss_Count || (snapshot&~known) ||
      (snapshot&(snapshot>>1)&UINT64_C(0x5555555555555555)))return UINT16_MAX;
  const unsigned source=(unsigned)((snapshot>>(2*rule))&3);
  return source<kArRegionalSource_Count?kRules[rule].value[source]:UINT16_MAX;
}
bool ArRegionalBoss_GroupSource(const ArRegionalBossPolicy *policy,ArRegionalSource *source) {
  uint64_t snapshot;if(!source || !ArRegionalBoss_Resolve(policy,&snapshot))return false;
  for(unsigned region=0;region<kArRegionalSource_Count;++region) {
    bool match=true;
    for(unsigned i=0;i<kArRegionalBoss_Count;++i)match&=ArRegionalBoss_Value(snapshot,i)==kRules[i].value[region];
    if(match){*source=(ArRegionalSource)region;return true;}
  }
  return false;
}
bool ArRegionalBoss_MinoRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || dx || dy)return false;
  unsigned rule;
  if(state==0 && !row)rule=kArRegionalBoss_MinoIdle;
  else if(state==2 && row==4)rule=kArRegionalBoss_MinoThrowEnd;
  else if(state==1 && !row)rule=kArRegionalBoss_MinoThrowWindup;
  else if(state==4 && !row)rule=kArRegionalBoss_MinoJumpWindup;
  else return false;
  const uint16_t next=ArRegionalBoss_Value(snapshot,rule);
  if(next==UINT16_MAX || *duration!=kRules[rule].value[0])return false;
  *duration=next;return true;
}
bool ArRegionalBoss_IceSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next) {
  if(!next || ArRegionalBoss_Value(snapshot,kArRegionalBoss_IceWindup)!=106 ||
      !((state==17 && row==5) || (state==18 && row==6)))return false;
  *next=row+2;return true;
}
bool ArRegionalBoss_TanzraRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || dy)return false;
  unsigned rule;
  if(state==10 && row==7 && !dx)rule=kArRegionalBoss_TanzraClosing;
  else if(state==48 && !row && dx==-4)rule=kArRegionalBoss_TanzraUpperTurn;
  else return false;
  const uint16_t value=ArRegionalBoss_Value(snapshot,rule);
  if(value==UINT16_MAX || *duration!=kRules[rule].value[0])return false;
  *duration=value;return true;
}
bool ArRegionalBoss_TanzraMinionSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next) {
  if(!next || ArRegionalBoss_Value(snapshot,kArRegionalBoss_TanzraMinionTurn)!=8 || state!=22 || row)return false;
  *next=2;return true;
}
