#include "regional/session/regional_session_internal.h"
#include "regional/regional_families.h"
#include "byte_order.h"

#include <stdio.h>
#include <string.h>

/* Stable v1-v70 persistence, separate from live activation. See
 * docs/save-format.md for the wire contract before extending. */
enum {
  kHeaderBytes = 36,
  kPayloadCapacity = kSaveCheckpointPayloadMax,
  /* Upper bound for the per-decode duplicate table; WireRecordCount() is the
   * real total and is checked against it. */
  kRecordCapacity = 512,
};

/* Save wire order: records were appended one format version at a time, and a
 * record's position never changes once written. This list is frozen history.
 * Extend it only by appending a span with the new version, never by growing an
 * existing span. A span covers the family's rules [first, first + count). */
typedef struct WireSpan {
  uint8_t version;
  uint8_t family;
  uint8_t first, count;
} WireSpan;
static const WireSpan kWire[] = {
  {1, kArRegionalFamily_Costs, 0, kArRegionalCostRule_Count},
  {1, kArRegionalFamily_Timers, 0, kArRegionalTimerRule_Count},
  {2, kArRegionalFamily_RetryScore, 0, 1},
  {3, kArRegionalFamily_TownWait, 0, 1},
  {4, kArRegionalFamily_Fishing, 0, 1},
  {5, kArRegionalFamily_Development, 0, kArRegionalDevelopmentRule_Count},
  {6, kArRegionalFamily_Recovery, 0, kArRegionalRecovery_Count},
  {7, kArRegionalFamily_Quake, 0, kArRegionalQuake_Count},
  {8, kArRegionalFamily_ScorePage, 0, 1},
  {9, kArRegionalFamily_MenuReturn, 0, 1},
  {10, kArRegionalFamily_SpeedRange, 0, 1},
  {11, kArRegionalFamily_MagicGesture, 0, 1},
  {13, kArRegionalFamily_LairSeeds, 0, kArRegionalLairCount},
  {14, kArRegionalFamily_HouseCredit, 0, 1},
  {15, kArRegionalFamily_ScoreFeedback, 0, kArRegionalScore_Phase},
  {16, kArRegionalFamily_ScoreFeedback, kArRegionalScore_Phase, 1},
  {17, kArRegionalFamily_LivesDisplay, 0, 1},
  {18, kArRegionalFamily_Sources, 0, kArRegionalSourceItem_Count},
  {19, kArRegionalFamily_SkullWait, 0, 1},
  {20, kArRegionalFamily_Story, 0, kArRegionalStory_Count},
  {21, kArRegionalFamily_LairReloads, 0, 1},
  {22, kArRegionalFamily_TownStatus, 0, kArRegionalTownStatus_Count},
  {23, kArRegionalFamily_LevelGoals, 0, 1},
  {24, kArRegionalFamily_SimCombat, 0, kArRegionalSimCombat_Count},
  {25, kArRegionalFamily_SimAi, 0, kArRegionalSimAi_Count},
  {26, kArRegionalFamily_Construction, 0, 1},
  {27, kArRegionalFamily_Support, 0, kArRegionalSupport_Count},
  {28, kArRegionalFamily_Arrival, 0, 1},
  {29, kArRegionalFamily_ActionMotion, 0, 7},
  {30, kArRegionalFamily_ActionMotion, 7, 2},
  {31, kArRegionalFamily_ActionMotion, 9, 1},
  {32, kArRegionalFamily_ActionMotion, 10, 2},
  {33, kArRegionalFamily_Emitters, 0, kArRegionalEmitter_Count},
  {34, kArRegionalFamily_StatueVolley, 0, 1},
  {35, kArRegionalFamily_Bosses, 0, 6},
  {36, kArRegionalFamily_Bosses, 6, 1},
  {37, kArRegionalFamily_Collision, 0, kArRegionalCollision_Count},
  {38, kArRegionalFamily_PlatformSkull, 0, kArRegionalPlatformSkull_Count},
  {39, kArRegionalFamily_ActorStats, 0, kArRegionalActorStat_BaseCount},
  {40, kArRegionalFamily_ActorStats, kArRegionalActorStat_BaseCount,
   kArRegionalActorStat_Count - kArRegionalActorStat_BaseCount},
  {41, kArRegionalFamily_Bosses, 7, 4},
  {42, kArRegionalFamily_CastHold, 0, kArRegionalCastHold_Count},
  {43, kArRegionalFamily_Bosses, 11, 2},
  {44, kArRegionalFamily_FireEnemy, 0, kArRegionalFire_Count},
  {45, kArRegionalFamily_Bosses, 13, 2},
  {46, kArRegionalFamily_Bosses, 15, 4},
  {47, kArRegionalFamily_ActionMotion, kArRegionalActionMotion_HeadWithdrawal, 1},
  {48, kArRegionalFamily_Bosses, 19, 3},
  {49, kArRegionalFamily_Bosses, 22, 4},
  {50, kArRegionalFamily_ActionMotion, kArRegionalActionMotion_TreeSeeds, 1},
  {51, kArRegionalFamily_Difficulty, 0, kArRegionalDifficultyRule_Count},
  {52, kArRegionalFamily_ScoreLives, 0, 1},
  {53, kArRegionalFamily_ActionStart, 0, kArRegionalActionStart_Count},
  {54, kArRegionalFamily_SpellInventory, 0, 1},
  {55, kArRegionalFamily_ModeEntry, 0, kArRegionalMode_Count},
  {56, kArRegionalFamily_Bosses, 26, 4},
  {57, kArRegionalFamily_Bosses, kArRegionalBoss_PlantGeometry, 1},
  {58, kArRegionalFamily_Hazards, 0, 1},
  {59, kArRegionalFamily_Terrain, 0, 1},
  {60, kArRegionalFamily_Music, 0, 1},
  {61, kArRegionalFamily_Placements, 0, kArRegionalPlacement_Count},
  {62, kArRegionalFamily_Mosaic, 0, 1},
  {63, kArRegionalFamily_Artwork, 0, 1},
  {64, kArRegionalFamily_Artwork, 1, 1},
  {65, kArRegionalFamily_Artwork, 2, 3},
  {66, kArRegionalFamily_Artwork, 5, 1},
  {67, kArRegionalFamily_Poses, 0, kArRegionalPose_Count},
  {68, kArRegionalFamily_Sequences, 0, kArRegionalSequence_Count},
  {69, kArRegionalFamily_ActorArtwork, 0, kArRegionalActorArtwork_Count},
};

