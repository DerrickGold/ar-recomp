#include "regional/regional_families.h"

/* One accessor per family: the rule's stable key and its per-region values.
 * Written out rather than generated so each can be found by name. */

static ArRegionalRuleInfo HazardsRule(unsigned rule) {
  (void)rule;
  const ArRegionalHazardDescriptor *d = ArRegionalHazards_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo TerrainRule(unsigned rule) {
  (void)rule;
  const ArRegionalTerrainDescriptor *d = ArRegionalTerrain_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo MusicRule(unsigned rule) {
  (void)rule;
  const ArRegionalMusicDescriptor *d = ArRegionalMusic_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo SequencesRule(unsigned rule) {
  const ArRegionalMusicDescriptor *d = ArRegionalSequences_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo MosaicRule(unsigned rule) {
  (void)rule;
  const ArRegionalMosaicDescriptor *d = ArRegionalMosaic_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo PosesRule(unsigned rule) {
  const ArRegionalPoseDescriptor *d = ArRegionalPoses_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo ArtworkRule(unsigned rule) {
  const ArRegionalArtworkDescriptor *d = ArRegionalArtwork_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo ActorArtworkRule(unsigned rule) {
  const ArRegionalArtworkDescriptor *d = ArRegionalActorArtwork_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo PlacementsRule(unsigned rule) {
  const ArRegionalPlacementDescriptor *d = ArRegionalPlacements_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->profile};
}
static ArRegionalRuleInfo ModeEntryRule(unsigned rule) {
  const ArRegionalModeDescriptor *d = ArRegionalMode_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo CostsRule(unsigned rule) {
  const ArRegionalCostDescriptor *d = ArRegionalCosts_Descriptor((ArRegionalCostRule)rule);
  return (ArRegionalRuleInfo){d->key, d->price};
}
static ArRegionalRuleInfo TimersRule(unsigned rule) {
  const ArRegionalTimerDescriptor *d = ArRegionalTimers_Descriptor((ArRegionalTimerRule)rule);
  return (ArRegionalRuleInfo){d->key, d->bcd};
}
static ArRegionalRuleInfo RetryScoreRule(unsigned rule) {
  (void)rule;
  const ArRegionalRetryDescriptor *d = ArRegionalRetry_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->clear_score};
}
static ArRegionalRuleInfo TownWaitRule(unsigned rule) {
  (void)rule;
  const ArRegionalTownWaitDescriptor *d = ArRegionalTownWait_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->updates};
}
static ArRegionalRuleInfo FishingRule(unsigned rule) {
  (void)rule;
  const ArRegionalFishingDescriptor *d = ArRegionalFishing_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->updates};
}
static ArRegionalRuleInfo DevelopmentRule(unsigned rule) {
  const ArRegionalDevelopmentDescriptor *d =
      ArRegionalDevelopment_Descriptor((ArRegionalDevelopmentRule)rule);
  return (ArRegionalRuleInfo){d->key, d->updates};
}
static ArRegionalRuleInfo RecoveryRule(unsigned rule) {
  const ArRegionalRecoveryDescriptor *d =
      ArRegionalRecovery_Descriptor((ArRegionalRecoveryRule)rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo QuakeRule(unsigned rule) {
  const ArRegionalQuakeDescriptor *d = ArRegionalQuake_Descriptor((ArRegionalQuakeRule)rule);
  return (ArRegionalRuleInfo){d->key, d->random};
}
static ArRegionalRuleInfo ScorePageRule(unsigned rule) {
  (void)rule;
  const ArRegionalScorePageDescriptor *d = ArRegionalScorePage_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo MenuReturnRule(unsigned rule) {
  (void)rule;
  const ArRegionalMenuReturnDescriptor *d = ArRegionalMenuReturn_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->keep_open};
}
static ArRegionalRuleInfo SpeedRangeRule(unsigned rule) {
  (void)rule;
  const ArRegionalSpeedRangeDescriptor *d = ArRegionalSpeedRange_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->maximum};
}
static ArRegionalRuleInfo MagicGestureRule(unsigned rule) {
  (void)rule;
  const ArRegionalMagicGestureDescriptor *d = ArRegionalMagicGesture_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->up_attack};
}
static ArRegionalRuleInfo LairSeedsRule(unsigned rule) {
  const ArRegionalLairSeedDescriptor *d = ArRegionalLair_SeedDescriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->stock};
}
static ArRegionalRuleInfo HouseCreditRule(unsigned rule) {
  (void)rule;
  const ArRegionalHouseCreditDescriptor *d = ArRegionalHouseCredit_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->tiered};
}
static ArRegionalRuleInfo ScoreFeedbackRule(unsigned rule) {
  const ArRegionalScoreDescriptor *d = ArRegionalScore_Descriptor((ArRegionalScoreRule)rule);
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo LivesDisplayRule(unsigned rule) {
  (void)rule;
  const ArRegionalLivesDisplayDescriptor *d = ArRegionalLivesDisplay_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->zero_based};
}
static ArRegionalRuleInfo SourcesRule(unsigned rule) {
  const ArRegionalSourcesDescriptor *d = ArRegionalSources_Descriptor((ArRegionalSourceItem)rule);
  return (ArRegionalRuleInfo){d->key, d->automatic};
}
static ArRegionalRuleInfo SkullWaitRule(unsigned rule) {
  (void)rule;
  const ArRegionalSkullWaitDescriptor *d = ArRegionalSkullWait_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->frames};
}
static ArRegionalRuleInfo StoryRule(unsigned rule) {
  const ArRegionalStoryDescriptor *d = ArRegionalStory_Descriptor((ArRegionalStoryRule)rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
/* The lair reload policy has no descriptor of its own: one Japanese flag. */
static ArRegionalRuleInfo LairReloadsRule(unsigned rule) {
  (void)rule;
  static const uint16_t japanese[kArRegionalSource_Count] = {0, 1, 0};
  return (ArRegionalRuleInfo){"lair_reload_japanese", japanese};
}
static ArRegionalRuleInfo TownStatusRule(unsigned rule) {
  const ArRegionalTownStatusDescriptor *d =
      ArRegionalTownStatus_Descriptor((ArRegionalTownStatusRule)rule);
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo LevelGoalsRule(unsigned rule) {
  (void)rule;
  const ArRegionalLevelGoalsDescriptor *d = ArRegionalLevelGoals_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo SimCombatRule(unsigned rule) {
  const ArRegionalSimCombatDescriptor *d =
      ArRegionalSimCombat_Descriptor((ArRegionalSimCombatRule)rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo SimAiRule(unsigned rule) {
  const ArRegionalSimAiDescriptor *d = ArRegionalSimAi_Descriptor((ArRegionalSimAiRule)rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo ConstructionRule(unsigned rule) {
  (void)rule;
  const ArRegionalConstructionDescriptor *d = ArRegionalConstruction_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo SupportRule(unsigned rule) {
  const ArRegionalSupportDescriptor *d = ArRegionalSupport_Descriptor((ArRegionalSupportRule)rule);
  return (ArRegionalRuleInfo){d->key, d->amount};
}
static ArRegionalRuleInfo ArrivalRule(unsigned rule) {
  (void)rule;
  const ArRegionalArrivalDescriptor *d = ArRegionalArrival_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo ActionMotionRule(unsigned rule) {
  const ArRegionalActionMotionDescriptor *d =
      ArRegionalActionMotion_Descriptor((ArRegionalActionMotionRule)rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo EmittersRule(unsigned rule) {
  const ArRegionalEmitterDescriptor *d = ArRegionalEmitter_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo StatueVolleyRule(unsigned rule) {
  (void)rule;
  const ArRegionalVolleyDescriptor *d = ArRegionalVolley_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->shots};
}
static ArRegionalRuleInfo BossesRule(unsigned rule) {
  const ArRegionalBossDescriptor *d = ArRegionalBoss_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo CollisionRule(unsigned rule) {
  const ArRegionalCollisionDescriptor *d = ArRegionalCollision_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->japanese};
}
static ArRegionalRuleInfo PlatformSkullRule(unsigned rule) {
  const ArRegionalPlatformSkullDescriptor *d = ArRegionalPlatformSkull_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo ActorStatsRule(unsigned rule) {
  const ArRegionalActorStatDescriptor *d = ArRegionalActorStats_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo CastHoldRule(unsigned rule) {
  const ArRegionalCastHoldDescriptor *d = ArRegionalCastHold_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo FireEnemyRule(unsigned rule) {
  const ArRegionalFireDescriptor *d = ArRegionalFire_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo DifficultyRule(unsigned rule) {
  const ArRegionalDifficultyDescriptor *d = ArRegionalDifficulty_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo ScoreLivesRule(unsigned rule) {
  (void)rule;
  const ArRegionalScoreLivesDescriptor *d = ArRegionalScoreLives_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->enabled};
}
static ArRegionalRuleInfo ActionStartRule(unsigned rule) {
  const ArRegionalActionStartDescriptor *d = ArRegionalActionStart_Descriptor(rule);
  return (ArRegionalRuleInfo){d->key, d->value};
}
static ArRegionalRuleInfo SpellInventoryRule(unsigned rule) {
  (void)rule;
  const ArRegionalInventoryDescriptor *d = ArRegionalInventory_Descriptor();
  return (ArRegionalRuleInfo){d->key, d->enabled};
}

/* Family summaries: each forwards to that family's own _GroupSource. */
static bool TimersGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalTimers_GroupSource(&rules->timers, source);
}
static bool DevelopmentGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalDevelopment_GroupSource(&rules->development, source);
}
static bool RecoveryGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalRecovery_GroupSource(&rules->recovery, source);
}
static bool QuakeGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalQuake_GroupSource(&rules->quake, source);
}
static bool ScoreFeedbackGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalScore_GroupSource(&rules->score_feedback, source);
}
static bool SourcesGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalSources_GroupSource(&rules->sources, source);
}
static bool StoryGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalStory_GroupSource(&rules->story, source);
}
static bool TownStatusGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalTownStatus_GroupSource(&rules->town_status, source);
}
static bool SimCombatGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalSimCombat_GroupSource(&rules->sim_combat, source);
}
static bool SimAiGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalSimAi_GroupSource(&rules->sim_ai, source);
}
static bool SupportGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalSupport_GroupSource(&rules->support, source);
}
static bool ActionMotionGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalActionMotion_GroupSource(&rules->action_motion, source);
}
static bool EmittersGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalEmitter_GroupSource(&rules->emitters, source);
}
static bool BossesGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalBoss_GroupSource(&rules->bosses, source);
}
static bool CollisionGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalCollision_GroupSource(&rules->collision, source);
}
static bool PlatformSkullGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalPlatformSkull_GroupSource(&rules->platform_skull, source);
}
static bool ActorStatsGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalActorStats_GroupSource(&rules->actor_stats, source);
}
static bool CastHoldGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalCastHold_GroupSource(&rules->cast_hold, source);
}
static bool FireEnemyGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalFire_GroupSource(&rules->fire_enemy, source);
}
static bool DifficultyGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalDifficulty_GroupSource(&rules->difficulty, source);
}
static bool ActionStartGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalActionStart_GroupSource(&rules->action_start, source);
}
static bool ModeEntryGroup(const ArRegionalRules *rules, ArRegionalSource *source) {
  return ArRegionalMode_GroupSource(&rules->mode_entry, source);
}

