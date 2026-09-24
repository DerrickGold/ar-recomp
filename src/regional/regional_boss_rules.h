#ifndef AR_REGIONAL_BOSS_RULES_H
#define AR_REGIONAL_BOSS_RULES_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalBossRule {
  kArRegionalBoss_MinoIdle,
  kArRegionalBoss_MinoThrowEnd,
  kArRegionalBoss_MinoThrowWindup,
  kArRegionalBoss_MinoJumpWindup,
  kArRegionalBoss_MinoAxeOffset,
  kArRegionalBoss_WizardPause,
  kArRegionalBoss_IceWindup,
  kArRegionalBoss_TanzraClosing,
  kArRegionalBoss_TanzraClock,
  kArRegionalBoss_TanzraUpperTurn,
  kArRegionalBoss_TanzraMinionTurn,
  kArRegionalBoss_AntlionTrigger,
  kArRegionalBoss_AntlionStrategy,
  kArRegionalBoss_DragonProjectileDelay,
  kArRegionalBoss_DragonProjectileFlight,
  kArRegionalBoss_ViperChoice,
  kArRegionalBoss_ViperLightning,
  kArRegionalBoss_ViperRematchLightning,
  kArRegionalBoss_ViperFloor,
  kArRegionalBoss_PharaohLanding,
  kArRegionalBoss_PharaohRematchLanding,
  kArRegionalBoss_PharaohHeads,
  kArRegionalBoss_PlantCycle,
  kArRegionalBoss_PlantOpen,
  kArRegionalBoss_PlantHighWindup,
  kArRegionalBoss_PlantLowWindup,
  kArRegionalBoss_NorthwallThrow,
  kArRegionalBoss_NorthwallImpact,
  kArRegionalBoss_NorthwallThrowOffset,
  kArRegionalBoss_NorthwallImpactOffset,
  kArRegionalBoss_PlantGeometry,
  kArRegionalBoss_Count
} ArRegionalBossRule;
typedef struct ArRegionalBossPolicy { ArRegionalSource source[kArRegionalBoss_Count]; } ArRegionalBossPolicy;
/* Two canonical source bits per semantic rule; numeric aliases use the first
 * matching source. Stable ordinals preserve older replay domains when rules
 * are appended. No native pointers or encounter-local state. */
typedef uint64_t ArRegionalBossSnapshot;
typedef struct ArRegionalBossDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
  uint16_t phase_extra_updates;
} ArRegionalBossDescriptor;
const ArRegionalBossDescriptor *ArRegionalBoss_Descriptor(unsigned rule);
bool ArRegionalBoss_Init(ArRegionalBossPolicy *policy, ArRegionalSource source);
bool ArRegionalBoss_Resolve(const ArRegionalBossPolicy *policy, ArRegionalBossSnapshot *snapshot);
bool ArRegionalBoss_GroupSource(const ArRegionalBossPolicy *policy, ArRegionalSource *source);
/* UINT16_MAX denotes out-of-range bits/rules or an invalid selected source.
 * Runtime snapshots have already been validated at the room boundary.
 * Durations are stored delay values. */
uint16_t ArRegionalBoss_Value(ArRegionalBossSnapshot snapshot, unsigned rule);
bool ArRegionalBoss_MinoRow(ArRegionalBossSnapshot snapshot, unsigned state,
    unsigned row, uint16_t *duration, int16_t dx, int16_t dy);
/* One coherent head/body policy; not two independently desynchronizable
 * sequence edits. Caller proves rematch ownership and the US row shape. */
bool ArRegionalBoss_IceSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next);
bool ArRegionalBoss_TanzraRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint16_t *duration,int16_t dx,int16_t dy);
bool ArRegionalBoss_TanzraMinionSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next);
bool ArRegionalBoss_DragonRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t dy);
bool ArRegionalBoss_ViperRow(ArRegionalBossSnapshot snapshot,bool rematch,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t *dy);
bool ArRegionalBoss_PharaohSkip(ArRegionalBossSnapshot snapshot,bool rematch,
    unsigned state,unsigned row,unsigned *next);
typedef struct ArRegionalPlantPhase { uint8_t state,repetitions; } ArRegionalPlantPhase;
bool ArRegionalBoss_PlantPhase(ArRegionalBossSnapshot snapshot,unsigned previous,ArRegionalPlantPhase *next);
bool ArRegionalBoss_PlantOpenRow(ArRegionalBossSnapshot snapshot,unsigned row,unsigned *native_row,uint8_t *visual);
bool ArRegionalBoss_PlantWindup(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t dy);
/* Northwall Act 1: expanded impact rows reuse US row5's tiles. The caller
 * owns native composition encoding; expansion is 1..4 only for added poses. */
bool ArRegionalBoss_NorthwallRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    unsigned *native_row,uint16_t *duration,unsigned *expansion,bool *end);
#endif
