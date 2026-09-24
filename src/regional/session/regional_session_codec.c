#include "regional/session/regional_session_internal.h"
#include "byte_order.h"

#include <stdio.h>
#include <string.h>

/* Stable v1-v70 persistence, separate from live activation. See
 * docs/save-format.md for the wire contract before extending. */
enum {
  kHeaderBytes = 36,
  kPayloadCapacity = kSaveCheckpointPayloadMax,
  kV1RecordCount = kArRegionalCostRule_Count + kArRegionalTimerRule_Count,
  kV2RecordCount = kV1RecordCount + 1,
  kV3RecordCount = kV2RecordCount + 1,
  kV4RecordCount = kV3RecordCount + 1,
  kV5RecordCount = kV4RecordCount + kArRegionalDevelopmentRule_Count,
  kV6RecordCount = kV5RecordCount + kArRegionalRecovery_Count,
  kV7RecordCount = kV6RecordCount + kArRegionalQuake_Count,
  kV8RecordCount = kV7RecordCount + 1,
  kV9RecordCount = kV8RecordCount + 1,
  kV10RecordCount = kV9RecordCount + 1,
  kV12RecordCount = kV10RecordCount + 1,
  kV13RecordCount = kV12RecordCount + kArRegionalLairCount,
  kV14RecordCount = kV13RecordCount + 1,
  kV15RecordCount = kV14RecordCount + 3,
  kV16RecordCount = kV14RecordCount + kArRegionalScore_Count,
  kV17RecordCount = kV16RecordCount + 1,
  kV18RecordCount = kV17RecordCount + kArRegionalSourceItem_Count,
  kV19RecordCount = kV18RecordCount + 1,
  kV20RecordCount = kV19RecordCount + kArRegionalStory_Count,
  kV21RecordCount = kV20RecordCount + 1,
  kV22RecordCount = kV21RecordCount + kArRegionalTownStatus_Count,
  kV23RecordCount = kV22RecordCount + 1,
  kV24RecordCount = kV23RecordCount + kArRegionalSimCombat_Count,
  kV25RecordCount = kV24RecordCount + kArRegionalSimAi_Count,
  kV26RecordCount = kV25RecordCount + 1,
  kV27RecordCount = kV26RecordCount + kArRegionalSupport_Count,
  kV28RecordCount = kV27RecordCount + 1,
  kV29RecordCount = kV28RecordCount + 7,
  kV30RecordCount = kV28RecordCount + 9,
  kV31RecordCount = kV28RecordCount + 10,
  kV32RecordCount = kV28RecordCount + 12,
  kV33RecordCount = kV32RecordCount + 2,
  kV34RecordCount = kV33RecordCount + 1,
  kV35RecordCount = kV34RecordCount + 6,
  kV36RecordCount = kV34RecordCount + 7,
  kV37RecordCount = kV36RecordCount + 2,
  kV38RecordCount = kV37RecordCount + 4,
  kV39RecordCount = kV38RecordCount + 63,
  kV40RecordCount = kV38RecordCount + 66,
  kV41RecordCount = kV40RecordCount + 4,
  kV42RecordCount = kV41RecordCount + 3,
  kV43RecordCount = kV42RecordCount + 2,
  kV44RecordCount = kV43RecordCount + kArRegionalFire_Count,
  kV45RecordCount = kV44RecordCount + 2,
  kV46RecordCount = kV45RecordCount + 4,
  kV47RecordCount = kV46RecordCount + 1,
  kV48RecordCount = kV47RecordCount + 3,
  kV49RecordCount = kV48RecordCount + 4,
  kV50RecordCount = kV49RecordCount + 1,
  kV51RecordCount = kV50RecordCount + kArRegionalDifficultyRule_Count,
  kV52RecordCount = kV51RecordCount + 1,
  kV53RecordCount = kV52RecordCount + kArRegionalActionStart_Count,
  kV54RecordCount = kV53RecordCount + 1,
  kV55RecordCount = kV54RecordCount + kArRegionalMode_Count,
  kV56RecordCount = kV55RecordCount + 4,
  kV57RecordCount = kV56RecordCount + 1,
  kV58RecordCount = kV57RecordCount + 1,
  kV59RecordCount = kV58RecordCount + 1,
  kV60RecordCount = kV59RecordCount + 1,
  kV61RecordCount = kV60RecordCount + 2,
  kV62RecordCount = kV61RecordCount + 1,
  kV66RecordCount = kV62RecordCount + 6,
  kV67RecordCount = kV66RecordCount + 2,
  kV68RecordCount = kV67RecordCount + 2,
  kRecordCount = kV68RecordCount + kArRegionalActorArtwork_Count
};
_Static_assert(kArRegionalSequence_Count == 2, "preserve v68 sequence ordinals");
_Static_assert(kArRegionalPose_Count == 2, "preserve v67 pose ordinals");
_Static_assert(kArRegionalPlacement_Count == 2, "preserve placement record ordinals");
_Static_assert(kArRegionalArtwork_Count == 6,
               "freeze prior artwork ordinals before extending the codec");
