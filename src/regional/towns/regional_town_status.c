#include "regional/towns/regional_town_status.h"
#include <stddef.h>

static const ArRegionalTownStatusDescriptor kRules[kArRegionalTownStatus_Count] = {
  {"town_report_flag_classifier", {0,1,0}},
  {"town_status_fixed_low_growth", {0,1,0}},
  {"town_status_extra_plot", {0,1,0}},
  {"town_status_food_attempt", {0,1,0}},
  {"town_status_discard_computed_flags", {0,1,0}},
};
const ArRegionalTownStatusDescriptor *ArRegionalTownStatus_Descriptor(ArRegionalTownStatusRule rule) {
  return (unsigned)rule<kArRegionalTownStatus_Count ? &kRules[rule] : NULL;
}
bool ArRegionalTownStatus_Init(ArRegionalTownStatusPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source>=kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalTownStatus_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalTownStatus_Resolve(const ArRegionalTownStatusPolicy *policy, ArRegionalTownStatusSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  ArRegionalTownStatusSnapshot next;
  for (unsigned i=0;i<kArRegionalTownStatus_Count;++i) {
    if ((unsigned)policy->source[i]>=kArRegionalSource_Count) return false;
    next.japanese[i]=kRules[i].japanese[policy->source[i]];
  }
  *snapshot=next; return true;
}
bool ArRegionalTownStatus_GroupSource(const ArRegionalTownStatusPolicy *policy, ArRegionalSource *source) {
  ArRegionalTownStatusSnapshot snapshot;
  if (!source || !ArRegionalTownStatus_Resolve(policy,&snapshot)) return false;
  bool uniform=true;
  for (unsigned i=1;i<kArRegionalTownStatus_Count;++i) uniform &= policy->source[i]==policy->source[0];
  if (uniform) { *source=policy->source[0]; return true; }
  for (unsigned candidate=0;candidate<kArRegionalSource_Count;++candidate) {
    bool equal=true;
    for (unsigned i=0;i<kArRegionalTownStatus_Count;++i)
      equal &= snapshot.japanese[i]==kRules[i].japanese[candidate];
    if (equal) { *source=(ArRegionalSource)candidate; return true; }
  }
  return false;
}
uint16_t ArRegionalTownStatus_JapaneseCode(uint16_t population, uint16_t development_gate, uint16_t flags) {
  if (!population) return 0;
  if (development_gate) return 1;
  if (flags & 1) return 5;
  if (flags & 2) return 1;
  if (flags & 0x54) return 2;
  if (flags & 0x80) return 1;
  return 3;
}
bool ArRegionalTownStatus_Plots(bool japanese, unsigned town, uint16_t *count) {
  static const uint16_t us[6]={28,29,39,25,15,20};
  if (!count || town>=6) return false;
  *count=(uint16_t)(us[town]+japanese); return true;
}
