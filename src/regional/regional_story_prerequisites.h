#ifndef AR_REGIONAL_STORY_PREREQUISITES_H
#define AR_REGIONAL_STORY_PREREQUISITES_H

#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Prerequisite producers only. These do not grant items, mark events fired,
 * rewrite town populations or replace the native callback priority. */
typedef enum ArRegionalStoryRule {
  kArRegionalStory_FillmoreHint,
  kArRegionalStory_KasandoraTablet,
  kArRegionalStory_ClearCompassPrerequisite,
  kArRegionalStory_Count
} ArRegionalStoryRule;
typedef struct ArRegionalStoryDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalStoryDescriptor;
typedef struct ArRegionalStoryPolicy {
  ArRegionalSource source[kArRegionalStory_Count];
} ArRegionalStoryPolicy;
typedef struct ArRegionalStorySnapshot {
  uint16_t value[kArRegionalStory_Count];
} ArRegionalStorySnapshot;
const ArRegionalStoryDescriptor *ArRegionalStory_Descriptor(ArRegionalStoryRule rule);
bool ArRegionalStory_Init(ArRegionalStoryPolicy *policy, ArRegionalSource source);
bool ArRegionalStory_Resolve(const ArRegionalStoryPolicy *policy, ArRegionalStorySnapshot *snapshot);
bool ArRegionalStory_GroupSource(const ArRegionalStoryPolicy *policy, ArRegionalSource *source);

#endif
