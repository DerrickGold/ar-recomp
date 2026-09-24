#include "regional/session/regional_session_internal.h"

#include <string.h>

/* Live session validation and non-room transactions. Action-room activation
 * is in regional_session_action.c; stable wire records and checkpoint I/O
 * are in regional_session_codec.c. No menu order or ROM addresses live here. */

bool ArRegionalSession_Valid(const ArRegionalSession *session) {
  if (!session || !session->revision || !RandomizerConfig_Valid(&session->randomizer) || !ArRegionalLairHistory_Valid(&session->lairs) ||
      !ArRegionalLairReloads_Valid(&session->reloads) || !ArRegionalSimActors_Valid(&session->sim_actors)) return false;
  bool has_id = false;
  uint8_t poses;
  if(!ArRegionalPoses_Resolve(&session->requested.poses,&poses) ||
      !ArRegionalPoses_Resolve(&session->effective.poses,&poses))return false;
  if(!ArRegionalSequences_Resolve(&session->requested.sequences,&poses) ||
      !ArRegionalSequences_Resolve(&session->effective.sequences,&poses))return false;
  if(!ArRegionalActorArtwork_Resolve(&session->requested.actor_artwork,&poses) ||
      !ArRegionalActorArtwork_Resolve(&session->effective.actor_artwork,&poses))return false;
  for (unsigned i = 0; i < sizeof(session->campaign); ++i) has_id |= session->campaign[i] != 0;
  ArRegionalCostSnapshot unused;
  ArRegionalDevelopmentSnapshot unused_development;
  ArRegionalRecoverySnapshot unused_recovery;
  ArRegionalQuakeSnapshot unused_quake;
  ArRegionalScoreSnapshot unused_score;
  ArRegionalSourcesSnapshot unused_sources;
  ArRegionalStorySnapshot unused_story;
  ArRegionalTownStatusSnapshot unused_status;
  bool unused_level;
  uint16_t unused_combat;
  uint16_t unused_ai;
  uint8_t unused_emitter;
  uint64_t unused_boss;
  const ArRegionalLairAccounting requested=ArRegionalRules_LairAccounting(&session->requested);
  const ArRegionalLairAccounting effective=ArRegionalRules_LairAccounting(&session->effective);
  unsigned pending_projection, active_projection;
  if (!ArRegionalLairAccounting_Projection(&requested,&pending_projection) ||
      !ArRegionalLairAccounting_Projection(&effective,&active_projection)) return false;
  uint8_t unused_collision;
  ArRegionalActorStatsSnapshot unused_stats;
  ArRegionalDifficultySnapshot unused_difficulty;
  bool unused_lives;
  uint8_t unused_mode;
  ArRegionalActionStartSnapshot unused_start;
  return has_id && ArRegionalArtwork_Resolve(&session->requested.artwork,&unused_mode) &&
      ArRegionalArtwork_Resolve(&session->effective.artwork,&unused_mode) &&
      ArRegionalMosaic_Resolve(session->requested.mosaic,&unused_mode) &&
      ArRegionalMosaic_Resolve(session->effective.mosaic,&unused_mode) && ArRegionalPlacements_Valid(&session->requested.placements) &&
      ArRegionalPlacements_Valid(&session->effective.placements) &&
      ArRegionalMusic_Resolve(session->requested.music,&unused_mode) &&
      ArRegionalMusic_Resolve(session->effective.music,&unused_mode) &&
      ArRegionalTerrain_Resolve(session->requested.terrain,&unused_mode) &&
      ArRegionalTerrain_Resolve(session->effective.terrain,&unused_mode) &&
      ArRegionalHazards_Resolve(session->requested.hazards,&unused_mode) &&
      ArRegionalHazards_Resolve(session->effective.hazards,&unused_mode) &&
      ArRegionalMode_Resolve(&session->requested.mode_entry,&unused_mode) &&
      ArRegionalMode_Resolve(&session->effective.mode_entry,&unused_mode) &&
      ArRegionalActionStart_Resolve(&session->requested.action_start,&unused_start) &&
      ArRegionalInventory_Resolve(session->requested.spell_inventory,&unused_lives) &&
      ArRegionalInventory_Resolve(session->effective.spell_inventory,&unused_lives) &&
      ArRegionalActionStart_Resolve(&session->effective.action_start,&unused_start) &&
      ArRegionalScoreLives_Resolve(session->requested.score_lives,&unused_lives) &&
      ArRegionalScoreLives_Resolve(session->effective.score_lives,&unused_lives) &&
      ArRegionalDifficulty_Resolve(&session->requested.difficulty,&unused_difficulty) &&
      ArRegionalDifficulty_Resolve(&session->effective.difficulty,&unused_difficulty) &&
      ArRegionalFire_Resolve(&session->requested.fire_enemy,&unused_collision) &&
      ArRegionalFire_Resolve(&session->effective.fire_enemy,&unused_collision) &&
      ArRegionalCastHold_Resolve(&session->requested.cast_hold,&unused_collision) &&
      ArRegionalCastHold_Resolve(&session->effective.cast_hold,&unused_collision) &&
      ArRegionalActorStats_Resolve(&session->requested.actor_stats,&unused_stats) &&
      ArRegionalActorStats_Resolve(&session->effective.actor_stats,&unused_stats) &&
      ArRegionalPlatformSkull_Resolve(&session->requested.platform_skull,&unused_collision) &&
      ArRegionalPlatformSkull_Resolve(&session->effective.platform_skull,&unused_collision) &&
      ArRegionalCollision_Resolve(&session->requested.collision,&unused_collision) &&
      ArRegionalCollision_Resolve(&session->effective.collision,&unused_collision) &&
      ArRegionalBoss_Resolve(&session->requested.bosses,&unused_boss) &&
      ArRegionalBoss_Resolve(&session->effective.bosses,&unused_boss) &&
      ArRegionalVolley_Resolve(session->requested.statue_volley,&unused_level) &&
      ArRegionalVolley_Resolve(session->effective.statue_volley,&unused_level) &&
      ArRegionalEmitter_Resolve(&session->requested.emitters,&unused_emitter) &&
      ArRegionalEmitter_Resolve(&session->effective.emitters,&unused_emitter) &&
      ArRegionalActionMotion_Resolve(&session->requested.action_motion,&unused_ai) &&
      ArRegionalActionMotion_Resolve(&session->effective.action_motion,&unused_ai) &&
      ArRegionalArrival_Resolve(session->requested.arrival,&unused_level) &&
      ArRegionalArrival_Resolve(session->effective.arrival,&unused_level) &&
      ArRegionalRules_PopulationCompatible(&session->requested) &&
      ArRegionalRules_PopulationCompatible(&session->effective) &&
      !memcmp(&session->requested.support,&session->effective.support,sizeof(session->requested.support)) &&
      ArRegionalCosts_Resolve(&session->requested.costs, &unused) &&
      ArRegionalConstruction_Resolve(session->requested.construction,&unused_level) &&
      ArRegionalConstruction_Resolve(session->effective.construction,&unused_level) &&
      ArRegionalSimAi_Resolve(&session->requested.sim_ai,&unused_ai) &&
      ArRegionalSimAi_Resolve(&session->effective.sim_ai,&unused_ai) &&
      ArRegionalSimCombat_Resolve(&session->requested.sim_combat,&unused_combat) &&
      ArRegionalSimCombat_Resolve(&session->effective.sim_combat,&unused_combat) &&
      ArRegionalLevelGoals_Resolve(session->requested.level_goals,&unused_level) &&
      ArRegionalLevelGoals_Resolve(session->effective.level_goals,&unused_level) &&
      ArRegionalTownStatus_Resolve(&session->requested.town_status,&unused_status) &&
      ArRegionalTownStatus_Resolve(&session->effective.town_status,&unused_status) &&
      (unsigned)session->requested.lair_reloads<kArRegionalSource_Count &&
      (unsigned)session->effective.lair_reloads<kArRegionalSource_Count &&
      ((session->requested.lair_reloads!=kArRegionalSource_Japan &&
        session->effective.lair_reloads!=kArRegionalSource_Japan) || session->reloads.initialized_towns==0x3f) &&
      ArRegionalStory_Resolve(&session->requested.story,&unused_story) &&
      ArRegionalStory_Resolve(&session->effective.story,&unused_story) &&
      ArRegionalSources_Resolve(&session->requested.sources,&unused_sources) &&
      ArRegionalSources_Resolve(&session->effective.sources,&unused_sources) &&
      ArRegionalScore_Resolve(&session->requested.score_feedback,&unused_score) &&
      ArRegionalScore_Resolve(&session->effective.score_feedback,&unused_score) &&
      ArRegionalCosts_Resolve(&session->effective.costs, &unused) &&
      ArRegionalTimers_Valid(&session->requested.timers) &&
      ArRegionalTimers_Valid(&session->effective.timers) &&
      (unsigned)session->requested.retry_score < kArRegionalSource_Count &&
      (unsigned)session->effective.retry_score < kArRegionalSource_Count &&
      (unsigned)session->requested.town_wait < kArRegionalSource_Count &&
      (unsigned)session->effective.town_wait < kArRegionalSource_Count &&
      (unsigned)session->requested.fishing < kArRegionalSource_Count &&
      (unsigned)session->effective.fishing < kArRegionalSource_Count &&
      ArRegionalDevelopment_Resolve(&session->requested.development,&unused_development) &&
      ArRegionalDevelopment_Resolve(&session->effective.development,&unused_development) &&
      ArRegionalRecovery_Resolve(&session->requested.recovery, &unused_recovery) &&
      ArRegionalRecovery_Resolve(&session->effective.recovery, &unused_recovery) &&
      ArRegionalQuake_Resolve(&session->requested.quake, &unused_quake) &&
      ArRegionalQuake_Resolve(&session->effective.quake, &unused_quake) &&
      (unsigned)session->requested.score_page < kArRegionalSource_Count &&
      (unsigned)session->effective.score_page < kArRegionalSource_Count &&
      (unsigned)session->requested.lives_display < kArRegionalSource_Count &&
      (unsigned)session->effective.lives_display < kArRegionalSource_Count &&
      (unsigned)session->requested.skull_wait < kArRegionalSource_Count &&
      (unsigned)session->effective.skull_wait < kArRegionalSource_Count &&
      (unsigned)session->requested.menu_return < kArRegionalSource_Count &&
      (unsigned)session->effective.menu_return < kArRegionalSource_Count &&
      (unsigned)session->requested.speed_range < kArRegionalSource_Count &&
      (unsigned)session->effective.speed_range < kArRegionalSource_Count &&
      (unsigned)session->requested.magic_gesture < kArRegionalSource_Count &&
      (unsigned)session->effective.magic_gesture < kArRegionalSource_Count &&
      ((!pending_projection && !active_projection &&
        session->requested.score_feedback.source[kArRegionalScore_Phase]!=kArRegionalSource_Japan &&
        session->effective.score_feedback.source[kArRegionalScore_Phase]!=kArRegionalSource_Japan) ||
       session->lairs.initialized_towns == 0x3f);
}
bool ArRegionalSession_RequestRules(ArRegionalSession *session,uint32_t revision,
    const ArRegionalRules *requested) {
  if(!requested || !ArRegionalSession_Valid(session) || revision!=session->revision)return false;
  if(!memcmp(requested,&session->requested,sizeof(*requested)))return true;
  if(session->revision==UINT32_MAX)return false;
  ArRegionalSupportSnapshot current_support,next_support;
  if(!ArRegionalSupport_Resolve(&session->effective.support,&current_support) ||
      !ArRegionalSupport_Resolve(&requested->support,&next_support) ||
      memcmp(&current_support,&next_support,sizeof(current_support)))return false;
  const bool accounting_changed=!ArRegionalRules_SameAccounting(&session->requested,requested) ||
      memcmp(&session->requested.score_feedback,&requested->score_feedback,sizeof(requested->score_feedback));
  if(accounting_changed && (session->lairs.initialized_towns!=0x3f || session->lairs.diverged_towns))return false;
  if(session->requested.lair_reloads!=requested->lair_reloads &&
      (session->reloads.initialized_towns!=0x3f || session->reloads.diverged_towns))return false;
  ArRegionalSession candidate=*session;
  candidate.requested=*requested;
  candidate.effective.support=requested->support;
  ++candidate.revision;
  if(!ArRegionalSession_Valid(&candidate))return false;
  *session=candidate;return true;
}
bool ArRegionalSession_RequestTerrain(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalTerrain_Resolve(source,&unused))return false;
  if(session->requested.terrain==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.terrain=source;++session->revision;return true;
}
bool ArRegionalSession_RequestPlacements(ArRegionalSession *session,uint32_t revision,
    const ArRegionalPlacementPolicy *policy) {
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalPlacements_Valid(policy))return false;
  if(session->requested.placements.enemies==policy->enemies &&
      session->requested.placements.pickups==policy->pickups)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.placements=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestArtwork(ArRegionalSession *session,uint32_t revision,
                                     ArRegionalArtworkRule rule,ArRegionalSource source) {
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || (unsigned)rule>=kArRegionalArtwork_Count ||
      (unsigned)source>=kArRegionalSource_Count)return false;
  if(session->requested.artwork.source[rule]==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.artwork.source[rule]=source;++session->revision;return true;
}
bool ArRegionalSession_RequestActorArtwork(ArRegionalSession *session,uint32_t revision,const ArRegionalActorArtworkPolicy *policy) {
  uint8_t mask;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalActorArtwork_Resolve(policy,&mask))return false;
  if(!memcmp(policy,&session->requested.actor_artwork,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.actor_artwork=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginActorArtwork(ArRegionalSession *session,unsigned area,bool *enabled) {
  if(!enabled || area>=kArRegionalActorArtwork_Count || !ArRegionalSession_Valid(session))return false;
  const ArRegionalSource source=session->requested.actor_artwork.source[area];
  if(source!=session->effective.actor_artwork.source[area]) {
    if(session->revision==UINT32_MAX)return false;
    session->effective.actor_artwork.source[area]=source;++session->revision;
  }
  *enabled=source==kArRegionalSource_Japan;return true;
}
bool ArRegionalSession_RequestSequences(ArRegionalSession *session,uint32_t revision,const ArRegionalSequencePolicy *policy) {
  uint8_t mask;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalSequences_Resolve(policy,&mask))return false;
  if(!memcmp(policy,&session->requested.sequences,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.sequences=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginSequence(ArRegionalSession *session,unsigned rule,bool *enabled) {
  if(!enabled || rule>=kArRegionalSequence_Count || !ArRegionalSession_Valid(session))return false;
  const ArRegionalSource source=session->requested.sequences.source[rule];
  if(source!=session->effective.sequences.source[rule]) {
    if(session->revision==UINT32_MAX)return false;
    session->effective.sequences.source[rule]=source;++session->revision;
  }
  *enabled=source==kArRegionalSource_Japan;return true;
}
bool ArRegionalSession_RequestPoses(ArRegionalSession *session,uint32_t revision,const ArRegionalPosePolicy *policy) {
  uint8_t snapshot;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalPoses_Resolve(policy,&snapshot))return false;
  if(!memcmp(policy,&session->requested.poses,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.poses=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestMosaic(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalMosaic_Resolve(source,&unused))return false;
  if(session->requested.mosaic==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.mosaic=source;++session->revision;return true;
}
bool ArRegionalSession_RequestMusic(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalMusic_Resolve(source,&unused))return false;
  if(session->requested.music==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.music=source;++session->revision;return true;
}
bool ArRegionalSession_BeginMusic(ArRegionalSession *session,uint8_t *snapshot) {
  if(!snapshot || !ArRegionalSession_Valid(session))return false;
  const bool changed=session->requested.music!=session->effective.music;
  if(changed && session->revision==UINT32_MAX)return false;
  uint8_t next;if(!ArRegionalMusic_Resolve(session->requested.music,&next))return false;
  session->effective.music=session->requested.music;
  if(changed)++session->revision;
  *snapshot=next;return true;
}
bool ArRegionalSession_RequestLairReloads(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || (unsigned)source>=kArRegionalSource_Count ||
      session->reloads.initialized_towns!=0x3f || session->reloads.diverged_towns) return false;
  if (session->requested.lair_reloads==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.lair_reloads=source; ++session->revision; return true;
}
bool ArRegionalSession_BeginLairReloads(ArRegionalSession *session) {
  if (!ArRegionalSession_Valid(session) || session->reloads.initialized_towns!=0x3f || session->reloads.diverged_towns) return false;
  if (session->effective.lair_reloads==session->requested.lair_reloads) return true;
  if (session->revision==UINT32_MAX) return false;
  session->effective.lair_reloads=session->requested.lair_reloads; ++session->revision; return true;
}

bool ArRegionalSession_RequestLairSeeds(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision != session->revision ||
      (unsigned)source >= kArRegionalSource_Count || session->lairs.initialized_towns != 0x3f ||
      session->lairs.diverged_towns) return false;
  if (session->requested.lair_seeds == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.lair_seeds = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_RequestHouseCredit(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision != session->revision ||
      (unsigned)source >= kArRegionalSource_Count || session->lairs.initialized_towns != 0x3f ||
      session->lairs.diverged_towns) return false;
  if (session->requested.house_credit == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.house_credit = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_RequestScoreFeedback(ArRegionalSession *session, uint32_t revision,
                                          const ArRegionalScorePolicy *policy) {
  ArRegionalScoreSnapshot unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision ||
      !ArRegionalScore_Resolve(policy,&unused) || session->lairs.initialized_towns!=0x3f ||
      session->lairs.diverged_towns) return false;
  if (!memcmp(&session->requested.score_feedback,policy,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.score_feedback=*policy;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginLairAccounting(ArRegionalSession *session, ArRegionalLairAccounting *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session) || session->lairs.initialized_towns != 0x3f ||
      session->lairs.diverged_towns) return false;
  const bool changed = !ArRegionalRules_SameAccounting(&session->requested,&session->effective);
  if (changed && session->revision == UINT32_MAX) return false;
  session->effective.lair_seeds = session->requested.lair_seeds;
  session->effective.house_credit = session->requested.house_credit;
  for (unsigned i=0; i<kArRegionalScore_Phase; ++i)
    session->effective.score_feedback.source[i]=session->requested.score_feedback.source[i];
  if (changed) ++session->revision;
  *snapshot = ArRegionalRules_LairAccounting(&session->effective);
  return true;
}

bool ArRegionalSession_BeginScoreCompletion(ArRegionalSession *session,
                                           ArRegionalScoreSnapshot *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session)) return false;
  const ArRegionalSource phase=session->requested.score_feedback.source[kArRegionalScore_Phase];
  const bool changed=phase!=session->effective.score_feedback.source[kArRegionalScore_Phase];
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalScorePolicy policy=session->effective.score_feedback;
  policy.source[kArRegionalScore_Phase]=phase;
  ArRegionalScoreSnapshot next;
  if (!ArRegionalScore_Resolve(&policy,&next)) return false;
  session->effective.score_feedback=policy;
  if (changed) ++session->revision;
  *snapshot=next;
  return true;
}

bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults) {
  if (!session || !campaign || !defaults) return false;
  ArRegionalSession next = {.slot = slot, .revision = 1, .requested.costs = *defaults, .effective.costs = *defaults};
  ArRegionalTimers_Init(&next.requested.timers, kArRegionalSource_US);
  next.effective.timers = next.requested.timers;
  memcpy(next.campaign, campaign, sizeof(next.campaign));
  if (!ArRegionalSession_Valid(&next)) return false;
  *session = next;
  return true;
}

bool ArRegionalSession_RequestTimers(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision) return false;
  ArRegionalTimerPolicy next;
  if (!ArRegionalTimers_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.timers, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.timers = next;
  ++session->revision;
  return true;
}


bool ArRegionalSession_RequestRetryScore(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.retry_score == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.retry_score = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRetryScore(ArRegionalSession *session, bool *clear_score) {
  if (!clear_score || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.retry_score != session->effective.retry_score;
  if (changed && session->revision == UINT32_MAX) return false;
  bool resolved;
  if (!ArRegionalRetry_Resolve(session->requested.retry_score, &resolved)) return false;
  session->effective.retry_score = session->requested.retry_score;
  if (changed) ++session->revision;
  *clear_score = resolved;
  return true;
}

bool ArRegionalSession_RequestTownWait(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.town_wait == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.town_wait = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginTownWait(ArRegionalSession *session, uint16_t *updates) {
  if (!updates || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.town_wait != session->effective.town_wait;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t resolved;
  if (!ArRegionalTownWait_Resolve(session->requested.town_wait, &resolved)) return false;
  session->effective.town_wait = session->requested.town_wait;
  if (changed) ++session->revision;
  *updates = resolved;
  return true;
}

bool ArRegionalSession_RequestFishing(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.fishing == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.fishing = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginFishing(ArRegionalSession *session, uint16_t *updates,
                                   bool *reconcile) {
  if (!updates || !reconcile || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.fishing != session->effective.fishing;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t pending, active;
  if (!ArRegionalFishing_Resolve(session->requested.fishing, &pending) ||
      !ArRegionalFishing_Resolve(session->effective.fishing, &active)) return false;
  session->effective.fishing = session->requested.fishing;
  if (changed) ++session->revision;
  *updates = pending;
  *reconcile = pending != active;
  return true;
}

bool ArRegionalSession_RequestDevelopment(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if(!ArRegionalSession_Valid(session) || session->revision!=revision)return false;
  ArRegionalDevelopmentPolicy next;
  if(!ArRegionalDevelopment_Init(&next,source))return false;
  if(!memcmp(&next,&session->requested.development,sizeof(next)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.development=next;++session->revision;return true;
}
bool ArRegionalSession_BeginDevelopment(ArRegionalSession *session,
                                       ArRegionalDevelopmentSnapshot *snapshot) {
  if(!snapshot || !ArRegionalSession_Valid(session))return false;
  const bool changed=memcmp(&session->requested.development,&session->effective.development,
                            sizeof(session->requested.development))!=0;
  if(changed && session->revision==UINT32_MAX)return false;
  ArRegionalDevelopmentSnapshot next;
  if(!ArRegionalDevelopment_Resolve(&session->requested.development,&next))return false;
  session->effective.development=session->requested.development;
  if(changed)++session->revision;
  *snapshot=next;return true;
}

bool ArRegionalSession_RequestRecovery(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  ArRegionalRecoveryPolicy next;
  if (!ArRegionalRecovery_Init(&next, source)) return false;
  return ArRegionalSession_RequestRecoveryPolicy(session, revision, &next);
}

bool ArRegionalSession_RequestRecoveryPolicy(ArRegionalSession *session, uint32_t revision,
                                            const ArRegionalRecoveryPolicy *policy) {
  ArRegionalRecoverySnapshot resolved;
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      !ArRegionalRecovery_Resolve(policy, &resolved)) return false;
  if (!memcmp(policy, &session->requested.recovery, sizeof(*policy))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.recovery = *policy;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRecovery(ArRegionalSession *session,
    ArRegionalRecoverySnapshot *snapshot, unsigned *changed) {
  if (!snapshot || !changed || !ArRegionalSession_Valid(session)) return false;
  const bool edit = memcmp(&session->requested.recovery, &session->effective.recovery,
                           sizeof(session->requested.recovery)) != 0;
  if (edit && session->revision == UINT32_MAX) return false;
  ArRegionalRecoverySnapshot next;
  if (!ArRegionalRecovery_Resolve(&session->requested.recovery, &next)) return false;
  unsigned mask = 0;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) {
    const ArRegionalRecoveryDescriptor *rule = ArRegionalRecovery_Descriptor((ArRegionalRecoveryRule)i);
    if (rule->value[session->requested.recovery.source[i]] !=
        rule->value[session->effective.recovery.source[i]]) mask |= 1u << i;
  }
  session->effective.recovery = session->requested.recovery;
  if (edit) ++session->revision;
  *snapshot = next;
  *changed = mask;
  return true;
}

bool ArRegionalSession_RequestQuake(ArRegionalSession *session, uint32_t revision,
                                    ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision) return false;
  ArRegionalQuakePolicy next;
  if (!ArRegionalQuake_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.quake, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.quake = next;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginQuake(ArRegionalSession *session, ArRegionalQuakeSnapshot *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session)) return false;
  const bool changed = memcmp(&session->requested.quake, &session->effective.quake,
                              sizeof(session->requested.quake)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalQuakeSnapshot next;
  if (!ArRegionalQuake_Resolve(&session->requested.quake, &next)) return false;
  session->effective.quake = session->requested.quake;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_RequestActionMotion(ArRegionalSession *session,uint32_t revision,const ArRegionalActionMotionPolicy *policy) {
  uint16_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalActionMotion_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.action_motion,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.action_motion=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestVolley(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  bool unused;
  if (!ArRegionalSession_Valid(session) || revision != session->revision || !ArRegionalVolley_Resolve(source, &unused)) return false;
  if (source == session->requested.statue_volley) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.statue_volley = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_RequestFire(ArRegionalSession *session,uint32_t revision,const ArRegionalFirePolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalFire_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.fire_enemy,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.fire_enemy=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestCastHold(ArRegionalSession *session,uint32_t revision,const ArRegionalCastHoldPolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalCastHold_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.cast_hold,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.cast_hold=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestActorStats(ArRegionalSession *session,uint32_t revision,const ArRegionalActorStatsPolicy *policy) {
  ArRegionalActorStatsSnapshot unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalActorStats_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.actor_stats,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.actor_stats=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestPlatformSkull(ArRegionalSession *session,uint32_t revision,const ArRegionalPlatformSkullPolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalPlatformSkull_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.platform_skull,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.platform_skull=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestCollision(ArRegionalSession *session,uint32_t revision,const ArRegionalCollisionPolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalCollision_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.collision,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.collision=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestModeEntry(ArRegionalSession *session,uint32_t revision,const ArRegionalModePolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalMode_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.mode_entry,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.mode_entry=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginModeEntry(ArRegionalSession *session,uint8_t *snapshot) {
  if(!snapshot || !ArRegionalSession_Valid(session))return false;
  const bool changed=memcmp(&session->requested.mode_entry,&session->effective.mode_entry,sizeof(session->requested.mode_entry))!=0;
  if(changed && session->revision==UINT32_MAX)return false;
  uint8_t next;if(!ArRegionalMode_Resolve(&session->requested.mode_entry,&next))return false;
  session->effective.mode_entry=session->requested.mode_entry;
  if(changed)++session->revision;
  *snapshot=next;return true;
}
bool ArRegionalSession_RequestHazards(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalHazards_Resolve(source,&unused))return false;
  if(session->requested.hazards==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.hazards=source;++session->revision;return true;
}
bool ArRegionalSession_RequestInventory(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  bool unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalInventory_Resolve(source,&unused))return false;
  if(session->requested.spell_inventory==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.spell_inventory=source;++session->revision;return true;
}
bool ArRegionalSession_BeginInventory(ArRegionalSession *session,bool *enabled) {
  if(!enabled || !ArRegionalSession_Valid(session))return false;
  const bool changed=session->requested.spell_inventory!=session->effective.spell_inventory;
  if(changed && session->revision==UINT32_MAX)return false;
  bool next;if(!ArRegionalInventory_Resolve(session->requested.spell_inventory,&next))return false;
  session->effective.spell_inventory=session->requested.spell_inventory;
  if(changed)++session->revision;
  *enabled=next;return true;
}
bool ArRegionalSession_RequestActionStart(ArRegionalSession *session,uint32_t revision,const ArRegionalActionStartPolicy *policy) {
  ArRegionalActionStartSnapshot unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalActionStart_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.action_start,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.action_start=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginActionStart(ArRegionalSession *session,ArRegionalActionStartSnapshot *snapshot) {
  if(!snapshot || !ArRegionalSession_Valid(session))return false;
  const bool changed=memcmp(&session->requested.action_start,&session->effective.action_start,sizeof(session->requested.action_start))!=0;
  if(changed && session->revision==UINT32_MAX)return false;
  ArRegionalActionStartSnapshot next;
  if(!ArRegionalActionStart_Resolve(&session->requested.action_start,&next))return false;
  session->effective.action_start=session->requested.action_start;
  if(changed)++session->revision;
  *snapshot=next;return true;
}
bool ArRegionalSession_RequestScoreLives(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  bool unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalScoreLives_Resolve(source,&unused))return false;
  if(session->requested.score_lives==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.score_lives=source;++session->revision;return true;
}
bool ArRegionalSession_RequestDifficulty(ArRegionalSession *session, uint32_t revision,
    const ArRegionalDifficultyPolicy *policy) {
  ArRegionalDifficultySnapshot unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalDifficulty_Resolve(policy,&unused)) return false;
  if (!memcmp(policy,&session->requested.difficulty,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.difficulty=*policy;
  ++session->revision;
  return true;
}
bool ArRegionalSession_RequestBosses(ArRegionalSession *session,uint32_t revision,const ArRegionalBossPolicy *policy) {
  uint64_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalBoss_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.bosses,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.bosses=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestEmitters(ArRegionalSession *session,uint32_t revision,const ArRegionalEmitterPolicy *policy) {
  uint8_t unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalEmitter_Resolve(policy,&unused))return false;
  if(!memcmp(policy,&session->requested.emitters,sizeof(*policy)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.emitters=*policy;++session->revision;return true;
}

bool ArRegionalSession_RequestArrival(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  bool unused;
  if(!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalArrival_Resolve(source,&unused))return false;
  if(session->requested.arrival==source)return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.arrival=source;++session->revision;return true;
}
bool ArRegionalSession_BeginArrival(ArRegionalSession *session,bool continuing,bool *japanese) {
  if(!japanese || !ArRegionalSession_Valid(session))return false;
  if(!session->arrival_locked) {
    if(session->revision==UINT32_MAX)return false;
    if(!continuing)session->effective.arrival=session->requested.arrival;
    session->arrival_locked=true;++session->revision;
  }
  return ArRegionalArrival_Resolve(session->effective.arrival,japanese);
}

bool ArRegionalSession_SetPopulationProfile(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision!=revision || (unsigned)source>=kArRegionalSource_Count) return false;
  ArRegionalRules requested=session->requested, effective=session->effective;
  ArRegionalSupport_Init(&requested.support,source);
  effective.support=requested.support;
  requested.level_goals=effective.level_goals=source;
  requested.story.source[kArRegionalStory_FillmoreHint]=effective.story.source[kArRegionalStory_FillmoreHint]=source;
  requested.story.source[kArRegionalStory_KasandoraTablet]=effective.story.source[kArRegionalStory_KasandoraTablet]=source;
  if (!memcmp(&requested,&session->requested,sizeof(requested)) &&
      !memcmp(&effective,&session->effective,sizeof(effective))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested=requested;session->effective=effective;++session->revision;return true;
}

bool ArRegionalSession_RequestLevelGoals(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  bool unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalLevelGoals_Resolve(source,&unused)) return false;
  ArRegionalRules candidate=session->requested;candidate.level_goals=source;
  if (!ArRegionalRules_PopulationCompatible(&candidate)) return false;
  if (session->requested.level_goals==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.level_goals=source;++session->revision;return true;
}
bool ArRegionalSession_BeginLevelGoals(ArRegionalSession *session,bool *japanese) {
  if (!japanese || !ArRegionalSession_Valid(session)) return false;
  const bool changed=session->effective.level_goals!=session->requested.level_goals;
  if (changed && session->revision==UINT32_MAX) return false;
  bool next;
  if (!ArRegionalLevelGoals_Resolve(session->requested.level_goals,&next)) return false;
  session->effective.level_goals=session->requested.level_goals;
  if (changed) ++session->revision;
  *japanese=next;return true;
}

bool ArRegionalSession_RequestTownStatus(ArRegionalSession *session, uint32_t revision,
                                        const ArRegionalTownStatusPolicy *policy) {
  ArRegionalTownStatusSnapshot unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalTownStatus_Resolve(policy,&unused)) return false;
  if (!memcmp(policy,&session->requested.town_status,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.town_status=*policy; ++session->revision; return true;
}
bool ArRegionalSession_BeginTownStatus(ArRegionalSession *session, ArRegionalTownStatusSnapshot *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session)) return false;
  const bool changed=memcmp(&session->requested.town_status,&session->effective.town_status,sizeof(session->requested.town_status))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalTownStatusSnapshot next;
  if (!ArRegionalTownStatus_Resolve(&session->requested.town_status,&next)) return false;
  session->effective.town_status=session->requested.town_status;
  if (changed) ++session->revision;
  *snapshot=next; return true;
}

bool ArRegionalSession_RequestConstruction(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision != session->revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.construction == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.construction = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginConstruction(ArRegionalSession *session, bool *japanese) {
  if (!japanese || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.construction != session->effective.construction;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalConstruction_Resolve(session->requested.construction, &next)) return false;
  session->effective.construction = session->requested.construction;
  if (changed) ++session->revision;
  *japanese = next;
  return true;
}

bool ArRegionalSession_RequestStory(ArRegionalSession *session, uint32_t revision, const ArRegionalStoryPolicy *policy) {
  ArRegionalStorySnapshot unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalStory_Resolve(policy,&unused)) return false;
  ArRegionalRules candidate=session->requested;candidate.story=*policy;
  if (!ArRegionalRules_PopulationCompatible(&candidate)) return false;
  if (!memcmp(policy,&session->requested.story,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.story=*policy; ++session->revision; return true;
}
bool ArRegionalSession_BeginStory(ArRegionalSession *session, ArRegionalStorySnapshot *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session)) return false;
  const bool changed=memcmp(&session->requested.story,&session->effective.story,sizeof(session->requested.story))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalStorySnapshot next;
  if (!ArRegionalStory_Resolve(&session->requested.story,&next)) return false;
  session->effective.story=session->requested.story;
  if (changed) ++session->revision;
  *snapshot=next; return true;
}

bool ArRegionalSession_RequestSkullWait(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || (unsigned)source>=kArRegionalSource_Count) return false;
  if (session->requested.skull_wait==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.skull_wait=source; ++session->revision;
  return true;
}
bool ArRegionalSession_BeginSkullWait(ArRegionalSession *session, uint16_t *frames) {
  if (!frames || !ArRegionalSession_Valid(session)) return false;
  const bool changed=session->requested.skull_wait!=session->effective.skull_wait;
  if (changed && session->revision==UINT32_MAX) return false;
  uint16_t next;
  if (!ArRegionalSkullWait_Resolve(session->requested.skull_wait,&next)) return false;
  session->effective.skull_wait=session->requested.skull_wait;
  if (changed) ++session->revision;
  *frames=next;
  return true;
}

bool ArRegionalSession_RequestSources(ArRegionalSession *session, uint32_t revision,
                                     const ArRegionalSourcesPolicy *policy) {
  ArRegionalSourcesSnapshot unused;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalSources_Resolve(policy,&unused)) return false;
  if (!memcmp(&session->requested.sources,policy,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sources=*policy;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginSources(ArRegionalSession *session, ArRegionalSourcesSnapshot *snapshot) {
  if (!snapshot || !ArRegionalSession_Valid(session)) return false;
  const bool changed=memcmp(&session->requested.sources,&session->effective.sources,sizeof(session->requested.sources))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalSourcesSnapshot next;
  if (!ArRegionalSources_Resolve(&session->requested.sources,&next)) return false;
  session->effective.sources=session->requested.sources;
  if (changed) ++session->revision;
  *snapshot=next;
  return true;
}

bool ArRegionalSession_RequestScorePage(ArRegionalSession *session, uint32_t revision,
                                       ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.score_page == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.score_page = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_RequestLivesDisplay(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || revision != session->revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.lives_display == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.lives_display = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginLivesDisplay(ArRegionalSession *session, bool *zero_based) {
  if (!zero_based || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.lives_display != session->effective.lives_display;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalLivesDisplay_Resolve(session->requested.lives_display, &next)) return false;
  session->effective.lives_display = session->requested.lives_display;
  if (changed) ++session->revision;
  *zero_based = next;
  return true;
}
bool ArRegionalSession_BeginScorePage(ArRegionalSession *session, bool *enabled) {
  if (!enabled || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.score_page != session->effective.score_page;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalScorePage_Resolve(session->requested.score_page, &next)) return false;
  session->effective.score_page = session->requested.score_page;
  if (changed) ++session->revision;
  *enabled = next;
  return true;
}

bool ArRegionalSession_RequestMenuReturn(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.menu_return == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.menu_return = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginMenuReturn(ArRegionalSession *session, bool *keep_open) {
  if (!keep_open || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.menu_return != session->effective.menu_return;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalMenuReturn_Resolve(session->requested.menu_return, &next)) return false;
  session->effective.menu_return = session->requested.menu_return;
  if (changed) ++session->revision;
  *keep_open = next;
  return true;
}

bool ArRegionalSession_RequestSpeedRange(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.speed_range == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.speed_range = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginSpeedRange(ArRegionalSession *session, uint16_t *maximum) {
  if (!maximum || !ArRegionalSession_Valid(session)) return false;
  const bool changed = session->requested.speed_range != session->effective.speed_range;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t next;
  if (!ArRegionalSpeedRange_Resolve(session->requested.speed_range, &next)) return false;
  session->effective.speed_range = session->requested.speed_range;
  if (changed) ++session->revision;
  *maximum = next;
  return true;
}

bool ArRegionalSession_RequestMagicGesture(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.magic_gesture == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.magic_gesture = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginMagicGesture(ArRegionalSession *session,
                                        bool controls_released, bool *up_attack) {
  if (!up_attack || !ArRegionalSession_Valid(session)) return false;
  const ArRegionalSource next = controls_released ? session->requested.magic_gesture
                                                : session->effective.magic_gesture;
  const bool changed = next != session->effective.magic_gesture;
  if (changed && session->revision == UINT32_MAX) return false;
  bool resolved;
  if (!ArRegionalMagicGesture_Resolve(next, &resolved)) return false;
  session->effective.magic_gesture = next;
  if (changed) ++session->revision;
  *up_attack = resolved;
  return true;
}

bool ArRegionalSession_RequestSimCombat(ArRegionalSession *session,uint32_t revision,
                                      const ArRegionalSimCombatPolicy *policy) {
  uint16_t snapshot;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalSimCombat_Resolve(policy,&snapshot)) return false;
  if (!memcmp(policy,&session->requested.sim_combat,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sim_combat=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestSimAi(ArRegionalSession *session,uint32_t revision,const ArRegionalSimAiPolicy *policy) {
  uint16_t snapshot;
  if (!ArRegionalSession_Valid(session) || revision!=session->revision || !ArRegionalSimAi_Resolve(policy,&snapshot)) return false;
  if (!memcmp(policy,&session->requested.sim_ai,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sim_ai=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginSimActor(ArRegionalSession *session,unsigned town,unsigned slot) {
  ArRegionalSimActorRules snapshot;
  if (!ArRegionalSession_Valid(session) || town>=6 || slot>=4 || session->sim_actors.active_town_tag!=town+1 ||
      !ArRegionalSimCombat_Resolve(&session->requested.sim_combat,&snapshot.combat) ||
      !ArRegionalSimAi_Resolve(&session->requested.sim_ai,&snapshot.ai)) return false;
  const bool changed=memcmp(&session->requested.sim_combat,&session->effective.sim_combat,sizeof(session->requested.sim_combat))!=0 ||
      memcmp(&session->requested.sim_ai,&session->effective.sim_ai,sizeof(session->requested.sim_ai))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  if (!ArRegionalSimActors_Birth(&session->sim_actors,town,slot,snapshot)) return false;
  session->effective.sim_combat=session->requested.sim_combat;
  session->effective.sim_ai=session->requested.sim_ai;
  if (changed) ++session->revision;
  return true;
}
bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalSource source) {
  if (!ArRegionalSession_Valid(session) || session->revision != revision) return false;
  ArRegionalCostPolicy next = session->requested.costs;
  if (!ArRegionalCosts_SetGroup(&next, group, source)) return false;
  if (!memcmp(&next, &session->requested.costs, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.costs = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginCosts(ArRegionalSession *session,
    ArRegionalCostGroup group, ArRegionalCostSnapshot *quote) {
  if (!ArRegionalSession_Valid(session) || !quote || (unsigned)group >= kArRegionalCostGroup_Count) return false;
  ArRegionalCostPolicy next = session->effective.costs;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    if (ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->group == group)
      next.source[i] = session->requested.costs.source[i];
  bool changed = memcmp(&next, &session->effective.costs, sizeof(next)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalCostSnapshot prices;
  if (!ArRegionalCosts_Resolve(&next, &prices)) return false;
  session->effective.costs = next;
  if (changed) ++session->revision;
  *quote = prices;
  return true;
}
