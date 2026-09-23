#ifndef AR_REGIONAL_TIMERS_H
#define AR_REGIONAL_TIMERS_H

#include <stdbool.h>
#include <stdint.h>
#include "regional_source.h"

/* Initial displayed time only. Does not control tick cadence, PAL refresh,
 * difficulty, pause gates, stage-clear conversion, or a clock already running. */
typedef enum ArRegionalTimerRule {
  kArRegionalTimer_FillmoreSection1,
  kArRegionalTimer_FillmoreSection3,
  kArRegionalTimer_BloodpoolSection1,
  kArRegionalTimer_MarahnaBoss,
  kArRegionalTimer_NorthwallSection1,
  kArRegionalTimer_NorthwallSection2,
  kArRegionalTimerRule_Count,
} ArRegionalTimerRule;

typedef struct ArRegionalTimerDescriptor {
  const char *key;
  uint8_t us_profile;
  uint16_t bcd[kArRegionalSource_Count];
} ArRegionalTimerDescriptor;

typedef struct ArRegionalTimerPolicy {
  ArRegionalSource source[kArRegionalTimerRule_Count];
} ArRegionalTimerPolicy;

const ArRegionalTimerDescriptor *ArRegionalTimers_Descriptor(ArRegionalTimerRule rule);
bool ArRegionalTimers_Valid(const ArRegionalTimerPolicy *policy);
bool ArRegionalTimers_Init(ArRegionalTimerPolicy *policy, ArRegionalSource source);
bool ArRegionalTimers_SetRule(ArRegionalTimerPolicy *policy, ArRegionalTimerRule rule,
                              ArRegionalSource source);
bool ArRegionalTimers_GroupSource(const ArRegionalTimerPolicy *policy, ArRegionalSource *source);
/* Called only at native action-profile timer initialization. Unchanged profiles
 * and numerically baseline choices retain the native field exactly. A changed
 * choice requires the expected baseline field, so patched/unknown data cannot
 * silently receive an unrelated override. Failure leaves output untouched. */
bool ArRegionalTimers_Resolve(const ArRegionalTimerPolicy *policy, uint8_t us_profile,
                              uint16_t native_bcd, uint16_t *resolved_bcd);

#endif