_Static_assert(offsetof(ArRegionalPlacementPolicy, pickups) ==
                   offsetof(ArRegionalPlacementPolicy, enemies) + sizeof(ArRegionalSource),
               "placement sources are read as two consecutive slots");

static const ArRegionalFamily kFamilies[kArRegionalFamily_Count] = {
  [kArRegionalFamily_Hazards] = {"hazards", offsetof(ArRegionalRules, hazards), 1, 1,
                                 HazardsRule},
  [kArRegionalFamily_Terrain] = {"terrain", offsetof(ArRegionalRules, terrain), 1, 1,
                                 TerrainRule},
  [kArRegionalFamily_Music] = {"music", offsetof(ArRegionalRules, music), 1, 1, MusicRule},
  [kArRegionalFamily_Sequences] = {"sequences", offsetof(ArRegionalRules, sequences.source),
                                   kArRegionalSequence_Count, kArRegionalSequence_Count,
                                   SequencesRule},
  [kArRegionalFamily_Mosaic] = {"mosaic", offsetof(ArRegionalRules, mosaic), 1, 1, MosaicRule},
  [kArRegionalFamily_Poses] = {"poses", offsetof(ArRegionalRules, poses.source),
                               kArRegionalPose_Count, kArRegionalPose_Count, PosesRule},
  [kArRegionalFamily_Artwork] = {"artwork", offsetof(ArRegionalRules, artwork.source),
                                 kArRegionalArtwork_Count, kArRegionalArtwork_Count,
                                 ArtworkRule},
  [kArRegionalFamily_ActorArtwork] = {"actor_artwork",
                                      offsetof(ArRegionalRules, actor_artwork.source),
                                      kArRegionalActorArtwork_Count,
                                      kArRegionalActorArtwork_Count, ActorArtworkRule},
  [kArRegionalFamily_Placements] = {"placements", offsetof(ArRegionalRules, placements.enemies),
                                    kArRegionalPlacement_Count, kArRegionalPlacement_Count,
                                    PlacementsRule},
  [kArRegionalFamily_ModeEntry] = {"mode_entry", offsetof(ArRegionalRules, mode_entry.source),
                                   kArRegionalMode_Count, kArRegionalMode_Count,
                                   ModeEntryRule, ModeEntryGroup},
  [kArRegionalFamily_Costs] = {"costs", offsetof(ArRegionalRules, costs.source),
                               kArRegionalCostRule_Count, kArRegionalCostRule_Count, CostsRule},
  [kArRegionalFamily_Timers] = {"timers", offsetof(ArRegionalRules, timers.source),
                                kArRegionalTimerRule_Count, kArRegionalTimerRule_Count,
                                TimersRule, TimersGroup},
  [kArRegionalFamily_RetryScore] = {"retry_score", offsetof(ArRegionalRules, retry_score), 1, 1,
                                    RetryScoreRule},
  [kArRegionalFamily_TownWait] = {"town_wait", offsetof(ArRegionalRules, town_wait), 1, 1,
                                  TownWaitRule},
  [kArRegionalFamily_Fishing] = {"fishing", offsetof(ArRegionalRules, fishing), 1, 1,
                                 FishingRule},
  [kArRegionalFamily_Development] = {"development",
                                     offsetof(ArRegionalRules, development.source),
                                     kArRegionalDevelopmentRule_Count,
                                     kArRegionalDevelopmentRule_Count, DevelopmentRule,
                                     DevelopmentGroup},
  [kArRegionalFamily_Recovery] = {"recovery", offsetof(ArRegionalRules, recovery.source),
                                  kArRegionalRecovery_Count, kArRegionalRecovery_Count,
                                  RecoveryRule, RecoveryGroup},
  [kArRegionalFamily_Quake] = {"quake", offsetof(ArRegionalRules, quake.source),
                               kArRegionalQuake_Count, kArRegionalQuake_Count, QuakeRule,
                               QuakeGroup},
  [kArRegionalFamily_ScorePage] = {"score_page", offsetof(ArRegionalRules, score_page), 1, 1,
                                   ScorePageRule},
  [kArRegionalFamily_MenuReturn] = {"menu_return", offsetof(ArRegionalRules, menu_return), 1, 1,
                                    MenuReturnRule},
  [kArRegionalFamily_SpeedRange] = {"speed_range", offsetof(ArRegionalRules, speed_range), 1, 1,
                                    SpeedRangeRule},
  [kArRegionalFamily_MagicGesture] = {"magic_gesture", offsetof(ArRegionalRules, magic_gesture),
                                      1, 1, MagicGestureRule},
  [kArRegionalFamily_LairSeeds] = {"lair_seeds", offsetof(ArRegionalRules, lair_seeds), 1,
                                   kArRegionalLairCount, LairSeedsRule},
  [kArRegionalFamily_HouseCredit] = {"house_credit", offsetof(ArRegionalRules, house_credit), 1,
                                     1, HouseCreditRule},
  [kArRegionalFamily_ScoreFeedback] = {"score_feedback",
                                       offsetof(ArRegionalRules, score_feedback.source),
                                       kArRegionalScore_Count, kArRegionalScore_Count,
                                       ScoreFeedbackRule, ScoreFeedbackGroup},
  [kArRegionalFamily_LivesDisplay] = {"lives_display", offsetof(ArRegionalRules, lives_display),
                                      1, 1, LivesDisplayRule},
  [kArRegionalFamily_Sources] = {"sources", offsetof(ArRegionalRules, sources.source),
                                 kArRegionalSourceItem_Count, kArRegionalSourceItem_Count,
                                 SourcesRule, SourcesGroup},
  [kArRegionalFamily_SkullWait] = {"skull_wait", offsetof(ArRegionalRules, skull_wait), 1, 1,
                                   SkullWaitRule},
  [kArRegionalFamily_Story] = {"story", offsetof(ArRegionalRules, story.source),
                               kArRegionalStory_Count, kArRegionalStory_Count, StoryRule,
                               StoryGroup},
  [kArRegionalFamily_LairReloads] = {"lair_reloads", offsetof(ArRegionalRules, lair_reloads), 1,
                                     1, LairReloadsRule},
  [kArRegionalFamily_TownStatus] = {"town_status", offsetof(ArRegionalRules, town_status.source),
                                    kArRegionalTownStatus_Count, kArRegionalTownStatus_Count,
                                    TownStatusRule, TownStatusGroup},
  [kArRegionalFamily_LevelGoals] = {"level_goals", offsetof(ArRegionalRules, level_goals), 1, 1,
                                    LevelGoalsRule},
  [kArRegionalFamily_SimCombat] = {"sim_combat", offsetof(ArRegionalRules, sim_combat.source),
                                   kArRegionalSimCombat_Count, kArRegionalSimCombat_Count,
                                   SimCombatRule, SimCombatGroup},
  [kArRegionalFamily_SimAi] = {"sim_ai", offsetof(ArRegionalRules, sim_ai.source),
                               kArRegionalSimAi_Count, kArRegionalSimAi_Count, SimAiRule,
                               SimAiGroup},
  [kArRegionalFamily_Construction] = {"construction", offsetof(ArRegionalRules, construction), 1,
                                      1, ConstructionRule},
  [kArRegionalFamily_Support] = {"support", offsetof(ArRegionalRules, support.source),
                                 kArRegionalSupport_Count, kArRegionalSupport_Count,
                                 SupportRule, SupportGroup},
  [kArRegionalFamily_Arrival] = {"arrival", offsetof(ArRegionalRules, arrival), 1, 1,
                                 ArrivalRule},
  [kArRegionalFamily_ActionMotion] = {"action_motion",
                                      offsetof(ArRegionalRules, action_motion.source),
                                      kArRegionalActionMotion_Count,
                                      kArRegionalActionMotion_Count, ActionMotionRule,
                                      ActionMotionGroup},
  [kArRegionalFamily_Emitters] = {"emitters", offsetof(ArRegionalRules, emitters.source),
                                  kArRegionalEmitter_Count, kArRegionalEmitter_Count,
                                  EmittersRule, EmittersGroup},
  [kArRegionalFamily_StatueVolley] = {"statue_volley", offsetof(ArRegionalRules, statue_volley),
                                      1, 1, StatueVolleyRule},
  [kArRegionalFamily_Bosses] = {"bosses", offsetof(ArRegionalRules, bosses.source),
                                kArRegionalBoss_Count, kArRegionalBoss_Count, BossesRule,
                                BossesGroup},
  [kArRegionalFamily_Collision] = {"collision", offsetof(ArRegionalRules, collision.source),
                                   kArRegionalCollision_Count, kArRegionalCollision_Count,
                                   CollisionRule, CollisionGroup},
  [kArRegionalFamily_PlatformSkull] = {"platform_skull",
                                       offsetof(ArRegionalRules, platform_skull.source),
                                       kArRegionalPlatformSkull_Count,
                                       kArRegionalPlatformSkull_Count, PlatformSkullRule,
                                       PlatformSkullGroup},
  [kArRegionalFamily_ActorStats] = {"actor_stats", offsetof(ArRegionalRules, actor_stats.source),
                                    kArRegionalActorStat_Count, kArRegionalActorStat_Count,
                                    ActorStatsRule, ActorStatsGroup},
  [kArRegionalFamily_CastHold] = {"cast_hold", offsetof(ArRegionalRules, cast_hold.source),
                                  kArRegionalCastHold_Count, kArRegionalCastHold_Count,
                                  CastHoldRule, CastHoldGroup},
  [kArRegionalFamily_FireEnemy] = {"fire_enemy", offsetof(ArRegionalRules, fire_enemy.source),
                                   kArRegionalFire_Count, kArRegionalFire_Count, FireEnemyRule,
                                   FireEnemyGroup},
  [kArRegionalFamily_Difficulty] = {"difficulty", offsetof(ArRegionalRules, difficulty.source),
                                    kArRegionalDifficultyRule_Count,
                                    kArRegionalDifficultyRule_Count, DifficultyRule,
                                    DifficultyGroup},
  [kArRegionalFamily_ScoreLives] = {"score_lives", offsetof(ArRegionalRules, score_lives), 1, 1,
                                    ScoreLivesRule},
  [kArRegionalFamily_ActionStart] = {"action_start",
                                     offsetof(ArRegionalRules, action_start.source),
                                     kArRegionalActionStart_Count, kArRegionalActionStart_Count,
                                     ActionStartRule, ActionStartGroup},
  [kArRegionalFamily_SpellInventory] = {"spell_inventory",
                                        offsetof(ArRegionalRules, spell_inventory), 1, 1,
                                        SpellInventoryRule},
};

