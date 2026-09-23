#include "regional_actor_stats.h"
#include <stddef.h>
static const ArRegionalActorStatDescriptor kRules[]={
#define AR_ACTOR_STAT(key,actor,field,us,jp,eu) {key,actor,field,{us,jp,eu}},
#include "regional_actor_stats_data.inc"
#undef AR_ACTOR_STAT
};
const ArRegionalActorStatDescriptor *ArRegionalActorStats_Descriptor(unsigned rule) {
  return rule<kArRegionalActorStat_Count?&kRules[rule]:NULL;
}
bool ArRegionalActorStats_Init(ArRegionalActorStatsPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalActorStat_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalActorStats_Resolve(const ArRegionalActorStatsPolicy *policy,ArRegionalActorStatsSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  ArRegionalActorStatsSnapshot next={0};
  for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    next.value[i]=(uint8_t)kRules[i].value[policy->source[i]];
    if(i<kArRegionalActorStat_BaseCount)next.changed|=next.value[i]!=kRules[i].value[0];
  }
  *snapshot=next;return true;
}
bool ArRegionalActorStats_GroupSource(const ArRegionalActorStatsPolicy *policy,ArRegionalSource *source) {
  ArRegionalActorStatsSnapshot snapshot;if(!source || !ArRegionalActorStats_Resolve(policy,&snapshot))return false;
  for(unsigned region=0;region<kArRegionalSource_Count;++region) {
    bool matches=true;
    for(unsigned i=0;i<kArRegionalActorStat_Count;++i)matches&=snapshot.value[i]==kRules[i].value[region];
    if(matches){*source=(ArRegionalSource)region;return true;}
  }
  return false;
}
bool ArRegionalActorStats_Apply(const ArRegionalActorStatsSnapshot *snapshot,uint16_t actor,
    uint16_t native_hp,uint16_t native_attack,uint16_t *hp,uint16_t *attack) {
  if(!snapshot || !snapshot->changed || !hp || !attack || native_hp>255 || native_attack>255)return false;
  /* Stable area/type order allows a short binary search only at initialization,
   * followed by at most the HP and attack leaves for the selected owner. */
  unsigned lo=0,hi=kArRegionalActorStat_BaseCount;
  while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(kRules[mid].actor<actor)lo=mid+1;else hi=mid;}
  uint16_t next_hp=native_hp,next_attack=native_attack;
  for(unsigned i=lo;i<kArRegionalActorStat_BaseCount && kRules[i].actor==actor;++i) {
    const bool health=kRules[i].field==kArRegionalActorStat_HP;
    if((health?native_hp:native_attack)!=kRules[i].value[0])return false;
    if(health)next_hp=snapshot->value[i];else next_attack=snapshot->value[i];
  }
  if(next_hp==native_hp && next_attack==native_attack)return false;
  *hp=next_hp;*attack=next_attack;return true;
}
