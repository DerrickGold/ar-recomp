#ifndef AR_REGIONAL_ACTOR_STATS_H
#define AR_REGIONAL_ACTOR_STATS_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalActorStatField { kArRegionalActorStat_HP, kArRegionalActorStat_Attack, kArRegionalActorStat_Reward } ArRegionalActorStatField;
enum {
  /* Frozen v39 base-record/replay order. Child IDs are semantic owners, not
   * placed type-table keys and must never enter the base-stat projection. */
  kArRegionalActorStat_BaseCount=63,
  kArRegionalActorStat_TanzraMinionHp=63,
  kArRegionalActorStat_TanzraMinionReward=64,
  kArRegionalActorStat_TanzraProjectileAttack=65,
  kArRegionalActorStat_Count=0
#define AR_ACTOR_STAT(key,actor,field,us,jp,eu) +1
#include "regional_actor_stats_data.inc"
#undef AR_ACTOR_STAT
};
typedef struct ArRegionalActorStatsPolicy { ArRegionalSource source[kArRegionalActorStat_Count]; } ArRegionalActorStatsPolicy;
typedef struct ArRegionalActorStatsSnapshot {
  uint8_t value[kArRegionalActorStat_Count];
  bool changed; /* Base-record fields only; explicit child fields are separate. */
} ArRegionalActorStatsSnapshot;
typedef struct ArRegionalActorStatDescriptor {
  const char *key;
  uint16_t actor;
  ArRegionalActorStatField field;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalActorStatDescriptor;
const ArRegionalActorStatDescriptor *ArRegionalActorStats_Descriptor(unsigned rule);
bool ArRegionalActorStats_Init(ArRegionalActorStatsPolicy *policy,ArRegionalSource source);
bool ArRegionalActorStats_Resolve(const ArRegionalActorStatsPolicy *policy,ArRegionalActorStatsSnapshot *snapshot);
bool ArRegionalActorStats_GroupSource(const ArRegionalActorStatsPolicy *policy,ArRegionalSource *source);
/* Validated cached snapshot, semantic area/type, and freshly copied US fields.
 * Return false without outputs on baseline/unmapped/incompatible input. This
 * never promotes difficulty, heals a running actor or edits its death reward. */
bool ArRegionalActorStats_Apply(const ArRegionalActorStatsSnapshot *snapshot,uint16_t actor,
    uint16_t native_hp,uint16_t native_attack,uint16_t *hp,uint16_t *attack);
#endif