const ArRegionalFamily *ArRegionalFamilies_Get(ArRegionalFamilyId family) {
  return (unsigned)family < kArRegionalFamily_Count ? &kFamilies[family] : NULL;
}

static unsigned SlotOf(const ArRegionalFamily *family, unsigned rule) {
  return family->slots == 1 ? 0 : rule;
}

ArRegionalSource *ArRegionalFamilies_Field(ArRegionalRules *rules, ArRegionalFamilyId family,
                                           unsigned rule) {
  const ArRegionalFamily *row = &kFamilies[family];
  return (ArRegionalSource *)((unsigned char *)rules + row->offset) + SlotOf(row, rule);
}

ArRegionalSource ArRegionalFamilies_Source(const ArRegionalRules *rules,
                                           ArRegionalFamilyId family, unsigned rule) {
  const ArRegionalFamily *row = &kFamilies[family];
  const ArRegionalSource *first =
      (const ArRegionalSource *)((const unsigned char *)rules + row->offset);
  return first[SlotOf(row, rule)];
}

bool ArRegionalFamilies_SourcesValid(const ArRegionalRules *rules) {
  if (!rules) return false;
  for (unsigned family = 0; family < kArRegionalFamily_Count; ++family) {
    const ArRegionalFamily *row = &kFamilies[family];
    const ArRegionalSource *first =
        (const ArRegionalSource *)((const unsigned char *)rules + row->offset);
    for (unsigned slot = 0; slot < row->slots; ++slot)
      if ((unsigned)first[slot] >= kArRegionalSource_Count) return false;
  }
  return true;
}
