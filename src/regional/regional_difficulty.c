#include "regional_difficulty.h"
#include <stddef.h>

static const ArRegionalDifficultyDescriptor kRules[] = {
  {"action_difficulty_hp", {0,0,1}},
  {"action_difficulty_contact", {0,0,1}},
  {"action_difficulty_clock", {0,0,1}},
  {"action_difficulty_dragon", {0,0,1}},
  {"action_difficulty_tendril", {0,0,1}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0]) == kArRegionalDifficultyRule_Count,
    "name every difficulty leaf");
const ArRegionalDifficultyDescriptor *ArRegionalDifficulty_Descriptor(unsigned rule) {
  return rule < kArRegionalDifficultyRule_Count ? &kRules[rule] : NULL;
}
bool ArRegionalDifficulty_Init(ArRegionalDifficultyPolicy *policy, ArRegionalSource source,
    ArRegionalDifficulty level) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count ||
      (unsigned)level >= kArRegionalDifficulty_Count) return false;
  for (unsigned i=0; i<kArRegionalDifficultyRule_Count; ++i) policy->source[i]=source;
  policy->level=level;
  return true;
}
bool ArRegionalDifficulty_Resolve(const ArRegionalDifficultyPolicy *policy,
    ArRegionalDifficultySnapshot *snapshot) {
  if (!policy || !snapshot || (unsigned)policy->level >= kArRegionalDifficulty_Count) return false;
  for (unsigned i=0; i<kArRegionalDifficultyRule_Count; ++i)
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
  const bool beginner=policy->level==kArRegionalDifficulty_Beginner;
  const bool expert=policy->level==kArRegionalDifficulty_Expert;
  *snapshot=(ArRegionalDifficultySnapshot){
    .spawn_hp=policy->source[kArRegionalDifficulty_SpawnHp]==kArRegionalSource_Europe ?
        (uint8_t)(1u+policy->level) : 0,
    .contact_extra=policy->source[kArRegionalDifficulty_Contact]==kArRegionalSource_Europe && expert,
    .timer_reload=policy->source[kArRegionalDifficulty_Countdown]==kArRegionalSource_Europe ?
        (beginner?71:expert?47:59) : 59,
    .skip_dragon_attack=policy->source[kArRegionalDifficulty_DragonAttack]==kArRegionalSource_Europe && beginner,
    .single_tendril_bob=policy->source[kArRegionalDifficulty_PlantTendril]==kArRegionalSource_Europe && beginner,
  };
  return true;
}
bool ArRegionalDifficulty_GroupSource(const ArRegionalDifficultyPolicy *policy, ArRegionalSource *source) {
  ArRegionalDifficultySnapshot snapshot;
  if (!source || !ArRegionalDifficulty_Resolve(policy,&snapshot)) return false;
  bool uniform=true, native=true;
  for (unsigned i=0; i<kArRegionalDifficultyRule_Count; ++i) {
    uniform &= policy->source[i]==policy->source[0];
    native &= policy->source[i]!=kArRegionalSource_Europe;
  }
  if (uniform) *source=policy->source[0];
  else if (native) *source=kArRegionalSource_US;
  else return false;
  return true;
}
uint8_t ArRegionalDifficulty_Identity(const ArRegionalDifficultySnapshot *snapshot) {
  return snapshot ? (uint8_t)(snapshot->spawn_hp | (snapshot->contact_extra<<2) |
      ((snapshot->timer_reload==71?1u:snapshot->timer_reload==47?2u:0u)<<3) |
      (snapshot->skip_dragon_attack?32u:0u) | (snapshot->single_tendril_bob?64u:0u)) : 0;
}
uint16_t ArRegionalDifficulty_SpawnHp(const ArRegionalDifficultySnapshot *snapshot,
    uint16_t flags, uint16_t hp) {
  if (!snapshot || (flags&0x8231)) return hp;
  if (snapshot->spawn_hp==2 && hp==2) return 1;
  if (snapshot->spawn_hp==3 && hp==1) return 2;
  return hp;
}
