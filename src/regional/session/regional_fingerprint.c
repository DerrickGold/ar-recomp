#include "regional/session/regional_fingerprint.h"

#include "byte_order.h"
#include "regional/regional_families.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

bool ArRegionalCosts_Fingerprint(const ArRegionalCostPolicy *requested,
    const ArRegionalCostPolicy *effective, uint8_t out[32], bool *baseline) {
  ArRegionalCostSnapshot pending, active;
  if (!out || !baseline || !ArRegionalCosts_Resolve(requested, &pending) ||
      !ArRegionalCosts_Resolve(effective, &active)) return false;
  uint8_t bytes[512] = "ARCOST-R1";
  size_t used = 9;
  bool native = true;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *rule = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(bytes) - used) return false;
    bytes[used++] = (uint8_t)length;
    memcpy(bytes + used, rule->key, length);
    used += length;
    ByteOrder_WriteLe16(bytes + used, pending.price[i]);
    ByteOrder_WriteLe16(bytes + used + 2, active.price[i]);
    used += 4;
    native &= pending.price[i] == rule->price[kArRegionalSource_US] &&
              active.price[i] == rule->price[kArRegionalSource_US];
  }
  if (!sr_support_sha256(bytes, used, out)) return false;
  *baseline = native;
  return true;
}

/* The rules fingerprint is a chain of layers seeded by the cost digest. Each
 * layer hashes its tag (zero-padded to the digest position), the previous
 * digest and its payload, and is applied only when its rules differ from the
 * native values; so a digest never changes when a newly added rule stays
 * native. The layer list below is frozen history: its order, tags, offsets
 * and payload layouts are part of every recorded identity. Append new layers;
 * never edit or reorder existing ones. */

/* One side's rule values in the form the layers hash. */
typedef enum Flag {
  kFlag_Retry, kFlag_ScorePage, kFlag_Menu, kFlag_Gesture, kFlag_Seeds, kFlag_House,
  kFlag_Phase, kFlag_Lives, kFlag_Reload, kFlag_Level, kFlag_Construction, kFlag_Arrival,
  kFlag_Emitter, kFlag_Volley, kFlag_Collision, kFlag_PlatformSkull, kFlag_CastHold,
  kFlag_Fire, kFlag_Difficulty, kFlag_ScoreLives, kFlag_Inventory, kFlag_Music, kFlag_Terrain,
  kFlag_Artwork, kFlag_Mosaic, kFlag_Poses, kFlag_Sequences, kFlag_Hazards, kFlag_ModeEntry,
  kFlag_ActorArt, kFlag_Count
} Flag;

typedef struct Resolved {
  uint8_t flag[kFlag_Count];
  uint16_t wait, fish, speed, skull, combat, ai, motion;
  uint64_t boss;
  ArRegionalQuakeSnapshot quake;
  ArRegionalScoreSnapshot score;
  ArRegionalSourcesSnapshot sources;
  ArRegionalStorySnapshot story;
  ArRegionalTownStatusSnapshot status;
  ArRegionalSupportSnapshot support;
  ArRegionalActorStatsSnapshot stats;
  ArRegionalActionStartSnapshot start;
} Resolved;

typedef struct Side {
  const ArRegionalRules *rules;
  Resolved resolved;
} Side;

/* Every family the fingerprint depends on must resolve; any invalid source
 * rejects the whole fingerprint. */
