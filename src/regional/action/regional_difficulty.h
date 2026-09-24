#ifndef AR_REGIONAL_DIFFICULTY_H
#define AR_REGIONAL_DIFFICULTY_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Host choices, not PAL WRAM values. Zero preserves older sessions. */
typedef enum ArRegionalDifficulty {
  kArRegionalDifficulty_Normal=0,
  kArRegionalDifficulty_Beginner=1,
  kArRegionalDifficulty_Expert=2,
  kArRegionalDifficulty_Count
} ArRegionalDifficulty;
typedef enum ArRegionalDifficultyRule {
  kArRegionalDifficulty_SpawnHp,
  kArRegionalDifficulty_Contact,
  kArRegionalDifficulty_Countdown,
  kArRegionalDifficulty_DragonAttack,
  kArRegionalDifficulty_PlantTendril,
  kArRegionalDifficultyRule_Count
} ArRegionalDifficultyRule;
typedef struct ArRegionalDifficultyPolicy {
  ArRegionalSource source[kArRegionalDifficultyRule_Count];
  ArRegionalDifficulty level;
} ArRegionalDifficultyPolicy;
/* Player-facing selection. Original disables the European adjustments; it is
 * not EU Normal. Custom is a read-only summary for existing partial policies.
 * Stored policies and replay IDs keep their existing representation. */
typedef enum ArRegionalDifficultyChoice {
  kArRegionalDifficultyChoice_Original,
  kArRegionalDifficultyChoice_Beginner,
  kArRegionalDifficultyChoice_Normal,
  kArRegionalDifficultyChoice_Expert,
  kArRegionalDifficultyChoice_Count,
  kArRegionalDifficultyChoice_Custom = kArRegionalDifficultyChoice_Count,
} ArRegionalDifficultyChoice;
ArRegionalDifficultyChoice ArRegionalDifficulty_Choice(const ArRegionalDifficultyPolicy *policy);
bool ArRegionalDifficulty_Select(ArRegionalDifficultyChoice choice, ArRegionalDifficultyPolicy *out);
typedef struct ArRegionalDifficultyDescriptor {
  const char *key;
  uint16_t value[kArRegionalSource_Count];
} ArRegionalDifficultyDescriptor;
/* Canonical room-owned values. A zero snapshot delegates every US path.
 * HP mode: native=0, PAL Normal=1, Beginner=2, Expert=3. */
typedef struct ArRegionalDifficultySnapshot {
  uint8_t spawn_hp, contact_extra, timer_reload;
  bool skip_dragon_attack, single_tendril_bob;
} ArRegionalDifficultySnapshot;
const ArRegionalDifficultyDescriptor *ArRegionalDifficulty_Descriptor(unsigned rule);
bool ArRegionalDifficulty_Init(ArRegionalDifficultyPolicy *policy, ArRegionalSource source,
    ArRegionalDifficulty level);
bool ArRegionalDifficulty_Resolve(const ArRegionalDifficultyPolicy *policy,
    ArRegionalDifficultySnapshot *snapshot);
bool ArRegionalDifficulty_GroupSource(const ArRegionalDifficultyPolicy *policy, ArRegionalSource *source);
/* Stable semantic encoding used for replay identity; does not encode labels. */
uint8_t ArRegionalDifficulty_Identity(const ArRegionalDifficultySnapshot *snapshot);
/* Transforms selected base stats at birth. Native mode promotion remains the
 * original routine when spawn_hp is zero; PAL changes HP only, never attack. */
uint16_t ArRegionalDifficulty_SpawnHp(const ArRegionalDifficultySnapshot *snapshot,
    uint16_t flags, uint16_t hp);

#endif
