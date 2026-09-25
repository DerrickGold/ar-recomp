/* RegionalFamilies: the one inventory of regional rule families.
 * Phase: pure. Each row names where a family's ArRegionalSource fields sit in
 * ArRegionalRules and how to read each rule's stable key and per-region
 * values. The session codec, fingerprint, profiles and settings readout read
 * this table instead of keeping their own lists.
 * Must not: hold state or decide any order. Consumers keep their own frozen
 * orders (save wire order, fingerprint layers) as data referencing families.
 * Tests: tests/regional_families_test.c, tests/regional_characterization_test.c */
#ifndef AR_REGIONAL_FAMILIES_H
#define AR_REGIONAL_FAMILIES_H
#include "regional/regional_rules.h"

#include <stddef.h>
#include <stdint.h>

/* One row per ArRegionalRules field, in declaration order. */
typedef enum ArRegionalFamilyId {
  kArRegionalFamily_Hazards,
  kArRegionalFamily_Terrain,
  kArRegionalFamily_Music,
  kArRegionalFamily_Sequences,
  kArRegionalFamily_Mosaic,
  kArRegionalFamily_Poses,
  kArRegionalFamily_Artwork,
  kArRegionalFamily_ActorArtwork,
  kArRegionalFamily_Placements,
  kArRegionalFamily_ModeEntry,
  kArRegionalFamily_Costs,
  kArRegionalFamily_Timers,
  kArRegionalFamily_RetryScore,
  kArRegionalFamily_TownWait,
  kArRegionalFamily_Fishing,
  kArRegionalFamily_Development,
  kArRegionalFamily_Recovery,
  kArRegionalFamily_Quake,
  kArRegionalFamily_ScorePage,
  kArRegionalFamily_MenuReturn,
  kArRegionalFamily_SpeedRange,
  kArRegionalFamily_MagicGesture,
  kArRegionalFamily_LairSeeds,
  kArRegionalFamily_HouseCredit,
  kArRegionalFamily_ScoreFeedback,
  kArRegionalFamily_LivesDisplay,
  kArRegionalFamily_Sources,
  kArRegionalFamily_SkullWait,
  kArRegionalFamily_Story,
  kArRegionalFamily_LairReloads,
  kArRegionalFamily_TownStatus,
  kArRegionalFamily_LevelGoals,
  kArRegionalFamily_SimCombat,
  kArRegionalFamily_SimAi,
  kArRegionalFamily_Construction,
  kArRegionalFamily_Support,
  kArRegionalFamily_Arrival,
  kArRegionalFamily_ActionMotion,
  kArRegionalFamily_Emitters,
  kArRegionalFamily_StatueVolley,
  kArRegionalFamily_Bosses,
  kArRegionalFamily_Collision,
  kArRegionalFamily_PlatformSkull,
  kArRegionalFamily_ActorStats,
  kArRegionalFamily_CastHold,
  kArRegionalFamily_FireEnemy,
  kArRegionalFamily_Difficulty,
  kArRegionalFamily_ScoreLives,
  kArRegionalFamily_ActionStart,
  kArRegionalFamily_SpellInventory,
  kArRegionalFamily_Count,
} ArRegionalFamilyId;

typedef struct ArRegionalRuleInfo {
  const char *key;          /* stable persisted key */
  const uint16_t *values;   /* indexed by ArRegionalSource */
} ArRegionalRuleInfo;

typedef struct ArRegionalFamily {
  const char *name;
  size_t offset;            /* first ArRegionalSource of the family */
  unsigned slots;           /* consecutive ArRegionalSource fields */
  /* Rule entries. Equal to slots, except the lair seeds, whose 24 per-lair
   * rules all read one shared field. */
  unsigned rules;
  ArRegionalRuleInfo (*info)(unsigned rule);
  /* The whole family summarized as one source, with the family's own rules
   * for aliases and mixed values (its _GroupSource); NULL when it has none.
   * False, or kArRegionalSource_Count, means Custom. */
  bool (*group_source)(const ArRegionalRules *rules, ArRegionalSource *source);
} ArRegionalFamily;

const ArRegionalFamily *ArRegionalFamilies_Get(ArRegionalFamilyId family);
/* The field holding `rule`'s source. The caller keeps rule < family rules. */
ArRegionalSource *ArRegionalFamilies_Field(ArRegionalRules *rules, ArRegionalFamilyId family,
                                           unsigned rule);
ArRegionalSource ArRegionalFamilies_Source(const ArRegionalRules *rules,
                                           ArRegionalFamilyId family, unsigned rule);
/* Every source field of every family names a real region. Cross-family rules
 * (population compatibility, lair accounting) stay with their consumers. */
bool ArRegionalFamilies_SourcesValid(const ArRegionalRules *rules);
#endif