static bool ResolveSide(const ArRegionalRules *r, Resolved *out) {
  memset(out, 0, sizeof(*out));
  uint8_t *flag = out->flag;
  if ((unsigned)r->lair_reloads >= kArRegionalSource_Count ||
      (unsigned)r->lair_seeds >= kArRegionalSource_Count ||
      (unsigned)r->house_credit >= kArRegionalSource_Count ||
      !ArRegionalTimers_Valid(&r->timers) || !ArRegionalPlacements_Valid(&r->placements))
    return false;
  bool retry, score_page, menu, gesture, lives, level, construction, arrival, volley;
  bool score_lives, inventory;
  ArRegionalDevelopmentSnapshot development;
  ArRegionalRecoverySnapshot recovery;
  ArRegionalDifficultySnapshot difficulty;
  if (!ArRegionalMode_Resolve(&r->mode_entry, &flag[kFlag_ModeEntry]) ||
      !ArRegionalActorArtwork_Resolve(&r->actor_artwork, &flag[kFlag_ActorArt]) ||
      !ArRegionalArtwork_Resolve(&r->artwork, &flag[kFlag_Artwork]) ||
      !ArRegionalSequences_Resolve(&r->sequences, &flag[kFlag_Sequences]) ||
      !ArRegionalPoses_Resolve(&r->poses, &flag[kFlag_Poses]) ||
      !ArRegionalMosaic_Resolve(r->mosaic, &flag[kFlag_Mosaic]) ||
      !ArRegionalMusic_Resolve(r->music, &flag[kFlag_Music]) ||
      !ArRegionalTerrain_Resolve(r->terrain, &flag[kFlag_Terrain]) ||
      !ArRegionalHazards_Resolve(r->hazards, &flag[kFlag_Hazards]) ||
      !ArRegionalInventory_Resolve(r->spell_inventory, &inventory) ||
      !ArRegionalActionStart_Resolve(&r->action_start, &out->start) ||
      !ArRegionalScoreLives_Resolve(r->score_lives, &score_lives) ||
      !ArRegionalDifficulty_Resolve(&r->difficulty, &difficulty) ||
      !ArRegionalFire_Resolve(&r->fire_enemy, &flag[kFlag_Fire]) ||
      !ArRegionalCastHold_Resolve(&r->cast_hold, &flag[kFlag_CastHold]) ||
      !ArRegionalActorStats_Resolve(&r->actor_stats, &out->stats) ||
      !ArRegionalPlatformSkull_Resolve(&r->platform_skull, &flag[kFlag_PlatformSkull]) ||
      !ArRegionalCollision_Resolve(&r->collision, &flag[kFlag_Collision]) ||
      !ArRegionalBoss_Resolve(&r->bosses, &out->boss) ||
      !ArRegionalVolley_Resolve(r->statue_volley, &volley) ||
      !ArRegionalEmitter_Resolve(&r->emitters, &flag[kFlag_Emitter]) ||
      !ArRegionalActionMotion_Resolve(&r->action_motion, &out->motion) ||
      !ArRegionalArrival_Resolve(r->arrival, &arrival) ||
      !ArRegionalRetry_Resolve(r->retry_score, &retry) ||
      !ArRegionalTownWait_Resolve(r->town_wait, &out->wait) ||
      !ArRegionalFishing_Resolve(r->fishing, &out->fish) ||
      !ArRegionalDevelopment_Resolve(&r->development, &development) ||
      !ArRegionalRecovery_Resolve(&r->recovery, &recovery) ||
      !ArRegionalQuake_Resolve(&r->quake, &out->quake) ||
      !ArRegionalSupport_Resolve(&r->support, &out->support) ||
      !ArRegionalConstruction_Resolve(r->construction, &construction) ||
      !ArRegionalSimAi_Resolve(&r->sim_ai, &out->ai) ||
      !ArRegionalSimCombat_Resolve(&r->sim_combat, &out->combat) ||
      !ArRegionalLevelGoals_Resolve(r->level_goals, &level) ||
      !ArRegionalTownStatus_Resolve(&r->town_status, &out->status) ||
      !ArRegionalStory_Resolve(&r->story, &out->story) ||
      !ArRegionalSkullWait_Resolve(r->skull_wait, &out->skull) ||
      !ArRegionalSources_Resolve(&r->sources, &out->sources) ||
      !ArRegionalLivesDisplay_Resolve(r->lives_display, &lives) ||
      !ArRegionalScorePage_Resolve(r->score_page, &score_page) ||
      !ArRegionalMenuReturn_Resolve(r->menu_return, &menu) ||
      !ArRegionalSpeedRange_Resolve(r->speed_range, &out->speed) ||
      !ArRegionalMagicGesture_Resolve(r->magic_gesture, &gesture) ||
      !ArRegionalScore_Resolve(&r->score_feedback, &out->score))
    return false;
  flag[kFlag_Retry] = retry;
  flag[kFlag_ScorePage] = score_page;
  flag[kFlag_Menu] = menu;
  flag[kFlag_Gesture] = gesture;
  flag[kFlag_Seeds] = r->lair_seeds == kArRegionalSource_Japan;
  flag[kFlag_House] = r->house_credit == kArRegionalSource_Japan;
  flag[kFlag_Phase] = out->score.japanese[kArRegionalScore_Phase];
  flag[kFlag_Lives] = lives;
  flag[kFlag_Reload] = r->lair_reloads == kArRegionalSource_Japan;
  flag[kFlag_Level] = level;
  flag[kFlag_Construction] = construction;
  flag[kFlag_Arrival] = arrival;
  flag[kFlag_Volley] = volley;
  flag[kFlag_Difficulty] = ArRegionalDifficulty_Identity(&difficulty);
  flag[kFlag_ScoreLives] = score_lives;
  flag[kFlag_Inventory] = inventory;
  return true;
}