/* Records a payload of this format version carries: every span introduced
 * at or before it. v11 and v12, and v69 and v70, share a count. */
static unsigned WireRecordCount(unsigned version) {
  unsigned count = 0;
  for (unsigned i = 0; i < sizeof(kWire) / sizeof(kWire[0]); ++i)
    if (kWire[i].version <= version) count += kWire[i].count;
  return count;
}
static unsigned LatestRecordCount(void) { return WireRecordCount(UINT8_MAX); }
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

/* One record binds a stable key, its regional values and the source field,
 * resolved through the family table in wire order (kWire). Encode and decode
 * use this same binding, so neither keeps an ordinal-to-field switch. */
typedef struct RuleRecord {
  const char *key;
  const uint16_t *values;
  ArRegionalSource *source;
} RuleRecord;

static RuleRecord Record(ArRegionalRules *rules, unsigned index) {
  for (unsigned i = 0; i < sizeof(kWire) / sizeof(kWire[0]); ++i) {
    if (index < kWire[i].count) {
      const ArRegionalFamilyId family = (ArRegionalFamilyId)kWire[i].family;
      const unsigned rule = kWire[i].first + index;
      const ArRegionalRuleInfo info = ArRegionalFamilies_Get(family)->info(rule);
      return (RuleRecord){info.key, info.values, ArRegionalFamilies_Field(rules, family, rule)};
    }
    index -= kWire[i].count;
  }
  return (RuleRecord){NULL, NULL, NULL};
}

/* Compact explicit codec, never fwrite a C struct or persist enum ordinals.
 * Each named leaf carries requested/effective source keys AND resolved values.
 * A table/schema change that cannot reproduce those values fails visibly. */
static bool Encode(const ArRegionalSession *session, uint8_t *out, size_t *size) {
  if (!ArRegionalSession_Valid(session)) return false;
  memset(out, 0, kHeaderBytes);
  memcpy(out, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(out + 8, session->randomizer.generator ? 70 : 69);
  const unsigned record_count = LatestRecordCount();
  ByteOrder_WriteLe16(out + 10, record_count);
  ByteOrder_WriteLe32(out + 12, session->slot);
  memcpy(out + 16, session->campaign, 16);
  ByteOrder_WriteLe32(out + 32, session->revision);
  /* Read-only encoding uses local rule copies so the shared typed binding
   * needs no const cast and can also provide writable fields to the decoder. */
  ArRegionalRules requested_rules = session->requested;
  ArRegionalRules effective_rules = session->effective;
  size_t offset = kHeaderBytes;
  for (unsigned i = 0; i < record_count; ++i) {
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
  const unsigned count =
      pricing_only ? (unsigned)kArRegionalCostRule_Count : WireRecordCount(version);
  if (count > kRecordCapacity) return kSaveCheckpoint_Invalid;
  if (ByteOrder_ReadLe16(bytes + 10) != count) return kSaveCheckpoint_Unsupported;
  ArRegionalSession next = {.slot = ByteOrder_ReadLe32(bytes + 12),
                            .revision = ByteOrder_ReadLe32(bytes + 32)};
  memcpy(next.campaign, bytes + 16, sizeof(next.campaign));
  ArRegionalTimers_Init(&next.requested.timers, kArRegionalSource_US);
  next.effective.timers = next.requested.timers;
  bool seen[kRecordCapacity] = {0};
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
          if (size - offset !=
                  (version >= 70       ? 19u + kRandomizerConfigBytes
                       : version >= 51 ? 19u
                                       : 9u) ||
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

bool ArRegionalSession_Encode(const ArRegionalSession *session, void *out, size_t capacity,
                              size_t *size) {
  if(!session || !out || !size)return false;
  uint8_t bytes[kPayloadCapacity];
  size_t length = 0;
  if(!Encode(session,bytes,&length) || length>capacity)return false;
  memcpy(out, bytes, length);
  *size = length;
  return true;
}
SaveCheckpointStatus ArRegionalSession_Decode(const void *bytes, size_t size,
                                              ArRegionalSession *out) {
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
