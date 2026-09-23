#include "regional_action_motion.h"
#include <stddef.h>
static const ArRegionalActionMotionDescriptor kRules[kArRegionalActionMotion_Count]={
  {"action_bird_speed",{3,4,3},0},
  {"action_leaper_speed",{2,3,2},0},
  {"action_cave_recovery_delay",{59,39,59},1},
  {"action_cave_straight_delay",{47,31,47},11},
  {"action_cave_high_delay",{47,31,47},17},
  {"action_caster_low_delay",{47,19,47},9},
  {"action_caster_high_delay",{47,19,47},21},
  {"action_sword_straight_delay",{31,15,31},31},
  {"action_sword_high_delay",{31,23,31},32},
  {"action_trap_arrow_speed",{2,3,2},0},
  {"action_wall_head_short_hold",{31,0,31},0},
  {"action_wall_head_long_hold",{31,0,31},0},
};
enum { kMask=(1u<<kArRegionalActionMotion_Count)-1 };
_Static_assert(kArRegionalActionMotion_Count<=16,"extend the motion snapshot for new leaves");
const ArRegionalActionMotionDescriptor *ArRegionalActionMotion_Descriptor(ArRegionalActionMotionRule rule) {
  return (unsigned)rule<kArRegionalActionMotion_Count?&kRules[rule]:NULL;
}
bool ArRegionalActionMotion_Init(ArRegionalActionMotionPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalActionMotion_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalActionMotion_Resolve(const ArRegionalActionMotionPolicy *policy,ArRegionalActionMotionSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint16_t next=0;
  for(unsigned i=0;i<kArRegionalActionMotion_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    if(policy->source[i]==kArRegionalSource_Japan)next|=(uint16_t)(1u<<i);
  }
  *snapshot=next;return true;
}
bool ArRegionalActionMotion_GroupSource(const ArRegionalActionMotionPolicy *policy,ArRegionalSource *source) {
  uint16_t snapshot;if(!source || !ArRegionalActionMotion_Resolve(policy,&snapshot))return false;
  bool uniform=true;
  for(unsigned i=1;i<kArRegionalActionMotion_Count;++i)uniform &= policy->source[i]==policy->source[0];
  if(uniform)*source=policy->source[0];
  else if(!snapshot)*source=kArRegionalSource_US;
  else if(snapshot==kMask)*source=kArRegionalSource_Japan;
  else return false;
  return true;
}
bool ArRegionalActionMotion_Row(uint16_t snapshot,ArRegionalActionMotionFamily family,
    unsigned state,unsigned row,uint16_t *duration,int16_t *dx) {
  if(!duration || !dx || (snapshot&~kMask))return false;
  ArRegionalActionMotionRule rule;
  switch(family) {
    case kArRegionalActionMotion_Bird:
      if(state!=21 || row>=2 || *duration!=1)return false;
      rule=kArRegionalActionMotion_BirdSpeed;break;
    case kArRegionalActionMotion_Leaper:
      if(!((state==30 && row==10 && *duration==7) ||
          (state>=31 && state<=33 && row<4 && *duration==5)))return false;
      rule=kArRegionalActionMotion_LeaperSpeed;break;
    case kArRegionalActionMotion_Cave:
      if(state!=31 || row!=0)return false;
      rule=kArRegionalActionMotion_CaveRecovery;break;
    case kArRegionalActionMotion_CaveAttacker:
      if(state==39 && row==1)rule=kArRegionalActionMotion_CaveStraightRecovery;
      else if(state==41 && row==4)rule=kArRegionalActionMotion_CaveHighRecovery;
      else return false;
      break;
    case kArRegionalActionMotion_Caster:
      if(row!=0 || (state!=16 && state!=19))return false;
      rule=state==16?kArRegionalActionMotion_CasterLowWindup:kArRegionalActionMotion_CasterHighWindup;
      break;
    case kArRegionalActionMotion_Swordsman:
      if(state==29 && row==8)rule=kArRegionalActionMotion_SwordStraightRecovery;
      else if(state==30 && row==4)rule=kArRegionalActionMotion_SwordHighRecovery;
      else return false;
      break;
    case kArRegionalActionMotion_Arrow:
      /* Preserve the two-row Western flashing timeline. JP artwork/cadence
       * is a separate presentation choice; only per-update velocity changes. */
      if(state!=41 || row>=2 || *duration!=1)return false;
      rule=kArRegionalActionMotion_ArrowSpeed;break;
    default:return false;
  }
  const ArRegionalActionMotionDescriptor *desc=&kRules[rule];
  const uint16_t value=desc->value[(snapshot&(1u<<rule))!=0];
  if(rule<=kArRegionalActionMotion_LeaperSpeed || rule==kArRegionalActionMotion_ArrowSpeed) {
    if(*dx!=-(int16_t)desc->value[0])return false;
    *dx=-(int16_t)value;
  } else {
    if(*duration!=desc->value[0] || *dx!=0)return false;
    *duration=value;
  }
  return true;
}