_Static_assert(kArRegionalMode_Count == 2, "preserve mode-entry record ordinals");
_Static_assert(kArRegionalActionStart_Count == 2, "preserve action-start record ordinals");
_Static_assert(kArRegionalDifficultyRule_Count == 5, "preserve difficulty record ordinals");
_Static_assert(kArRegionalFire_Count == 4, "preserve fire-enemy record ordinals");
_Static_assert(kArRegionalCastHold_Count == 3, "preserve cast-hold record ordinals");
_Static_assert(kArRegionalActorStat_BaseCount == 63 && kArRegionalActorStat_Count == 66,
               "preserve historical stat record ordinals");
_Static_assert(kArRegionalPlatformSkull_Count == 4, "preserve historical skull record ordinals");
_Static_assert(kArRegionalCollision_Count == 2, "preserve historical collision record ordinals");
_Static_assert(kArRegionalBoss_Count == 31, "preserve historical boss record ordinals");
_Static_assert(kArRegionalEmitter_Count == 2, "preserve historical emitter record ordinals");
_Static_assert(kArRegionalActionMotion_Count == 14, "preserve historical motion record ordinals");
_Static_assert(kArRegionalCostRule_Count == 9 && kArRegionalTimerRule_Count == 6,
               "extend the legacy record mapping explicitly when adding family leaves");
_Static_assert(kArRegionalDevelopmentRule_Count == 3, "version5 has three development leaves");
_Static_assert(kArRegionalRecovery_Count == 2, "version6 has two recovery leaves");
_Static_assert(kArRegionalQuake_Count == 5, "version7 has five quake selectors");
_Static_assert(kArRegionalScore_Phase == 3 && kArRegionalScore_Count == 4,
               "version16 appends score phase after version15's three arithmetic leaves");
static const uint8_t kMagic[8] = {'A', 'R', 'R', 'E', 'G', 'I', 'O', 'N'};
_Static_assert(kArRegionalSourceItem_Count == 2, "version18 adds two Source collection policies");
_Static_assert(kArRegionalStory_Count == 3, "version20 adds three story-prerequisite policies");
static const uint8_t kPriceMagic[8] = {'A', 'R', 'P', 'R', 'I', 'C', 'E', 0};
static const char *const kSourceKeys[] = {"us", "jp", "eu"};
_Static_assert(sizeof(kSourceKeys) / sizeof(kSourceKeys[0]) == kArRegionalSource_Count,
               "each persisted source needs a stable key");

/* One typed record inventory owns the key, regional values and source field.
 * Encode and decode use this same binding: no layout offsets or duplicate
 * ordinal-to-field switches. Historical append order remains part of v69. */
typedef struct RuleRecord {
  const char *key;
  const uint16_t *values;
  ArRegionalSource *source;
} RuleRecord;

