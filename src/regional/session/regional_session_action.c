#include "regional/session/regional_session_action.h"
#include "regional/session/regional_session_internal.h"

#include <string.h>

/* Private activation primitives require a validated session. Public family
 * entry points validate independently; the room transaction validates once
 * and applies these same primitives to a detached candidate. No global bypass
 * flag, partial publication, or second implementation of activation semantics. */
static bool ActivateTimers(ArRegionalSession *session, ArRegionalTimerPolicy *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.timers, &session->effective.timers,
                              sizeof(session->requested.timers)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  session->effective.timers = session->requested.timers;
  if (changed) ++session->revision;
  *snapshot = session->effective.timers;
  return true;
}

bool ArRegionalSession_BeginTimers(ArRegionalSession *session, ArRegionalTimerPolicy *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateTimers(session, snapshot);
}

static bool ActivateActionMotion(ArRegionalSession *session,
                                 ArRegionalActionMotionSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.action_motion, &session->effective.action_motion,
                              sizeof(session->requested.action_motion)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t next;
  if (!ArRegionalActionMotion_Resolve(&session->requested.action_motion, &next)) return false;
  session->effective.action_motion = session->requested.action_motion;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginActionMotion(ArRegionalSession *session,
                                         ArRegionalActionMotionSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateActionMotion(session, snapshot);
}

static bool ActivateEmitters(ArRegionalSession *session, ArRegionalEmitterSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.emitters, &session->effective.emitters,
                              sizeof(session->requested.emitters)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalEmitter_Resolve(&session->requested.emitters, &next)) return false;
  session->effective.emitters = session->requested.emitters;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginEmitters(ArRegionalSession *session,
                                     ArRegionalEmitterSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateEmitters(session, snapshot);
}

static bool ActivateVolley(ArRegionalSession *session, bool *double_shot) {
  if (!double_shot) return false;
  const bool changed = session->requested.statue_volley != session->effective.statue_volley;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalVolley_Resolve(session->requested.statue_volley, &next)) return false;
  session->effective.statue_volley = session->requested.statue_volley;
  if (changed) ++session->revision;
  *double_shot = next;
  return true;
}

bool ArRegionalSession_BeginVolley(ArRegionalSession *session, bool *double_shot) {
  return ArRegionalSession_Valid(session) && ActivateVolley(session, double_shot);
}