/* A layer's hash input: tag and padding, previous digest, then payload. */
typedef struct Payload {
  uint8_t *bytes;
  size_t used, capacity;
  bool failed;
} Payload;

static void PutByte(Payload *p, unsigned value) {
  if (p->used + 1 > p->capacity) { p->failed = true; return; }
  p->bytes[p->used++] = (uint8_t)value;
}
static void PutWord(Payload *p, unsigned value) {
  if (p->used + 2 > p->capacity) { p->failed = true; return; }
  ByteOrder_WriteLe16(p->bytes + p->used, (uint16_t)value);
  p->used += 2;
}
static void PutKeyed(Payload *p, const char *key, uint16_t pending, uint16_t active) {
  const size_t length = strlen(key);
  if (length > 255 || length + 5 > p->capacity - p->used) { p->failed = true; return; }
  p->bytes[p->used++] = (uint8_t)length;
  memcpy(p->bytes + p->used, key, length);
  p->used += length;
  PutWord(p, pending);
  PutWord(p, active);
}

/* Layer builders write the payload and return whether the layer is native.
 * Each keeps its original native test verbatim. */
typedef bool (*LayerBuild)(const Side *pending, const Side *active, Payload *payload);

/* Keyed list over a family's descriptor values; native when both sides
 * resolve to the US value of every rule. */