static RuleRecord Record(ArRegionalRules *rules, unsigned i) {
  if (i >= kV68RecordCount) {
    const ArRegionalArtworkDescriptor *desc =
        ArRegionalActorArtwork_Descriptor(i - kV68RecordCount);
    return (RuleRecord){desc->key, desc->enabled,
                        &rules->actor_artwork.source[i - kV68RecordCount]};
  }
  if (i >= kV67RecordCount) {
    const ArRegionalMusicDescriptor *desc = ArRegionalSequences_Descriptor(i - kV67RecordCount);
    return (RuleRecord){desc->key, desc->profile, &rules->sequences.source[i - kV67RecordCount]};
  }
  if (i >= kV66RecordCount) {
    const ArRegionalPoseDescriptor *desc = ArRegionalPoses_Descriptor(i - kV66RecordCount);
    return (RuleRecord){desc->key, desc->enabled, &rules->poses.source[i - kV66RecordCount]};
  }
  if (i >= kV62RecordCount) {
    const ArRegionalArtworkDescriptor *desc = ArRegionalArtwork_Descriptor(i - kV62RecordCount);
    return (RuleRecord){desc->key, desc->enabled, &rules->artwork.source[i - kV62RecordCount]};
  }
  if (i == kV61RecordCount) {
    const ArRegionalMosaicDescriptor *desc = ArRegionalMosaic_Descriptor();
    return (RuleRecord){desc->key, desc->profile, &rules->mosaic};
  }
  if (i >= kV60RecordCount) {
    const ArRegionalPlacementDescriptor *desc =
        ArRegionalPlacements_Descriptor(i - kV60RecordCount);
    return (RuleRecord){
        desc->key, desc->profile,
        i == kV60RecordCount ? &rules->placements.enemies : &rules->placements.pickups};
  }
  if (i == kV59RecordCount) {
    const ArRegionalMusicDescriptor *desc = ArRegionalMusic_Descriptor();
    return (RuleRecord){desc->key, desc->profile, &rules->music};
  }
  if (i == kV58RecordCount) {
    const ArRegionalTerrainDescriptor *desc = ArRegionalTerrain_Descriptor();
    return (RuleRecord){desc->key, desc->profile, &rules->terrain};
  }
  if (i == kV57RecordCount) {
    const ArRegionalHazardDescriptor *desc = ArRegionalHazards_Descriptor();
    return (RuleRecord){desc->key, desc->profile, &rules->hazards};
  }
  if (i == kV56RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(kArRegionalBoss_PlantGeometry);
    return (RuleRecord){desc->key, desc->value,
                        &rules->bosses.source[kArRegionalBoss_PlantGeometry]};
  }
  if (i >= kV55RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(26 + i - kV55RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[26 + i - kV55RecordCount]};
  }
  if (i >= kV54RecordCount) {
    const ArRegionalModeDescriptor *desc = ArRegionalMode_Descriptor(i - kV54RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->mode_entry.source[i - kV54RecordCount]};
  }
  if (i == kV53RecordCount) {
    const ArRegionalInventoryDescriptor *desc = ArRegionalInventory_Descriptor();
    return (RuleRecord){desc->key, desc->enabled, &rules->spell_inventory};
  }
  if (i >= kV52RecordCount) {
    const ArRegionalActionStartDescriptor *desc =
        ArRegionalActionStart_Descriptor(i - kV52RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->action_start.source[i - kV52RecordCount]};
  }
  if (i == kV51RecordCount) {
    const ArRegionalScoreLivesDescriptor *desc = ArRegionalScoreLives_Descriptor();
    return (RuleRecord){desc->key, desc->enabled, &rules->score_lives};
  }
  if (i >= kV50RecordCount) {
    const ArRegionalDifficultyDescriptor *desc =
        ArRegionalDifficulty_Descriptor(i - kV50RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->difficulty.source[i - kV50RecordCount]};
  }
  if (i == kV49RecordCount) {
    const ArRegionalActionMotionDescriptor *desc =
        ArRegionalActionMotion_Descriptor(kArRegionalActionMotion_TreeSeeds);
    return (RuleRecord){desc->key, desc->value,
                        &rules->action_motion.source[kArRegionalActionMotion_TreeSeeds]};
  }
  if (i >= kV48RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(22 + i - kV48RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[22 + i - kV48RecordCount]};
  }
  if (i >= kV47RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(19 + i - kV47RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[19 + i - kV47RecordCount]};
  }
  if (i == kV46RecordCount) {
    const ArRegionalActionMotionDescriptor *desc =
        ArRegionalActionMotion_Descriptor(kArRegionalActionMotion_HeadWithdrawal);
    return (RuleRecord){desc->key, desc->value,
                        &rules->action_motion.source[kArRegionalActionMotion_HeadWithdrawal]};
  }
  if (i >= kV45RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(15 + i - kV45RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[15 + i - kV45RecordCount]};
  }
  if (i >= kV44RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(13 + i - kV44RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[13 + i - kV44RecordCount]};
  }
  if (i >= kV43RecordCount) {
    const ArRegionalFireDescriptor *desc = ArRegionalFire_Descriptor(i - kV43RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->fire_enemy.source[i - kV43RecordCount]};
  }
  if (i >= kV42RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(11 + i - kV42RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[11 + i - kV42RecordCount]};
  }
  if (i >= kV41RecordCount) {
    const ArRegionalCastHoldDescriptor *desc = ArRegionalCastHold_Descriptor(i - kV41RecordCount);
    return (RuleRecord){desc->key, desc->enabled, &rules->cast_hold.source[i - kV41RecordCount]};
  }
  if (i >= kV40RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(7 + i - kV40RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[7 + i - kV40RecordCount]};
  }
  if (i >= kV38RecordCount) {
    const ArRegionalActorStatDescriptor *desc =
        ArRegionalActorStats_Descriptor(i - kV38RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->actor_stats.source[i - kV38RecordCount]};
  }
  if (i >= kV37RecordCount) {
    const ArRegionalPlatformSkullDescriptor *desc =
        ArRegionalPlatformSkull_Descriptor(i - kV37RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->platform_skull.source[i - kV37RecordCount]};
  }
  if (i >= kV36RecordCount) {
    const ArRegionalCollisionDescriptor *desc = ArRegionalCollision_Descriptor(i - kV36RecordCount);
    return (RuleRecord){desc->key, desc->japanese, &rules->collision.source[i - kV36RecordCount]};
  }
  if (i >= kV34RecordCount) {
    const ArRegionalBossDescriptor *desc = ArRegionalBoss_Descriptor(i - kV34RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->bosses.source[i - kV34RecordCount]};
  }
  if (i == kV33RecordCount) {
    const ArRegionalVolleyDescriptor *desc = ArRegionalVolley_Descriptor();
    return (RuleRecord){desc->key, desc->shots, &rules->statue_volley};
  }
  if (i >= kV32RecordCount) {
    const ArRegionalEmitterDescriptor *desc = ArRegionalEmitter_Descriptor(i - kV32RecordCount);
    return (RuleRecord){desc->key, desc->value, &rules->emitters.source[i - kV32RecordCount]};
  }
  if (i >= kV28RecordCount) {
    const ArRegionalActionMotionDescriptor *desc =
        ArRegionalActionMotion_Descriptor((ArRegionalActionMotionRule)(i - kV28RecordCount));
    return (RuleRecord){desc->key, desc->value, &rules->action_motion.source[i - kV28RecordCount]};
  }
  if (i == kV27RecordCount) {
    const ArRegionalArrivalDescriptor *desc = ArRegionalArrival_Descriptor();
    return (RuleRecord){desc->key, desc->japanese, &rules->arrival};
  }
  if (i >= kV26RecordCount) {
    const ArRegionalSupportDescriptor *desc =
        ArRegionalSupport_Descriptor((ArRegionalSupportRule)(i - kV26RecordCount));
    return (RuleRecord){desc->key, desc->amount, &rules->support.source[i - kV26RecordCount]};
  }
  if (i == kV25RecordCount) {
    const ArRegionalConstructionDescriptor *desc = ArRegionalConstruction_Descriptor();
    return (RuleRecord){desc->key, desc->japanese, &rules->construction};
  }
  if (i >= kV24RecordCount) {
    const ArRegionalSimAiDescriptor *desc =
        ArRegionalSimAi_Descriptor((ArRegionalSimAiRule)(i - kV24RecordCount));
    return (RuleRecord){desc->key, desc->value, &rules->sim_ai.source[i - kV24RecordCount]};
  }
  if (i >= kV23RecordCount) {
    const ArRegionalSimCombatDescriptor *desc =
        ArRegionalSimCombat_Descriptor((ArRegionalSimCombatRule)(i - kV23RecordCount));
    return (RuleRecord){desc->key, desc->value, &rules->sim_combat.source[i - kV23RecordCount]};
  }
  if (i == kV22RecordCount) {
    const ArRegionalLevelGoalsDescriptor *desc = ArRegionalLevelGoals_Descriptor();
    return (RuleRecord){desc->key, desc->japanese, &rules->level_goals};
  }
  if (i >= kV21RecordCount) {
    const ArRegionalTownStatusDescriptor *desc =
        ArRegionalTownStatus_Descriptor((ArRegionalTownStatusRule)(i - kV21RecordCount));
    return (RuleRecord){desc->key, desc->japanese, &rules->town_status.source[i - kV21RecordCount]};
  }
  if (i >= kV20RecordCount) {
    static const uint16_t japanese[kArRegionalSource_Count] = {0, 1, 0};
    return (RuleRecord){"lair_reload_japanese", japanese, &rules->lair_reloads};
  }
  if (i >= kV19RecordCount) {
    const ArRegionalStoryDescriptor *desc =
        ArRegionalStory_Descriptor((ArRegionalStoryRule)(i - kV19RecordCount));
    return (RuleRecord){desc->key, desc->value, &rules->story.source[i - kV19RecordCount]};
  }
  if (i == kV18RecordCount) {
    const ArRegionalSkullWaitDescriptor *desc = ArRegionalSkullWait_Descriptor();
    return (RuleRecord){desc->key, desc->frames, &rules->skull_wait};
  }
  if (i >= kV17RecordCount) {
    const ArRegionalSourcesDescriptor *desc =
        ArRegionalSources_Descriptor((ArRegionalSourceItem)(i - kV17RecordCount));
    return (RuleRecord){desc->key, desc->automatic, &rules->sources.source[i - kV17RecordCount]};
  }
  if (i == kV16RecordCount) {
    const ArRegionalLivesDisplayDescriptor *desc = ArRegionalLivesDisplay_Descriptor();
    return (RuleRecord){desc->key, desc->zero_based, &rules->lives_display};
  }
  if (i >= kV14RecordCount) {
    const ArRegionalScoreDescriptor *desc =
        ArRegionalScore_Descriptor((ArRegionalScoreRule)(i - kV14RecordCount));
    return (RuleRecord){desc->key, desc->japanese,
                        &rules->score_feedback.source[i - kV14RecordCount]};
  }
  if (i == kV13RecordCount) {
    const ArRegionalHouseCreditDescriptor *desc = ArRegionalHouseCredit_Descriptor();
    return (RuleRecord){desc->key, desc->tiered, &rules->house_credit};
  }
  if (i >= kV12RecordCount) {
    const ArRegionalLairSeedDescriptor *desc = ArRegionalLair_SeedDescriptor(i - kV12RecordCount);
    return (RuleRecord){desc->key, desc->stock, &rules->lair_seeds};
  }
  if (i == kV10RecordCount) {
    const ArRegionalMagicGestureDescriptor *desc = ArRegionalMagicGesture_Descriptor();
    return (RuleRecord){desc->key, desc->up_attack, &rules->magic_gesture};
  }
  if (i == kV9RecordCount) {
    const ArRegionalSpeedRangeDescriptor *desc = ArRegionalSpeedRange_Descriptor();
    return (RuleRecord){desc->key, desc->maximum, &rules->speed_range};
  }
  if (i == kV8RecordCount) {
    const ArRegionalMenuReturnDescriptor *desc = ArRegionalMenuReturn_Descriptor();
    return (RuleRecord){desc->key, desc->keep_open, &rules->menu_return};
  }
  if (i == kV7RecordCount) {
    const ArRegionalScorePageDescriptor *desc = ArRegionalScorePage_Descriptor();
    return (RuleRecord){desc->key, desc->enabled, &rules->score_page};
  }
  if (i >= kV6RecordCount) {
    const ArRegionalQuakeDescriptor *desc =
        ArRegionalQuake_Descriptor((ArRegionalQuakeRule)(i - kV6RecordCount));
    return (RuleRecord){desc->key, desc->random, &rules->quake.source[i - kV6RecordCount]};
  }
  if (i >= kV5RecordCount) {
    const ArRegionalRecoveryDescriptor *desc =
        ArRegionalRecovery_Descriptor((ArRegionalRecoveryRule)(i - kV5RecordCount));
    return (RuleRecord){desc->key, desc->value, &rules->recovery.source[i - kV5RecordCount]};
  }
  if (i >= kV4RecordCount) {
    const ArRegionalDevelopmentDescriptor *desc =
        ArRegionalDevelopment_Descriptor((ArRegionalDevelopmentRule)(i - kV4RecordCount));
    return (RuleRecord){desc->key, desc->updates, &rules->development.source[i - kV4RecordCount]};
  }
  if (i == kV3RecordCount) {
    const ArRegionalFishingDescriptor *desc = ArRegionalFishing_Descriptor();
    return (RuleRecord){desc->key, desc->updates, &rules->fishing};
  }
  if (i == kV2RecordCount) {
    const ArRegionalTownWaitDescriptor *desc = ArRegionalTownWait_Descriptor();
    return (RuleRecord){desc->key, desc->updates, &rules->town_wait};
  }
  if (i == kV1RecordCount) {
    const ArRegionalRetryDescriptor *desc = ArRegionalRetry_Descriptor();
    return (RuleRecord){desc->key, desc->clear_score, &rules->retry_score};
  }
  if (i < kArRegionalCostRule_Count) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    return (RuleRecord){desc->key, desc->price, &rules->costs.source[i]};
  }
  const ArRegionalTimerDescriptor *desc =
      ArRegionalTimers_Descriptor((ArRegionalTimerRule)(i - kArRegionalCostRule_Count));
  return (RuleRecord){desc->key, desc->bcd, &rules->timers.source[i - kArRegionalCostRule_Count]};
}