static bool ActivateBosses(ArRegionalSession *session, ArRegionalBossSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.bosses, &session->effective.bosses,
                              sizeof(session->requested.bosses)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint64_t next;
  if (!ArRegionalBoss_Resolve(&session->requested.bosses, &next)) return false;
  session->effective.bosses = session->requested.bosses;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginBosses(ArRegionalSession *session, ArRegionalBossSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateBosses(session, snapshot);
}

static bool ActivateDifficulty(ArRegionalSession *session, ArRegionalDifficultySnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.difficulty, &session->effective.difficulty,
                              sizeof(session->requested.difficulty)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalDifficultySnapshot next;
  if (!ArRegionalDifficulty_Resolve(&session->requested.difficulty, &next)) return false;
  session->effective.difficulty = session->requested.difficulty;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginDifficulty(ArRegionalSession *session,
                                       ArRegionalDifficultySnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateDifficulty(session, snapshot);
}

static bool ActivateHazards(ArRegionalSession *session, uint8_t *snapshot) {
  if (!snapshot) return false;
  const bool changed = session->requested.hazards != session->effective.hazards;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalHazards_Resolve(session->requested.hazards, &next)) return false;
  session->effective.hazards = session->requested.hazards;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginHazards(ArRegionalSession *session, uint8_t *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateHazards(session, snapshot);
}

static bool ActivateTerrain(ArRegionalSession *session, uint8_t *snapshot) {
  if (!snapshot) return false;
  const bool changed = session->requested.terrain != session->effective.terrain;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalTerrain_Resolve(session->requested.terrain, &next)) return false;
  session->effective.terrain = session->requested.terrain;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginTerrain(ArRegionalSession *session, uint8_t *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateTerrain(session, snapshot);
}

static bool ActivateMosaic(ArRegionalSession *session, uint8_t *snapshot) {
  if (!snapshot) return false;
  const bool changed = session->requested.mosaic != session->effective.mosaic;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalMosaic_Resolve(session->requested.mosaic, &next)) return false;
  session->effective.mosaic = session->requested.mosaic;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginMosaic(ArRegionalSession *session, uint8_t *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateMosaic(session, snapshot);
}

static bool ActivatePoses(ArRegionalSession *session, uint8_t *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.poses, &session->effective.poses,
                              sizeof(session->requested.poses)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalPoses_Resolve(&session->requested.poses, &next)) return false;
  session->effective.poses = session->requested.poses;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginPoses(ArRegionalSession *session, uint8_t *snapshot) {
  return ArRegionalSession_Valid(session) && ActivatePoses(session, snapshot);
}

static bool ActivatePlacements(ArRegionalSession *session, ArRegionalPlacementPolicy *snapshot) {
  if (!snapshot) return false;
  const bool changed =
      session->requested.placements.enemies != session->effective.placements.enemies ||
      session->requested.placements.pickups != session->effective.placements.pickups;
  if (changed && session->revision == UINT32_MAX) return false;
  session->effective.placements = session->requested.placements;
  if (changed) ++session->revision;
  *snapshot = session->effective.placements;
  return true;
}

bool ArRegionalSession_BeginPlacements(ArRegionalSession *session,
                                       ArRegionalPlacementPolicy *snapshot) {
  return ArRegionalSession_Valid(session) && ActivatePlacements(session, snapshot);
}

static bool ActivateScoreLives(ArRegionalSession *session, bool *enabled) {
  if (!enabled) return false;
  const bool changed = session->requested.score_lives != session->effective.score_lives;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalScoreLives_Resolve(session->requested.score_lives, &next)) return false;
  session->effective.score_lives = session->requested.score_lives;
  if (changed) ++session->revision;
  *enabled = next;
  return true;
}

bool ArRegionalSession_BeginScoreLives(ArRegionalSession *session, bool *enabled) {
  return ArRegionalSession_Valid(session) && ActivateScoreLives(session, enabled);
}

static bool ActivateCollision(ArRegionalSession *session, ArRegionalCollisionSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.collision, &session->effective.collision,
                              sizeof(session->requested.collision)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalCollision_Resolve(&session->requested.collision, &next)) return false;
  session->effective.collision = session->requested.collision;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginCollision(ArRegionalSession *session,
                                      ArRegionalCollisionSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateCollision(session, snapshot);
}

static bool ActivatePlatformSkull(ArRegionalSession *session,
                                  ArRegionalPlatformSkullSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed =
      memcmp(&session->requested.platform_skull, &session->effective.platform_skull,
             sizeof(session->requested.platform_skull)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalPlatformSkull_Resolve(&session->requested.platform_skull, &next)) return false;
  session->effective.platform_skull = session->requested.platform_skull;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginPlatformSkull(ArRegionalSession *session,
                                          ArRegionalPlatformSkullSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivatePlatformSkull(session, snapshot);
}

static bool ActivateActorStats(ArRegionalSession *session, ArRegionalActorStatsSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.actor_stats, &session->effective.actor_stats,
                              sizeof(session->requested.actor_stats)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalActorStatsSnapshot next;
  if (!ArRegionalActorStats_Resolve(&session->requested.actor_stats, &next)) return false;
  session->effective.actor_stats = session->requested.actor_stats;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginActorStats(ArRegionalSession *session,
                                       ArRegionalActorStatsSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateActorStats(session, snapshot);
}

static bool ActivateCastHold(ArRegionalSession *session, ArRegionalCastHoldSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.cast_hold, &session->effective.cast_hold,
                              sizeof(session->requested.cast_hold)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalCastHold_Resolve(&session->requested.cast_hold, &next)) return false;
  session->effective.cast_hold = session->requested.cast_hold;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginCastHold(ArRegionalSession *session,
                                     ArRegionalCastHoldSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateCastHold(session, snapshot);
}

static bool ActivateFire(ArRegionalSession *session, ArRegionalFireSnapshot *snapshot) {
  if (!snapshot) return false;
  const bool changed = memcmp(&session->requested.fire_enemy, &session->effective.fire_enemy,
                              sizeof(session->requested.fire_enemy)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalFire_Resolve(&session->requested.fire_enemy, &next)) return false;
  session->effective.fire_enemy = session->requested.fire_enemy;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_BeginFire(ArRegionalSession *session, ArRegionalFireSnapshot *snapshot) {
  return ArRegionalSession_Valid(session) && ActivateFire(session, snapshot);
}

static bool ActivateArtworkGroup(ArRegionalSession *session, uint8_t scope, uint8_t *mask) {
  if (!mask) return false;
  ArRegionalArtworkPolicy candidate = session->effective.artwork;
  for (unsigned i = 0; i < kArRegionalArtwork_Count; ++i)
    if (scope & (1u << i)) candidate.source[i] = session->requested.artwork.source[i];
  const bool changed = memcmp(&candidate, &session->effective.artwork, sizeof(candidate)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  uint8_t next;
  if (!ArRegionalArtwork_Resolve(&candidate, &next)) return false;
  session->effective.artwork = candidate;
  if (changed) ++session->revision;
  *mask = next & scope;
  return true;
}
bool ArRegionalSession_BeginArtwork(ArRegionalSession *session, uint8_t *mask) {
  return ArRegionalSession_Valid(session) &&
         ActivateArtworkGroup(session, kArRegionalArtwork_ActionMask, mask);
}
bool ArRegionalSession_BeginTownArtwork(ArRegionalSession *session, uint8_t *mask) {
  return ArRegionalSession_Valid(session) &&
         ActivateArtworkGroup(session, kArRegionalArtwork_TownMask, mask);
}
bool ArRegionalSession_BeginTitleArtwork(ArRegionalSession *session, uint8_t *mask) {
  return ArRegionalSession_Valid(session) &&
         ActivateArtworkGroup(session, kArRegionalArtwork_TitleMask, mask);
}
bool ArRegionalSession_BeginActionRoom(ArRegionalSession *session, uint8_t profile,
                                       uint16_t native_bcd, uint16_t *out_bcd,
                                       ArRegionalActionRoomSnapshot *snapshot_out) {
  if (!out_bcd || !snapshot_out || !ArRegionalSession_Valid(session)) return false;
  uint16_t resolved;
  ArRegionalTimerPolicy snapshot;
  ArRegionalActionRoomSnapshot next = {0};
  ArRegionalSession candidate = *session;
  if (!ArRegionalTimers_Resolve(&session->requested.timers, profile, native_bcd, &resolved) ||
      !ActivateTimers(&candidate, &snapshot) || !ActivateActionMotion(&candidate, &next.motion) ||
      !ActivateEmitters(&candidate, &next.emitters) ||
      !ActivateVolley(&candidate, &next.statue_volley) ||
      !ActivateBosses(&candidate, &next.bosses) ||
      !ActivateDifficulty(&candidate, &next.difficulty) ||
      !ActivateHazards(&candidate, &next.hazards) || !ActivateTerrain(&candidate, &next.terrain) ||
      !ActivateMosaic(&candidate, &next.mosaic) || !ActivatePoses(&candidate, &next.poses) ||
      !ActivateArtworkGroup(&candidate, kArRegionalArtwork_ActionMask, &next.artwork) ||
      !ActivatePlacements(&candidate, &next.placements) ||
      !ActivateScoreLives(&candidate, &next.score_lives) ||
      !ActivateCollision(&candidate, &next.collision) ||
      !ActivatePlatformSkull(&candidate, &next.platform_skull) ||
      !ActivateActorStats(&candidate, &next.actor_stats) ||
      !ActivateCastHold(&candidate, &next.cast_hold) || !ActivateFire(&candidate, &next.fire_enemy))
    return false;
  next.placement_difficulty = candidate.effective.difficulty.level;
  *session = candidate;
  *snapshot_out = next;
  *out_bcd = resolved;
  return true;
}