static bool KeyedFamily(const Side *pending, const Side *active, Payload *p,
                        ArRegionalFamilyId family) {
  const ArRegionalFamily *row = ArRegionalFamilies_Get(family);
  bool native = true;
  for (unsigned rule = 0; rule < row->rules; ++rule) {
    const ArRegionalRuleInfo info = row->info(rule);
    const uint16_t now = info.values[ArRegionalFamilies_Source(pending->rules, family, rule)];
    const uint16_t then = info.values[ArRegionalFamilies_Source(active->rules, family, rule)];
    PutKeyed(p, info.key, now, then);
    native &= now == info.values[kArRegionalSource_US] &&
              then == info.values[kArRegionalSource_US];
  }
  return native;
}
static bool TimersLayer(const Side *pending, const Side *active, Payload *p) {
  return KeyedFamily(pending, active, p, kArRegionalFamily_Timers);
}
static bool DevelopmentLayer(const Side *pending, const Side *active, Payload *p) {
  return KeyedFamily(pending, active, p, kArRegionalFamily_Development);
}
static bool RecoveryLayer(const Side *pending, const Side *active, Payload *p) {
  return KeyedFamily(pending, active, p, kArRegionalFamily_Recovery);
}
static bool QuakeLayer(const Side *pending, const Side *active, Payload *p) {
  bool native = true;
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) {
    const bool now = pending->resolved.quake.random[i], then = active->resolved.quake.random[i];
    PutKeyed(p, ArRegionalQuake_Descriptor((ArRegionalQuakeRule)i)->key, now, then);
    native &= !now && !then;
  }
  return native;
}
/* Keeps the v15 arithmetic identity: phase has its own layer. */
static bool ScoreStockLayer(const Side *pending, const Side *active, Payload *p) {
  bool native = true;
  for (unsigned i = 0; i < kArRegionalScore_Phase; ++i) {
    const bool now = pending->resolved.score.japanese[i], then = active->resolved.score.japanese[i];
    PutKeyed(p, ArRegionalScore_Descriptor((ArRegionalScoreRule)i)->key, now, then);
    native &= !now && !then;
  }
  return native;
}
static bool WordPair(Payload *p, uint16_t now, uint16_t then, uint16_t native) {
  PutWord(p, now);
  PutWord(p, then);
  return now == native && then == native;
}
static bool TownWaitLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.wait, active->resolved.wait,
                  ArRegionalTownWait_Descriptor()->updates[kArRegionalSource_US]);
}
static bool FishingLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.fish, active->resolved.fish,
                  ArRegionalFishing_Descriptor()->updates[kArRegionalSource_US]);
}
static bool SkullWaitLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.skull, active->resolved.skull, 90);
}
static bool SimCombatLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.combat, active->resolved.combat, 0);
}
static bool SimAiLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.ai, active->resolved.ai, 0);
}
static bool ActionMotionLayer(const Side *pending, const Side *active, Payload *p) {
  return WordPair(p, pending->resolved.motion, active->resolved.motion, 0);
}
static bool SpeedRangeLayer(const Side *pending, const Side *active, Payload *p) {
  PutByte(p, pending->resolved.speed);
  PutByte(p, active->resolved.speed);
  return pending->resolved.speed == 9 && active->resolved.speed == 9;
}
static bool SourcesLayer(const Side *pending, const Side *active, Payload *p) {
  _Static_assert(kArRegionalSourceItem_Count == 2, "ARSOURCES-R1 has two collection leaves");
  bool native = true;
  for (unsigned i = 0; i < kArRegionalSourceItem_Count; ++i) {
    const bool now = pending->resolved.sources.automatic[i];
    const bool then = active->resolved.sources.automatic[i];
    PutByte(p, now);
    PutByte(p, then);
    native &= now && then;
  }
  return native;
}
static bool StoryLayer(const Side *pending, const Side *active, Payload *p) {
  _Static_assert(kArRegionalStory_Count == 3, "ARSTORY-R1 has three prerequisite leaves");
  bool native = true;
  for (unsigned i = 0; i < kArRegionalStory_Count; ++i) {
    const unsigned us =
        ArRegionalStory_Descriptor((ArRegionalStoryRule)i)->value[kArRegionalSource_US];
    native &= WordPair(p, pending->resolved.story.value[i], active->resolved.story.value[i], us);
  }
  return native;
}
static bool TownStatusLayer(const Side *pending, const Side *active, Payload *p) {
  _Static_assert(kArRegionalTownStatus_Count == 5, "ARTOWNSTATUS-R1 has five reporting leaves");
  bool native = true;
  for (unsigned i = 0; i < kArRegionalTownStatus_Count; ++i) {
    const uint16_t now = pending->resolved.status.japanese[i];
    const uint16_t then = active->resolved.status.japanese[i];
    PutByte(p, now);
    PutByte(p, then);
    native &= !now && !then;
  }
  return native;
}
static bool SupportLayer(const Side *pending, const Side *active, Payload *p) {
  bool native = true;
  for (unsigned i = 0; i < kArRegionalSupport_Count; ++i) {
    const unsigned us =
        ArRegionalSupport_Descriptor((ArRegionalSupportRule)i)->amount[kArRegionalSource_US];
    native &= WordPair(p, pending->resolved.support.amount[i], active->resolved.support.amount[i],
                       us);
  }
  return native;
}
static bool BossLayer(const Side *pending, const Side *active, Payload *p) {
  for (unsigned i = 0; i < 8; ++i) PutByte(p, (uint8_t)(pending->resolved.boss >> (8 * i)));
  for (unsigned i = 0; i < 8; ++i) PutByte(p, (uint8_t)(active->resolved.boss >> (8 * i)));
  return !pending->resolved.boss && !active->resolved.boss;
}
static bool ActorStatsLayer(const Side *pending, const Side *active, Payload *p) {
  _Static_assert(kArRegionalActorStat_BaseCount == 63, "preserve actor-stat replay domain");
  for (unsigned i = 0; i < kArRegionalActorStat_BaseCount; ++i)
    PutByte(p, pending->resolved.stats.value[i]);
  for (unsigned i = 0; i < kArRegionalActorStat_BaseCount; ++i)
    PutByte(p, active->resolved.stats.value[i]);
  return !pending->resolved.stats.changed && !active->resolved.stats.changed;
}
static bool ChildStatsLayer(const Side *pending, const Side *active, Payload *p) {
  _Static_assert(kArRegionalActorStat_Count - kArRegionalActorStat_BaseCount == 3,
                 "preserve child-stat replay domain");
  bool native = true;
  for (unsigned i = kArRegionalActorStat_BaseCount; i < kArRegionalActorStat_Count; ++i) {
    const uint16_t us = ArRegionalActorStats_Descriptor(i)->value[0];
    native &= pending->resolved.stats.value[i] == us && active->resolved.stats.value[i] == us;
  }
  for (unsigned i = kArRegionalActorStat_BaseCount; i < kArRegionalActorStat_Count; ++i)
    PutByte(p, pending->resolved.stats.value[i]);
  for (unsigned i = kArRegionalActorStat_BaseCount; i < kArRegionalActorStat_Count; ++i)
    PutByte(p, active->resolved.stats.value[i]);
  return native;
}
static bool ActionStartLayer(const Side *pending, const Side *active, Payload *p) {
  const ArRegionalActionStartSnapshot now = pending->resolved.start, then = active->resolved.start;
  PutByte(p, now.spares);
  PutByte(p, now.health);
  PutByte(p, then.spares);
  PutByte(p, then.health);
  return now.spares == 4 && now.health == 24 && then.spares == 4 && then.health == 24;
}
static bool PlacementsLayer(const Side *pending, const Side *active, Payload *p) {
  const ArRegionalRules *now = pending->rules, *then = active->rules;
  PutByte(p, now->placements.enemies);
  PutByte(p, now->placements.pickups);
  PutByte(p, then->placements.enemies);
  PutByte(p, then->placements.pickups);
  PutByte(p, now->placements.enemies == kArRegionalSource_Europe ? now->difficulty.level : 0);
  PutByte(p, then->placements.enemies == kArRegionalSource_Europe ? then->difficulty.level : 0);
  return !now->placements.enemies && !now->placements.pickups &&
         !then->placements.enemies && !then->placements.pickups;
}

