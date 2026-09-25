#ifndef AR_REGIONAL_SIM_AI_H
#define AR_REGIONAL_SIM_AI_H
/* RegionalSimAi: per-region SIM enemy AI rules (bat fallback and wait, dragon search and extra
 * pass, target pool and coordinates).
 * Phase: pure.
 * Tests: tests/actraiser_sim_ai_test.c */
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalSimAiRule {
  kArRegionalSimAi_DragonSearch,
  kArRegionalSimAi_DragonExtraPass,
  kArRegionalSimAi_TargetCoordinates,
  kArRegionalSimAi_TargetPool,
  kArRegionalSimAi_BatFallback,
  kArRegionalSimAi_BatWait,
  kArRegionalSimAi_Count
} ArRegionalSimAiRule;
typedef struct ArRegionalSimAiPolicy {
  ArRegionalSource source[kArRegionalSimAi_Count];
} ArRegionalSimAiPolicy;
typedef uint16_t ArRegionalSimAiSnapshot;
typedef struct ArRegionalSimAiDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalSimAiDescriptor;
const ArRegionalSimAiDescriptor *ArRegionalSimAi_Descriptor(ArRegionalSimAiRule rule);
bool ArRegionalSimAi_Init(ArRegionalSimAiPolicy *policy,ArRegionalSource source);
bool ArRegionalSimAi_Resolve(const ArRegionalSimAiPolicy *policy,ArRegionalSimAiSnapshot *snapshot);
bool ArRegionalSimAi_GroupSource(const ArRegionalSimAiPolicy *policy,ArRegionalSource *source);
bool ArRegionalSimAi_Value(ArRegionalSimAiSnapshot snapshot, ArRegionalSimAiRule rule,
                           uint16_t *value);
#endif
