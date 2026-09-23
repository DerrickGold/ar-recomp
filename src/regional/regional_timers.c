#include "regional_timers.h"

#include <stddef.h>

/* The 43 used action records have the same pre-timer fields in all five
 * releases. Only these six initial times differ (US/EU/DE/FR versus JP).
 * Profile24 is shared by Marahna section8 and the Death Heim Viper rematch.
 * Source: docs/regional-differences-technical.md#action-timer-profiles. */
static const ArRegionalTimerDescriptor kRules[kArRegionalTimerRule_Count] = {
  {"time_fillmore_section_1", 0x03, {0x0300, 0x0200, 0x0300}},
  {"time_fillmore_section_3", 0x05, {0x0200, 0x0100, 0x0200}},
  {"time_bloodpool_section_1", 0x07, {0x0300, 0x0200, 0x0300}},
  {"time_marahna_boss",       0x24, {0x0300, 0x0200, 0x0300}},
  {"time_northwall_section_1",0x25, {0x0200, 0x0100, 0x0200}},
  {"time_northwall_section_2",0x26, {0x0200, 0x0100, 0x0200}},
};

const ArRegionalTimerDescriptor *ArRegionalTimers_Descriptor(ArRegionalTimerRule rule) {
  return (unsigned)rule < kArRegionalTimerRule_Count ? &kRules[rule] : NULL;
}

bool ArRegionalTimers_Valid(const ArRegionalTimerPolicy *policy) {
  if (!policy) return false;
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i)
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
  return true;
}

bool ArRegionalTimers_Init(ArRegionalTimerPolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) policy->source[i] = source;
  return true;
}

bool ArRegionalTimers_SetRule(ArRegionalTimerPolicy *policy, ArRegionalTimerRule rule,
                              ArRegionalSource source) {
  if (!ArRegionalTimers_Valid(policy) || (unsigned)rule >= kArRegionalTimerRule_Count ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  policy->source[rule] = source;
  return true;
}

bool ArRegionalTimers_GroupSource(const ArRegionalTimerPolicy *policy, ArRegionalSource *source) {
  if (!source || !ArRegionalTimers_Valid(policy)) return false;
  bool uniform = true;
  for (unsigned i = 1; i < kArRegionalTimerRule_Count; ++i)
    uniform &= policy->source[i] == policy->source[0];
  if (uniform) { *source = policy->source[0]; return true; }
  for (unsigned candidate = 0; candidate < kArRegionalSource_Count; ++candidate) {
    bool equal = true;
    for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i)
      equal &= kRules[i].bcd[policy->source[i]] == kRules[i].bcd[candidate];
    if (equal) { *source = (ArRegionalSource)candidate; return true; }
  }
  return false;
}

bool ArRegionalTimers_Resolve(const ArRegionalTimerPolicy *policy, uint8_t us_profile,
                              uint16_t native_bcd, uint16_t *resolved_bcd) {
  if (!resolved_bcd || !ArRegionalTimers_Valid(policy)) return false;
  uint16_t value = native_bcd;
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) {
    const ArRegionalTimerDescriptor *rule = &kRules[i];
    if (rule->us_profile != us_profile ||
        rule->bcd[policy->source[i]] == rule->bcd[kArRegionalSource_US]) continue;
    if (native_bcd != rule->bcd[kArRegionalSource_US]) return false;
    value = rule->bcd[policy->source[i]];
    break;
  }
  *resolved_bcd = value;
  return true;
}
