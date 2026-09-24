#include "regional/towns/regional_quake.h"
#include <stddef.h>

static const ArRegionalQuakeDescriptor kRules[kArRegionalQuake_Count] = {
  {"quake_houses_random", {0, 1, 0}},
  {"quake_fields_random", {0, 1, 0}},
  {"quake_class3_random", {0, 1, 0}},
  {"quake_class4_random", {0, 1, 0}},
  {"quake_class5_random", {0, 1, 0}},
};
const ArRegionalQuakeDescriptor *ArRegionalQuake_Descriptor(ArRegionalQuakeRule rule) {
  return (unsigned)rule < kArRegionalQuake_Count ? &kRules[rule] : NULL;
}
bool ArRegionalQuake_Init(ArRegionalQuakePolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) policy->source[i] = source;
  return true;
}
bool ArRegionalQuake_Resolve(const ArRegionalQuakePolicy *policy, ArRegionalQuakeSnapshot *out) {
  if (!policy || !out) return false;
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    out->random[i] = kRules[i].random[policy->source[i]] != 0;
  return true;
}
bool ArRegionalQuake_GroupSource(const ArRegionalQuakePolicy *policy, ArRegionalSource *source) {
  ArRegionalQuakeSnapshot snapshot;
  if (!source || !ArRegionalQuake_Resolve(policy, &snapshot)) return false;
  bool same = true;
  for (unsigned i = 1; i < kArRegionalQuake_Count; ++i) same &= policy->source[i] == policy->source[0];
  if (same) { *source = policy->source[0]; return true; }
  for (unsigned i = 1; i < kArRegionalQuake_Count; ++i)
    if (snapshot.random[i] != snapshot.random[0]) return false;
  *source = snapshot.random[0] ? kArRegionalSource_Japan : kArRegionalSource_US;
  return true;
}