/* A layer either runs a builder or hashes one resolved flag per side, native
 * when both equal `native`. */
typedef struct Layer {
  const char *tag;
  uint8_t digest_offset;
  uint16_t capacity;
  LayerBuild build;
  uint8_t flag, native;
} Layer;
enum { kLayerCapacity = 512 };
static const Layer kLayers[] = {
  {"ARTIME-R1", 9, 512, TimersLayer, 0, 0},
  {"ARRETRY-R1", 10, 64, NULL, kFlag_Retry, 0},
  {"ARTOWNWAIT-R1", 14, 64, TownWaitLayer, 0, 0},
  {"ARFISHING-R1", 12, 64, FishingLayer, 0, 0},
  {"ARDEVELOP-R1", 12, 256, DevelopmentLayer, 0, 0},
  {"ARRECOVERY-R1", 14, 256, RecoveryLayer, 0, 0},
  {"ARQUAKE-R1", 10, 256, QuakeLayer, 0, 0},
  {"ARSCOREPAGE-R1", 14, 48, NULL, kFlag_ScorePage, 1},
  {"ARMENURETURN-R1", 16, 50, NULL, kFlag_Menu, 0},
  {"ARSPEEDRANGE-R1", 16, 50, SpeedRangeLayer, 0, 0},
  {"ARMAGICGESTURE-R1", 18, 52, NULL, kFlag_Gesture, 0},
  {"ARLAIRSEEDS-R1", 16, 50, NULL, kFlag_Seeds, 0},
  {"ARHOUSECREDIT-R1", 16, 50, NULL, kFlag_House, 0},
  {"ARSCORESTOCK-R1", 16, 192, ScoreStockLayer, 0, 0},
  {"ARSCOREPHASE-R1", 16, 50, NULL, kFlag_Phase, 0},
  {"ARLIFEDISPLAY-R1", 16, 50, NULL, kFlag_Lives, 0},
  {"ARSOURCES-R1", 16, 52, SourcesLayer, 0, 0},
  {"ARSKULLWAIT-R1", 16, 52, SkullWaitLayer, 0, 0},
  {"ARSTORY-R1", 16, 60, StoryLayer, 0, 0},
  {"ARRELOADPOL-R1", 16, 50, NULL, kFlag_Reload, 0},
  {"ARTOWNSTATUS-R1", 16, 58, TownStatusLayer, 0, 0},
  {"ARLEVELGOALS-R1", 16, 50, NULL, kFlag_Level, 0},
  {"ARSIMCOMBAT-R1", 16, 52, SimCombatLayer, 0, 0},
  {"ARSIMAI-R1", 16, 52, SimAiLayer, 0, 0},
  {"ARBUILDPRICE-R1", 16, 50, NULL, kFlag_Construction, 0},
  {"ARSUPPORT-R1", 16, 48 + 4 * kArRegionalSupport_Count, SupportLayer, 0, 0},
  {"ARARRIVAL-R1", 16, 50, NULL, kFlag_Arrival, 0},
  {"ARACTIONMOVE-R1", 16, 52, ActionMotionLayer, 0, 0},
  {"AREMITTER-R1", 16, 50, NULL, kFlag_Emitter, 0},
  {"ARVOLLEY-R1", 16, 50, NULL, kFlag_Volley, 0},
  {"ARBOSS-R1", 16, 64, BossLayer, 0, 0},
  {"ARCOLLISION-R1", 16, 50, NULL, kFlag_Collision, 0},
  {"ARPLATSKULL-R1", 16, 50, NULL, kFlag_PlatformSkull, 0},
  {"ARACTORSTAT-R1", 16, 48 + 2 * kArRegionalActorStat_BaseCount, ActorStatsLayer, 0, 0},
  {"ARCHILDSTAT-R1", 16, 54, ChildStatsLayer, 0, 0},
  {"ARCASTHOLD-R1", 16, 50, NULL, kFlag_CastHold, 0},
  {"ARFIRE-R1", 16, 50, NULL, kFlag_Fire, 0},
  {"ARDIFF-R1", 16, 50, NULL, kFlag_Difficulty, 0},
  {"ARSCORELIVES-R1", 16, 50, NULL, kFlag_ScoreLives, 0},
  {"ARACTIONSTART-R1", 16, 52, ActionStartLayer, 0, 0},
  {"ARINVENTORY-R1", 16, 50, NULL, kFlag_Inventory, 0},
  {"ARPLACEMENTS-R1", 16, 54, PlacementsLayer, 0, 0},
  {"ARMUSICROUTE-R1", 16, 50, NULL, kFlag_Music, 0},
  {"ARTERRAIN-R1", 16, 50, NULL, kFlag_Terrain, 0},
  {"ARARTWORK-R1", 16, 50, NULL, kFlag_Artwork, 0},
  {"ARMOSAIC-R1", 16, 50, NULL, kFlag_Mosaic, 0},
  {"ARPOSES-R1", 16, 50, NULL, kFlag_Poses, 0},
  {"ARSEQUENCE-R1", 16, 50, NULL, kFlag_Sequences, 0},
  {"ARHAZARDS-R1", 16, 50, NULL, kFlag_Hazards, 0},
  {"ARMODEENTRY-R1", 16, 50, NULL, kFlag_ModeEntry, 0},
  {"ARACTORART-R1", 16, 50, NULL, kFlag_ActorArt, 0},
};

