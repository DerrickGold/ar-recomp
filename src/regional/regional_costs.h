#ifndef AR_REGIONAL_COSTS_H
#define AR_REGIONAL_COSTS_H

#include <stdbool.h>
#include <stdint.h>

/* Game-owned, renderer/platform/ROM-independent policy. This is the pricing
 * subset, NOT a complete regional preset. No locale, donor availability,
 * clock rate, inventory conversion or persistence is inferred here. */
typedef enum ArRegionalCostSource {
  kArRegionalCost_US,
  kArRegionalCost_Japan,
  kArRegionalCost_Europe,
  kArRegionalCostSource_Count,
} ArRegionalCostSource;

typedef enum ArRegionalCostGroup {
  kArRegionalCostGroup_Scrolls,
  kArRegionalCostGroup_Miracles,
  kArRegionalCostGroup_Count,
} ArRegionalCostGroup;

/* Semantic IDs, not native spell IDs, miracle dispatch order or WRAM offsets.
 * Persist descriptor keys rather than enum ordinals when storage is wired. */
typedef enum ArRegionalCostRule {
  kArRegionalCost_Fire,
  kArRegionalCost_Stardust,
  kArRegionalCost_Aura,
  kArRegionalCost_Light,
  kArRegionalCost_Lightning,
  kArRegionalCost_Rain,
  kArRegionalCost_Sunlight,
  kArRegionalCost_Wind,
  kArRegionalCost_Earthquake,
  kArRegionalCostRule_Count,
} ArRegionalCostRule;

typedef struct ArRegionalCostDescriptor {
  const char *key;
  ArRegionalCostGroup group;
  uint16_t price[kArRegionalCostSource_Count];
} ArRegionalCostDescriptor;

typedef struct ArRegionalCostPolicy {
  ArRegionalCostSource source[kArRegionalCostRule_Count];
} ArRegionalCostPolicy;

/* Resolve once on load/change. A transaction captures its price before the
 * affordability check; menu quote, gate and debit must use the same snapshot.
 * These scroll prices apply only to GENERIC scrolls. Europe's Action-mode
 * LIFO inventory/payment lifecycle is a different, not-yet-integrated policy. */
typedef struct ArRegionalCostSnapshot {
  uint16_t price[kArRegionalCostRule_Count];
} ArRegionalCostSnapshot;

typedef struct ArRegionalCostPreview {
  ArRegionalCostSnapshot current, requested;
  uint16_t source_changes;
  uint16_t price_changes;
} ArRegionalCostPreview;

const ArRegionalCostDescriptor *ArRegionalCosts_Descriptor(ArRegionalCostRule rule);
/* All fallible operations leave outputs unchanged on invalid inputs. Policies
 * are caller-owned: separate saves/sessions must never share mutable state. */
bool ArRegionalCosts_Init(ArRegionalCostPolicy *policy, ArRegionalCostSource source);
bool ArRegionalCosts_SetRule(ArRegionalCostPolicy *policy, ArRegionalCostRule rule,
                             ArRegionalCostSource source);
bool ArRegionalCosts_SetGroup(ArRegionalCostPolicy *policy, ArRegionalCostGroup group,
                              ArRegionalCostSource source);
bool ArRegionalCosts_Resolve(const ArRegionalCostPolicy *policy,
                             ArRegionalCostSnapshot *snapshot);
/* Preview only. It cannot activate a rule or mutate the current transaction.
 * US -> Europe can change provenance without changing an effective price. */
bool ArRegionalCosts_Preview(const ArRegionalCostPolicy *current,
                             const ArRegionalCostPolicy *requested,
                             ArRegionalCostPreview *preview);
/* Summary for a group: retain a uniform requested source; otherwise match
 * resolved prices to a known source (US first for equivalent tables). False
 * for invalid policies or a genuinely Custom price mix. Per-leaf provenance
 * remains in policy; numerically identical selections aren't called Custom. */
bool ArRegionalCosts_GroupSource(const ArRegionalCostPolicy *policy,
                                 ArRegionalCostGroup group,
                                 ArRegionalCostSource *source);

#endif
