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
#endif
