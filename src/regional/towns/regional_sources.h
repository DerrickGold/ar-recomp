#ifndef AR_REGIONAL_SOURCES_H
#define AR_REGIONAL_SOURCES_H
/* RegionalSources: whether life and magic sources are collected automatically, per region.
 * Phase: pure.
 * Tests: tests/actraiser_sources_test.c */

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalSourceItem {
  kArRegionalSourceItem_Life,
  kArRegionalSourceItem_Magic,
  kArRegionalSourceItem_Count
} ArRegionalSourceItem;
typedef struct ArRegionalSourcesPolicy {
  ArRegionalSource source[kArRegionalSourceItem_Count];
} ArRegionalSourcesPolicy;
typedef struct ArRegionalSourcesSnapshot {
  bool automatic[kArRegionalSourceItem_Count];
} ArRegionalSourcesSnapshot;
typedef struct ArRegionalSourcesDescriptor {
  const char *key;
  uint16_t automatic[kArRegionalSource_Count];
} ArRegionalSourcesDescriptor;

const ArRegionalSourcesDescriptor *ArRegionalSources_Descriptor(ArRegionalSourceItem item);
bool ArRegionalSources_Init(ArRegionalSourcesPolicy *policy, ArRegionalSource source);
bool ArRegionalSources_Resolve(const ArRegionalSourcesPolicy *policy,
                               ArRegionalSourcesSnapshot *snapshot);
bool ArRegionalSources_GroupSource(const ArRegionalSourcesPolicy *policy, ArRegionalSource *source);

#endif
