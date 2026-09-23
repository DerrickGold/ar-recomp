#include "regional_costs.h"

#include <stddef.h>

/* Evidence: docs/regional-differences-technical.md, "Action rules in code",
 * "Miracle costs", "European items and spell inventory" and "European
 * simulation numeric rules". All European difficulties share these prices;
 * this does not equate their inventory, enemies or other gameplay rules. */
static const ArRegionalCostDescriptor kCosts[kArRegionalCostRule_Count] = {
  [kArRegionalCost_Fire]       = {"scroll_fire",       kArRegionalCostGroup_Scrolls,  {1, 1, 1}},
  [kArRegionalCost_Stardust]   = {"scroll_stardust",   kArRegionalCostGroup_Scrolls,  {1, 2, 1}},
  [kArRegionalCost_Aura]       = {"scroll_aura",       kArRegionalCostGroup_Scrolls,  {1, 3, 1}},
  [kArRegionalCost_Light]      = {"scroll_light",      kArRegionalCostGroup_Scrolls,  {1, 4, 1}},
  [kArRegionalCost_Lightning]  = {"miracle_lightning", kArRegionalCostGroup_Miracles, {10, 12, 10}},
  [kArRegionalCost_Rain]       = {"miracle_rain",      kArRegionalCostGroup_Miracles, {20, 16, 20}},
  [kArRegionalCost_Sunlight]   = {"miracle_sunlight",  kArRegionalCostGroup_Miracles, {30, 18, 30}},
  [kArRegionalCost_Wind]       = {"miracle_wind",      kArRegionalCostGroup_Miracles, {80, 24, 80}},
  [kArRegionalCost_Earthquake] = {"miracle_earthquake", kArRegionalCostGroup_Miracles, {160, 60, 160}},
};

_Static_assert(kArRegionalCostRule_Count <= 16, "cost preview mask capacity");

static bool ValidSource(ArRegionalSource source) {
  return (unsigned)source < kArRegionalSource_Count;
}

static bool ValidPolicy(const ArRegionalCostPolicy *policy) {
  if (!policy) return false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    if (!ValidSource(policy->source[i])) return false;
  return true;
}

const ArRegionalCostDescriptor *ArRegionalCosts_Descriptor(ArRegionalCostRule rule) {
  return (unsigned)rule < kArRegionalCostRule_Count ? &kCosts[rule] : NULL;
}

bool ArRegionalCosts_Init(ArRegionalCostPolicy *policy, ArRegionalSource source) {
  if (!policy || !ValidSource(source)) return false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) policy->source[i] = source;
  return true;
}

bool ArRegionalCosts_SetRule(ArRegionalCostPolicy *policy, ArRegionalCostRule rule,
                             ArRegionalSource source) {
  if (!ValidPolicy(policy) || !ArRegionalCosts_Descriptor(rule) || !ValidSource(source))
    return false;
  policy->source[rule] = source;
  return true;
}

bool ArRegionalCosts_SetGroup(ArRegionalCostPolicy *policy, ArRegionalCostGroup group,
                              ArRegionalSource source) {
  if (!ValidPolicy(policy) || (unsigned)group >= kArRegionalCostGroup_Count ||
      !ValidSource(source)) return false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    if (kCosts[i].group == group) policy->source[i] = source;
  return true;
}

bool ArRegionalCosts_Resolve(const ArRegionalCostPolicy *policy,
                             ArRegionalCostSnapshot *snapshot) {
  if (!ValidPolicy(policy) || !snapshot) return false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    snapshot->price[i] = kCosts[i].price[policy->source[i]];
  return true;
}

bool ArRegionalCosts_Preview(const ArRegionalCostPolicy *current,
                             const ArRegionalCostPolicy *requested,
                             ArRegionalCostPreview *preview) {
  ArRegionalCostPreview next = {0};
  if (!preview || !ArRegionalCosts_Resolve(current, &next.current) ||
      !ArRegionalCosts_Resolve(requested, &next.requested)) return false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    if (current->source[i] != requested->source[i]) next.source_changes |= (uint16_t)(1u << i);
    if (next.current.price[i] != next.requested.price[i]) next.price_changes |= (uint16_t)(1u << i);
  }
  *preview = next;
  return true;
}

bool ArRegionalCosts_GroupSource(const ArRegionalCostPolicy *policy,
                                 ArRegionalCostGroup group,
                                 ArRegionalSource *source) {
  if (!ValidPolicy(policy) || !source || (unsigned)group >= kArRegionalCostGroup_Count)
    return false;
  ArRegionalSource found = kArRegionalSource_Count;
  bool uniform = true;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    if (kCosts[i].group != group) continue;
    if (found != kArRegionalSource_Count && found != policy->source[i]) uniform = false;
    found = policy->source[i];
  }
  if (uniform) {
    *source = found;
    return true;
  }
  for (unsigned candidate = 0; candidate < kArRegionalSource_Count; ++candidate) {
    bool matches = true;
    for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
      if (kCosts[i].group == group &&
          kCosts[i].price[policy->source[i]] != kCosts[i].price[candidate]) matches = false;
    }
    if (matches) {
      *source = (ArRegionalSource)candidate;
      return true;
    }
  }
  return false;
}