/* Compact explicit codec, never fwrite a C struct or persist enum ordinals.
 * Each named leaf carries requested/effective source keys AND resolved values.
 * A table/schema change that cannot reproduce those values fails visibly. */
static bool Encode(const ArRegionalSession *session, uint8_t *out, size_t *size) {
  if (!ArRegionalSession_Valid(session)) return false;
  memset(out, 0, kHeaderBytes);
  memcpy(out, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(out + 8, session->randomizer.generator ? 70 : 69);
  ByteOrder_WriteLe16(out + 10, kRecordCount);
  ByteOrder_WriteLe32(out + 12, session->slot);
  memcpy(out + 16, session->campaign, 16);
  ByteOrder_WriteLe32(out + 32, session->revision);
  /* Read-only encoding uses local rule copies so the shared typed binding
   * needs no const cast and can also provide writable fields to the decoder. */
  ArRegionalRules requested_rules = session->requested;
  ArRegionalRules effective_rules = session->effective;
  size_t offset = kHeaderBytes;
  for (unsigned i = 0; i < kRecordCount; ++i) {
    const RuleRecord record = Record(&requested_rules, i);
    const char *key = record.key;
    const uint16_t *values = record.values;
    const ArRegionalSource requested = *record.source;
    const ArRegionalSource effective = *Record(&effective_rules, i).source;
    size_t length = strlen(key);
    if (!length || length > UINT8_MAX || length + 9 > kPayloadCapacity - offset) return false;
    out[offset++] = (uint8_t)length;
    memcpy(out + offset, key, length);
    offset += length;
    memcpy(out + offset, kSourceKeys[requested], 2);
    memcpy(out + offset + 2, kSourceKeys[effective], 2);
    ByteOrder_WriteLe16(out + offset + 4, values[requested]);
    ByteOrder_WriteLe16(out + offset + 6, values[effective]);
    offset += 8;
  }
  if (!ArRegionalLairHistory_Encode(&session->lairs, out + offset, kPayloadCapacity - offset))
    return false;
  offset += kArRegionalLairHistoryEncodedBytes;
  if (!ArRegionalLairReloads_Encode(&session->reloads, out + offset, kPayloadCapacity - offset))
    return false;
  offset += kArRegionalLairReloadEncodedBytes;
  if (!ArRegionalSimActors_Encode(&session->sim_actors, out + offset, kPayloadCapacity - offset))
    return false;
  offset += kArRegionalSimActorsEncodedBytes;
  if (kPayloadCapacity - offset < 9) return false;
  memcpy(out + offset, "ARARRIV1", 8);
  out[offset + 8] = session->arrival_locked;
  offset += 9;
  if (kPayloadCapacity - offset < 10) return false;
  memcpy(out + offset, "ARDIFF01", 8);
  /* Stable choice bytes; neither foreign RAM values nor region keys. */
  out[offset + 8] = (uint8_t)session->requested.difficulty.level;
  out[offset + 9] = (uint8_t)session->effective.difficulty.level;
  offset += 10;
  if(session->randomizer.generator) {
    if(kPayloadCapacity-offset<kRandomizerConfigBytes ||
        !RandomizerConfig_Encode(&session->randomizer,out+offset))return false;
    offset+=kRandomizerConfigBytes;
  }
  *size = offset;
  return true;
}

static ArRegionalSource DecodeSource(const uint8_t *key) {
  for (unsigned i = 0; i < kArRegionalSource_Count; ++i)
    if (!memcmp(key, kSourceKeys[i], 2)) return (ArRegionalSource)i;
  return kArRegionalSource_Count;
}

static SaveCheckpointStatus Decode(const uint8_t *bytes, size_t size, ArRegionalSession *session) {
  if (size < kHeaderBytes) return kSaveCheckpoint_Invalid;
  const bool pricing_only = !memcmp(bytes, kPriceMagic, sizeof(kPriceMagic));
  if (!pricing_only && memcmp(bytes, kMagic, sizeof(kMagic))) return kSaveCheckpoint_Invalid;
  const unsigned version = ByteOrder_ReadLe16(bytes + 8);
  if (version < 1 || version > (pricing_only ? 1u : 70u)) return kSaveCheckpoint_Unsupported;
  const unsigned count = pricing_only    ? kArRegionalCostRule_Count
                         : version == 1  ? kV1RecordCount
                         : version == 2  ? kV2RecordCount
                         : version == 3  ? kV3RecordCount
                         : version == 4  ? kV4RecordCount
                         : version == 5  ? kV5RecordCount
                         : version == 6  ? kV6RecordCount
                         : version == 7  ? kV7RecordCount
                         : version == 8  ? kV8RecordCount
                         : version == 9  ? kV9RecordCount
                         : version == 10 ? kV10RecordCount
                         : version <= 12 ? kV12RecordCount
                         : version == 13 ? kV13RecordCount
                         : version == 14 ? kV14RecordCount
                         : version == 15 ? kV15RecordCount
                         : version == 16 ? kV16RecordCount
                         : version == 17 ? kV17RecordCount
                         : version == 18 ? kV18RecordCount
                         : version == 19 ? kV19RecordCount
                         : version == 20 ? kV20RecordCount
                         : version == 21 ? kV21RecordCount
                         : version == 22 ? kV22RecordCount
                         : version == 23 ? kV23RecordCount
                         : version == 24 ? kV24RecordCount
                         : version == 25 ? kV25RecordCount
                         : version == 26 ? kV26RecordCount
                         : version == 27 ? kV27RecordCount
                         : version == 28 ? kV28RecordCount
                         : version == 29 ? kV29RecordCount
                         : version == 30 ? kV30RecordCount
                         : version == 31 ? kV31RecordCount
                         : version == 32 ? kV32RecordCount
                         : version == 33 ? kV33RecordCount
                         : version == 34 ? kV34RecordCount
                         : version == 35 ? kV35RecordCount
                         : version == 36 ? kV36RecordCount
                         : version == 37 ? kV37RecordCount
                         : version == 38 ? kV38RecordCount
                         : version == 39 ? kV39RecordCount
                         : version == 40 ? kV40RecordCount
                         : version == 41 ? kV41RecordCount
                         : version == 42 ? kV42RecordCount
                         : version == 43 ? kV43RecordCount
                         : version == 44 ? kV44RecordCount
                         : version == 45 ? kV45RecordCount
                         : version == 46 ? kV46RecordCount
                         : version == 47 ? kV47RecordCount
                         : version == 48 ? kV48RecordCount
                         : version == 49 ? kV49RecordCount
                         : version == 50 ? kV50RecordCount
                         : version == 51 ? kV51RecordCount
                         : version == 52 ? kV52RecordCount
                         : version == 53 ? kV53RecordCount
                         : version == 54 ? kV54RecordCount
                         : version == 55 ? kV55RecordCount
                         : version == 56 ? kV56RecordCount
                         : version == 57 ? kV57RecordCount
                         : version == 58 ? kV58RecordCount
                         : version == 59 ? kV59RecordCount
                         : version == 60 ? kV60RecordCount
                         : version == 61 ? kV61RecordCount
                         : version == 62 ? kV62RecordCount
                         : version == 63 ? kV62RecordCount + 1
                         : version == 64 ? kV62RecordCount + 2
                         : version == 65 ? kV62RecordCount + 5
                         : version == 66 ? kV66RecordCount
                         : version == 67 ? kV67RecordCount
                         : version == 68 ? kV68RecordCount
                                         : kRecordCount;
  if (ByteOrder_ReadLe16(bytes + 10) != count) return kSaveCheckpoint_Unsupported;
  ArRegionalSession next = {.slot = ByteOrder_ReadLe32(bytes + 12),
                            .revision = ByteOrder_ReadLe32(bytes + 32)};
  memcpy(next.campaign, bytes + 16, sizeof(next.campaign));
  ArRegionalTimers_Init(&next.requested.timers, kArRegionalSource_US);
  next.effective.timers = next.requested.timers;
  bool seen[kRecordCount] = {0};
  bool seed_seen = false;
  size_t offset = kHeaderBytes;
  for (unsigned n = 0; n < count; ++n) {
    if (offset == size) return kSaveCheckpoint_Invalid;
    size_t length = bytes[offset++];
    if (!length || length + 8 > size - offset) return kSaveCheckpoint_Invalid;
    unsigned rule;
    const uint16_t *values = NULL;
    for (rule = 0; rule < count; ++rule) {
      const RuleRecord record = Record(&next.requested, rule);
      const char *key = record.key;
      values = record.values;
      if (strlen(key) == length && !memcmp(bytes + offset, key, length)) break;
    }
    if (rule == count) return kSaveCheckpoint_Unsupported;
    if (seen[rule]) return kSaveCheckpoint_Invalid;
    seen[rule] = true;
    offset += length;
    ArRegionalSource requested = DecodeSource(bytes + offset),
                     effective = DecodeSource(bytes + offset + 2);
    if (requested == kArRegionalSource_Count || effective == kArRegionalSource_Count)
      return kSaveCheckpoint_Unsupported;
    if (values[requested] != ByteOrder_ReadLe16(bytes + offset + 4) ||
        values[effective] != ByteOrder_ReadLe16(bytes + offset + 6))
      return kSaveCheckpoint_Unsupported;
    ArRegionalSource *requested_field = Record(&next.requested, rule).source;
    ArRegionalSource *effective_field = Record(&next.effective, rule).source;
    /* The 24 seed records share one policy. Reject contradictory aliases,
     * including reordered records, before overwriting the first value. */
    if (requested_field == &next.requested.lair_seeds) {
      if (seed_seen && (*requested_field != requested || *effective_field != effective))
        return kSaveCheckpoint_Invalid;
      seed_seen = true;
    }
    *requested_field = requested;
    *effective_field = effective;
    offset += 8;
  }
  if (version >= 12) {
    if (size - offset >= 8 && !memcmp(bytes + offset, "ARLHIST", 7) && bytes[offset + 7] != '1')
      return kSaveCheckpoint_Unsupported;
    const size_t history_size = version >= 21 ? kArRegionalLairHistoryEncodedBytes : size - offset;
    if (history_size > size - offset ||
        !ArRegionalLairHistory_Decode(bytes + offset, history_size, &next.lairs))
      return kSaveCheckpoint_Invalid;
    offset += history_size;
    if (version >= 21) {
      if (size - offset >= 8 && !memcmp(bytes + offset, "ARLDELY", 7) && bytes[offset + 7] != '1')
        return kSaveCheckpoint_Unsupported;
      const size_t reload_size = version >= 24 ? kArRegionalLairReloadEncodedBytes : size - offset;
      if (reload_size > size - offset ||
          !ArRegionalLairReloads_Decode(bytes + offset, reload_size, &next.reloads))
        return kSaveCheckpoint_Invalid;
      offset += reload_size;
      if (version >= 24) {
        if (size - offset >= 8 && !memcmp(bytes + offset, "ARSIMAC", 7) &&
            bytes[offset + 7] != (version == 24 ? '1' : '2'))
          return kSaveCheckpoint_Unsupported;
        const size_t actors_size = version >= 28 ? kArRegionalSimActorsEncodedBytes : size - offset;
        if (actors_size > size - offset ||
            !ArRegionalSimActors_Decode(bytes + offset, actors_size, &next.sim_actors))
          return kSaveCheckpoint_Invalid;
        offset += actors_size;
        if (version >= 28) {
          if (size - offset >= 8 && !memcmp(bytes + offset, "ARARRIV", 7) &&
              bytes[offset + 7] != '1')
            return kSaveCheckpoint_Unsupported;
          if (size - offset != (version >= 70 ? 19u+kRandomizerConfigBytes : version >= 51 ? 19u : 9u) ||
              memcmp(bytes + offset, "ARARRIV1", 8) || bytes[offset + 8] > 1)
            return kSaveCheckpoint_Invalid;
          next.arrival_locked = bytes[offset + 8] != 0;
          if (version >= 51) {
            offset += 9;
            if (memcmp(bytes + offset, "ARDIFF01", 8)) return kSaveCheckpoint_Unsupported;
            if (bytes[offset + 8] >= kArRegionalDifficulty_Count ||
                bytes[offset + 9] >= kArRegionalDifficulty_Count)
              return kSaveCheckpoint_Invalid;
            next.requested.difficulty.level = (ArRegionalDifficulty)bytes[offset + 8];
            next.effective.difficulty.level = (ArRegionalDifficulty)bytes[offset + 9];
            if(version>=70) {
              offset+=10;
              if(memcmp(bytes+offset,"ARRANDO1",8) || bytes[offset+8]!=kRandomizerGenerator)
                return kSaveCheckpoint_Unsupported;
              if(!RandomizerConfig_Decode(bytes+offset,size-offset,&next.randomizer))
                return kSaveCheckpoint_Invalid;
            }
          }
        }
      }
    }
  } else if (offset != size)
    return kSaveCheckpoint_Invalid;
  if (!ArRegionalSession_Valid(&next)) return kSaveCheckpoint_Invalid;
  *session = next;
  return kSaveCheckpoint_Ready;
}

bool ArRegionalSession_Encode(const ArRegionalSession *session,void *out,size_t capacity,size_t *size) {
  if(!session || !out || !size)return false;
  uint8_t bytes[kPayloadCapacity];size_t length=0;
  if(!Encode(session,bytes,&length) || length>capacity)return false;
  memcpy(out,bytes,length);*size=length;return true;
}
SaveCheckpointStatus ArRegionalSession_Decode(const void *bytes,size_t size,ArRegionalSession *out) {
  if(!bytes || !out)return kSaveCheckpoint_Invalid;
  return Decode(bytes,size,out);
}

SaveCheckpointStatus ArRegionalSession_Load(ArRegionalSession *session, uint32_t slot,
                                            const char *path, const uint8_t *image,
                                            SaveError *error) {
  if (error) error->message[0] = 0;
  if (!session) return kSaveCheckpoint_Invalid;
  uint8_t payload[kSaveCheckpointPayloadMax];
  size_t size;
  SaveCheckpointStatus status =
      SaveCheckpoint_Read(path, image, payload, sizeof(payload), &size, error);
  if (status != kSaveCheckpoint_Ready) return status;
  ArRegionalSession next;
  status = Decode(payload, size, &next);
  if (status == kSaveCheckpoint_Ready && next.slot != slot) status = kSaveCheckpoint_Mismatch;
  if (status != kSaveCheckpoint_Ready) {
    if (error)
      snprintf(error->message, sizeof(error->message),
               "regional checkpoint is incompatible or belongs to another slot; preserved");
    return status;
  }
  *session = next;
  return kSaveCheckpoint_Ready;
}

static SaveCheckpointStatus ValidatePayload(const uint8_t *payload, size_t size, void *context) {
  ArRegionalSession decoded;
  SaveCheckpointStatus status = Decode(payload, size, &decoded);
  if (status == kSaveCheckpoint_Ready && decoded.slot != *(const uint32_t *)context)
    return kSaveCheckpoint_Mismatch;
  return status;
}

bool ArRegionalSession_Save(const ArRegionalSession *session, SaveFileFormat format,
                            const char *path, const uint8_t *expected, const uint8_t *image,
                            SaveError *error) {
  uint8_t payload[kPayloadCapacity];
  size_t size;
  if (error) error->message[0] = 0;
  if (!Encode(session, payload, &size)) {
    if (error) snprintf(error->message, sizeof(error->message), "invalid regional rules session");
    return false;
  }
  uint32_t slot = session->slot;
  return SaveCheckpoint_Commit(format, path, expected, image, payload, size, ValidatePayload, &slot,
                               error);
}
