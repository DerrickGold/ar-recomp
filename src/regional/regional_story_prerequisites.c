#include "regional_story_prerequisites.h"
#include <stddef.h>

/* Population must be strictly greater than these thresholds. The third
 * leaf is boolean: clear prerequisite 9 when Bloodpool event 8 rejects
 * an unfinished Act 2. It does not enable an earlier Compass grant. */
static const ArRegionalStoryDescriptor kRules[kArRegionalStory_Count] = {
  {"story_fillmore_hint_threshold", {110,88,110}},
  {"story_kasandora_tablet_threshold", {700,400,700}},
  {"story_failed_act2_clear_compass", {1,0,1}},
};
const ArRegionalStoryDescriptor *ArRegionalStory_Descriptor(ArRegionalStoryRule rule) {
  return (unsigned)rule<kArRegionalStory_Count?&kRules[rule]:NULL;
}
bool ArRegionalStory_Init(ArRegionalStoryPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source>=kArRegionalSource_Count) return false;
  for (unsigned i=0;i<kArRegionalStory_Count;++i) policy->source[i]=source;
  return true;
}
bool ArRegionalStory_Resolve(const ArRegionalStoryPolicy *policy, ArRegionalStorySnapshot *snapshot) {
  if (!policy || !snapshot) return false;
  ArRegionalStorySnapshot next;
  for (unsigned i=0;i<kArRegionalStory_Count;++i) {
    if ((unsigned)policy->source[i]>=kArRegionalSource_Count) return false;
    next.value[i]=kRules[i].value[policy->source[i]];
  }
  *snapshot=next; return true;
}
bool ArRegionalStory_GroupSource(const ArRegionalStoryPolicy *policy, ArRegionalSource *source) {
  ArRegionalStorySnapshot snapshot;
  if (!source || !ArRegionalStory_Resolve(policy,&snapshot)) return false;
  bool uniform=true;
  for (unsigned i=1;i<kArRegionalStory_Count;++i) uniform &= policy->source[i]==policy->source[0];
  if (uniform) { *source=policy->source[0]; return true; }
  for (unsigned candidate=0;candidate<kArRegionalSource_Count;++candidate) {
    bool equal=true;
    for (unsigned i=0;i<kArRegionalStory_Count;++i) equal &= snapshot.value[i]==kRules[i].value[candidate];
    if (equal) { *source=(ArRegionalSource)candidate; return true; }
  }
  return false;
}
