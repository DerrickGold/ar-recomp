#ifndef AR_REGIONAL_SIM_COMBAT_H
#define AR_REGIONAL_SIM_COMBAT_H
#include "regional_source.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArRegionalSimCombatRule {
  kArRegionalSimCombat_DragonDurability,
  kArRegionalSimCombat_DemonDurability,
  kArRegionalSimCombat_DragonContact,
  kArRegionalSimCombat_DemonContact,
  kArRegionalSimCombat_SkullContact,
  kArRegionalSimCombat_Count
} ArRegionalSimCombatRule;
typedef struct ArRegionalSimCombatPolicy { ArRegionalSource source[kArRegionalSimCombat_Count]; } ArRegionalSimCombatPolicy;
typedef struct ArRegionalSimCombatDescriptor { const char *key; uint16_t value[kArRegionalSource_Count]; } ArRegionalSimCombatDescriptor;
/* A numeric snapshot: bit positions are explicitly versioned by the actor
 * codec. Zero means US behavior, including actors from older checkpoints. */
typedef uint16_t ArRegionalSimCombatSnapshot;
const ArRegionalSimCombatDescriptor *ArRegionalSimCombat_Descriptor(ArRegionalSimCombatRule rule);
bool ArRegionalSimCombat_Init(ArRegionalSimCombatPolicy *policy, ArRegionalSource source);
bool ArRegionalSimCombat_Resolve(const ArRegionalSimCombatPolicy *policy, ArRegionalSimCombatSnapshot *snapshot);
bool ArRegionalSimCombat_GroupSource(const ArRegionalSimCombatPolicy *policy, ArRegionalSource *source);
bool ArRegionalSimCombat_Values(ArRegionalSimCombatSnapshot snapshot, unsigned species,
                              uint8_t *threshold, uint8_t *contact);

#endif