bool ArRegionalRules_Fingerprint(const ArRegionalRules *requested,
    const ArRegionalRules *effective,
    uint8_t out[32], bool *baseline) {
  if (!requested || !effective || !out || !baseline) return false;
  Side pending = {requested}, active = {effective};
  if (!ResolveSide(requested, &pending.resolved) || !ResolveSide(effective, &active.resolved))
    return false;
  uint8_t digest[32];
  bool native;
  if (!ArRegionalCosts_Fingerprint(&requested->costs, &effective->costs, digest, &native))
    return false;
  for (unsigned i = 0; i < sizeof(kLayers) / sizeof(kLayers[0]); ++i) {
    const Layer *layer = &kLayers[i];
    uint8_t bytes[kLayerCapacity] = {0};
    memcpy(bytes, layer->tag, strlen(layer->tag));
    memcpy(bytes + layer->digest_offset, digest, sizeof(digest));
    Payload payload = {bytes, layer->digest_offset + sizeof(digest), layer->capacity, false};
    bool layer_native;
    if (layer->build) {
      layer_native = layer->build(&pending, &active, &payload);
    } else {
      const uint8_t now = pending.resolved.flag[layer->flag];
      const uint8_t then = active.resolved.flag[layer->flag];
      PutByte(&payload, now);
      PutByte(&payload, then);
      layer_native = now == layer->native && then == layer->native;
    }
    if (payload.failed) return false;
    if (!layer_native && !sr_support_sha256(bytes, payload.used, digest)) return false;
    native &= layer_native;
  }
  memcpy(out, digest, sizeof(digest));
  *baseline = native;
  return true;
}

