#ifndef AR_REGIONAL_ACTION_MOTION_H
#define AR_REGIONAL_ACTION_MOTION_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArRegionalActionMotionRule {
  kArRegionalActionMotion_BirdSpeed,
  kArRegionalActionMotion_LeaperSpeed,
  kArRegionalActionMotion_CaveRecovery,
  kArRegionalActionMotion_CaveStraightRecovery,
  kArRegionalActionMotion_CaveHighRecovery,
  kArRegionalActionMotion_CasterLowWindup,
  kArRegionalActionMotion_CasterHighWindup,
  kArRegionalActionMotion_SwordStraightRecovery,
  kArRegionalActionMotion_SwordHighRecovery,
  kArRegionalActionMotion_ArrowSpeed,
  kArRegionalActionMotion_WallHeadShortHold,
  kArRegionalActionMotion_WallHeadLongHold,
  kArRegionalActionMotion_Count
} ArRegionalActionMotionRule;
typedef enum ArRegionalActionMotionFamily {
  kArRegionalActionMotion_Bird,
  kArRegionalActionMotion_Leaper,
  kArRegionalActionMotion_Cave,
  kArRegionalActionMotion_CaveAttacker,
  kArRegionalActionMotion_Caster,
  kArRegionalActionMotion_Swordsman,
  kArRegionalActionMotion_Arrow,
  kArRegionalActionMotionFamily_Count
} ArRegionalActionMotionFamily;
typedef struct ArRegionalActionMotionPolicy { ArRegionalSource source[kArRegionalActionMotion_Count]; } ArRegionalActionMotionPolicy;
/* Numerical room snapshot: one bit per independently selectable JP rule.
 * No native pointers, actor memory, artwork or host lifecycle in this layer. */
typedef uint16_t ArRegionalActionMotionSnapshot;
typedef struct ArRegionalActionMotionDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
  /* Fixed remainder of the measured phase, for human-readable total updates.
   * Zero for velocity rules; it is not part of the native row or wire value. */
  uint16_t phase_extra_updates;
} ArRegionalActionMotionDescriptor;
const ArRegionalActionMotionDescriptor *ArRegionalActionMotion_Descriptor(ArRegionalActionMotionRule rule);
bool ArRegionalActionMotion_Init(ArRegionalActionMotionPolicy *policy,ArRegionalSource source);
bool ArRegionalActionMotion_Resolve(const ArRegionalActionMotionPolicy *policy,ArRegionalActionMotionSnapshot *snapshot);
bool ArRegionalActionMotion_GroupSource(const ArRegionalActionMotionPolicy *policy,ArRegionalSource *source);
/* Only the audited numerical row members change. Duration is the stored row
 * delay, not total phase length. DX is signed, before native facing mirroring.
 * Unknown/mismatched rows are rejected without changing the caller's values. */
bool ArRegionalActionMotion_Row(ArRegionalActionMotionSnapshot snapshot,
    ArRegionalActionMotionFamily family,unsigned state,unsigned row,
    uint16_t *duration,int16_t *dx);
#endif
