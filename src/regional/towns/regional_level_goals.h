#ifndef AR_REGIONAL_LEVEL_GOALS_H
#define AR_REGIONAL_LEVEL_GOALS_H
/* RegionalLevelGoals: the experience goal for each level (Japanese or Western table), per
 * region.
 * Phase: pure.
 * Tests: tests/actraiser_level_goals_test.c */
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

enum { kArRegionalLevelGoals_Count = 18, kArRegionalLevelGoals_MaxLevel = 17 };
typedef struct ArRegionalLevelGoalsDescriptor {
  const char *key;
  uint16_t japanese[kArRegionalSource_Count];
} ArRegionalLevelGoalsDescriptor;
const ArRegionalLevelGoalsDescriptor *ArRegionalLevelGoals_Descriptor(void);
bool ArRegionalLevelGoals_Resolve(ArRegionalSource source, bool *japanese);
/* Native level is the table index: entry 17 is the 9999 sentinel. */
bool ArRegionalLevelGoals_Threshold(bool japanese, unsigned level, uint16_t *threshold);
/* The Master report shows zero rather than the sentinel at maximum level. */
bool ArRegionalLevelGoals_Display(bool japanese, unsigned level, uint16_t *threshold);
#endif