bool ArRegionalSpellInventory_Fingerprint(const uint8_t previous[32],
    const ArRegionalSpellInventory *inventory,uint8_t out[32]) {
  if(!previous || !out || !ArRegionalSpellInventory_Valid(inventory))return false;
  if(!inventory->enabled) {memmove(out,previous,32);return true;}
  uint8_t bytes[51 + 256] = "ARSPELLSTACK-R1";
  memcpy(bytes + 16, previous, 32);
  bytes[48] = inventory->count;
  bytes[49] = inventory->icon;
  bytes[50] = inventory->casting;
  memcpy(bytes+51,inventory->spells,inventory->count);
  return sr_support_sha256(bytes,51+inventory->count,out);
}

bool ArRegionalArrivalLock_Fingerprint(const uint8_t previous[32],ArRegionalSource requested,
    ArRegionalSource effective,bool locked,uint8_t out[32]) {
  bool req,eff;
  if (!previous || !out || !ArRegionalArrival_Resolve(requested, &req) ||
      !ArRegionalArrival_Resolve(effective, &eff))
    return false;
  if(!req && !eff){memmove(out,previous,32);return true;}
  uint8_t bytes[49] = "ARARRLOCK-R1";
  memcpy(bytes + 16, previous, 32);
  bytes[48] = locked;
  return sr_support_sha256(bytes,sizeof(bytes),out);
}

bool ArRegionalSimActors_Fingerprint(const uint8_t previous[32],const ArRegionalSimActors *actors,
                                    uint8_t out[32],bool *baseline) {
  if (!previous || !out || !baseline || !ArRegionalSimActors_Valid(actors)) return false;
  bool native=true,ai_native=true;
  for (unsigned i = 0; i < 24; ++i) {
    native &= !actors->cached[i].combat;
    ai_native &= !actors->cached[i].ai;
  }
  for (unsigned i = 0; i < 4; ++i) {
    native &= !actors->active[i].combat;
    ai_native &= !actors->active[i].ai;
  }
  native &= ai_native;
  if (native) { memmove(out,previous,32);*baseline=true;return true; }
  uint8_t bytes[48+kArRegionalSimActorsEncodedBytes]="ARSIMACTOR-R1";
  if (!ai_native) memcpy(bytes,"ARSIMACTOR-R2",13);
  memcpy(bytes+16,previous,32);
  const size_t encoded =
      ai_native ? kArRegionalSimActorsV1EncodedBytes : kArRegionalSimActorsEncodedBytes;
  if (!ArRegionalSimActors_EncodeVersion(actors, bytes + 48, encoded, ai_native ? 1 : 2) ||
      !sr_support_sha256(bytes, 48 + encoded, out))
    return false;
  *baseline=false;return true;
}
