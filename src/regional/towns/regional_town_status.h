#ifndef AR_REGIONAL_TOWN_STATUS_H
#define AR_REGIONAL_TOWN_STATUS_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalTownStatusRule {
  kArRegionalTownStatus_Classifier,
  kArRegionalTownStatus_LowGrowth,
  kArRegionalTownStatus_PlotCount,
  kArRegionalTownStatus_FoodAttempt,
  kArRegionalTownStatus_PersistentFlags,
  kArRegionalTownStatus_Count,
} ArRegionalTownStatusRule;
typedef struct ArRegionalTownStatusPolicy {
  ArRegionalSource source[kArRegionalTownStatus_Count];
} ArRegionalTownStatusPolicy;
typedef struct ArRegionalTownStatusSnapshot {
  uint16_t japanese[kArRegionalTownStatus_Count];
} ArRegionalTownStatusSnapshot;
typedef struct ArRegionalTownStatusDescriptor {
  const char *key;
  uint16_t japanese[kArRegionalSource_Count];
} ArRegionalTownStatusDescriptor;
const ArRegionalTownStatusDescriptor *ArRegionalTownStatus_Descriptor(ArRegionalTownStatusRule rule);
bool ArRegionalTownStatus_Init(ArRegionalTownStatusPolicy *policy, ArRegionalSource source);
bool ArRegionalTownStatus_Resolve(const ArRegionalTownStatusPolicy *policy, ArRegionalTownStatusSnapshot *snapshot);
bool ArRegionalTownStatus_GroupSource(const ArRegionalTownStatusPolicy *policy, ArRegionalSource *source);
/* The native classifier's ordered predicates, not a population cap. Report
 * code 5 is "limit" in Japanese, not the separate "slow" label. */
uint16_t ArRegionalTownStatus_JapaneseCode(uint16_t population, uint16_t development_gate, uint16_t flags);
bool ArRegionalTownStatus_Plots(bool japanese, unsigned town, uint16_t *count);

#endif
