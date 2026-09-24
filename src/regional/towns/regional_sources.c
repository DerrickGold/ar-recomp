#include "regional/towns/regional_sources.h"
#include <stddef.h>

static const ArRegionalSourcesDescriptor kRules[kArRegionalSourceItem_Count] = {
  {"source_life_on_take", {1,0,1}},
  {"source_magic_on_take", {1,0,1}},
};
const ArRegionalSourcesDescriptor *ArRegionalSources_Descriptor(ArRegionalSourceItem item) {
  return (unsigned)item < kArRegionalSourceItem_Count ? &kRules[item] : NULL;
}
bool ArRegionalSources_Init(ArRegionalSourcesPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalSourceItem_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalSources_Resolve(const ArRegionalSourcesPolicy *policy, ArRegionalSourcesSnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  ArRegionalSourcesSnapshot next;
  for (unsigned i=0;i<kArRegionalSourceItem_Count;++i) {
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
    next.automatic[i]=kRules[i].automatic[policy->source[i]] != 0;
  }
  *snapshot=next;
  return true;
}
bool ArRegionalSources_GroupSource(const ArRegionalSourcesPolicy *policy, ArRegionalSource *source) {
  ArRegionalSourcesSnapshot snapshot;
  if (!source || !ArRegionalSources_Resolve(policy,&snapshot)) return false;
  if (policy->source[0]==policy->source[1]) { *source=policy->source[0]; return true; }
  if (snapshot.automatic[0]!=snapshot.automatic[1]) return false;
  *source=snapshot.automatic[0] ? kArRegionalSource_US : kArRegionalSource_Japan;
  return true;
}
