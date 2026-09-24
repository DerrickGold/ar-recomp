#include "regional/towns/regional_sim_combat.h"
#include <stddef.h>

static const ArRegionalSimCombatDescriptor kRules[kArRegionalSimCombat_Count]={
  {"sim_dragon_threshold",{2,1,2}}, {"sim_demon_threshold",{3,2,3}},
  {"sim_dragon_contact",{3,2,3}}, {"sim_demon_contact",{6,3,6}},
  {"sim_skull_contact",{8,4,8}},
};
enum { kMask = 0x1f };
_Static_assert(kArRegionalSimCombat_Count==5,"extend the actor codec explicitly for new snapshot bits");
const ArRegionalSimCombatDescriptor *ArRegionalSimCombat_Descriptor(ArRegionalSimCombatRule rule) {
  return (unsigned)rule<kArRegionalSimCombat_Count ? &kRules[rule] : NULL;
}
bool ArRegionalSimCombat_Init(ArRegionalSimCombatPolicy *policy,ArRegionalSource source) {
  if (!policy || (unsigned)source>=kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalSimCombat_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalSimCombat_Resolve(const ArRegionalSimCombatPolicy *policy,ArRegionalSimCombatSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  uint16_t next=0;
  for (unsigned i=0;i<kArRegionalSimCombat_Count;++i) {
    if ((unsigned)policy->source[i]>=kArRegionalSource_Count) return false;
    if (policy->source[i]==kArRegionalSource_Japan) next|=(uint16_t)(1u<<i);
  }
  *snapshot=next;return true;
}
bool ArRegionalSimCombat_GroupSource(const ArRegionalSimCombatPolicy *policy,ArRegionalSource *source) {
  uint16_t snapshot;
  if (!source || !ArRegionalSimCombat_Resolve(policy,&snapshot)) return false;
  bool uniform=true;
  for (unsigned i=1;i<kArRegionalSimCombat_Count;++i) uniform &= policy->source[i]==policy->source[0];
  if (uniform) *source=policy->source[0];
  else if (!snapshot) *source=kArRegionalSource_US;
  else if (snapshot==kMask) *source=kArRegionalSource_Japan;
  else return false;
  return true;
}
bool ArRegionalSimCombat_Values(uint16_t snapshot,unsigned species,uint8_t *threshold,uint8_t *contact) {
  if ((snapshot & ~kMask) || species>=4 || !threshold || !contact) return false;
  static const uint8_t limits[4]={2,0,3,7}, damage[4]={3,1,6,8};
  *threshold=limits[species];*contact=damage[species];
  if (species==0) {
    *threshold=kRules[0].value[(snapshot & 1)!=0];
    *contact=kRules[2].value[(snapshot & 4)!=0];
  } else if (species==2) {
    *threshold=kRules[1].value[(snapshot & 2)!=0];
    *contact=kRules[3].value[(snapshot & 8)!=0];
  } else if (species==3) *contact=kRules[4].value[(snapshot & 16)!=0];
  return true;
}
