#include "regional_session.h"

#include "byte_order.h"

#include <stdio.h>
#include <string.h>

enum { kHeaderBytes = 36, kPayloadCapacity = kSaveCheckpointPayloadMax,
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
       kRecordCount = kV24RecordCount + kArRegionalSimAi_Count };
_Static_assert(kArRegionalCostRule_Count == 9 && kArRegionalTimerRule_Count == 6,
               "extend the legacy record mapping explicitly when adding family leaves");
_Static_assert(kArRegionalDevelopmentRule_Count == 3,"version5 has three development leaves");
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

static bool Valid(const ArRegionalSession *session) {
  if (!session || !session->revision || !ArRegionalLairHistory_Valid(&session->lairs) ||
      !ArRegionalLairReloads_Valid(&session->reloads) || !ArRegionalSimActors_Valid(&session->sim_actors)) return false;
  bool has_id = false;
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
  const ArRegionalLairAccounting requested=ArRegionalRules_LairAccounting(&session->requested);
  const ArRegionalLairAccounting effective=ArRegionalRules_LairAccounting(&session->effective);
  unsigned pending_projection, active_projection;
  if (!ArRegionalLairAccounting_Projection(&requested,&pending_projection) ||
      !ArRegionalLairAccounting_Projection(&effective,&active_projection)) return false;
  return has_id && ArRegionalCosts_Resolve(&session->requested.costs, &unused) &&
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

bool ArRegionalSession_RequestLairReloads(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  if (!Valid(session) || revision!=session->revision || (unsigned)source>=kArRegionalSource_Count ||
      session->reloads.initialized_towns!=0x3f || session->reloads.diverged_towns) return false;
  if (session->requested.lair_reloads==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.lair_reloads=source; ++session->revision; return true;
}
bool ArRegionalSession_BeginLairReloads(ArRegionalSession *session) {
  if (!Valid(session) || session->reloads.initialized_towns!=0x3f || session->reloads.diverged_towns) return false;
  if (session->effective.lair_reloads==session->requested.lair_reloads) return true;
  if (session->revision==UINT32_MAX) return false;
  session->effective.lair_reloads=session->requested.lair_reloads; ++session->revision; return true;
}

bool ArRegionalSession_RequestLairSeeds(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  if (!Valid(session) || revision != session->revision ||
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
  if (!Valid(session) || revision != session->revision ||
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
  if (!Valid(session) || revision!=session->revision ||
      !ArRegionalScore_Resolve(policy,&unused) || session->lairs.initialized_towns!=0x3f ||
      session->lairs.diverged_towns) return false;
  if (!memcmp(&session->requested.score_feedback,policy,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.score_feedback=*policy;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginLairAccounting(ArRegionalSession *session, ArRegionalLairAccounting *snapshot) {
  if (!snapshot || !Valid(session) || session->lairs.initialized_towns != 0x3f ||
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
  if (!snapshot || !Valid(session)) return false;
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
  if (!Valid(&next)) return false;
  *session = next;
  return true;
}

bool ArRegionalSession_RequestTimers(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalTimerPolicy next;
  if (!ArRegionalTimers_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.timers, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.timers = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginTimers(ArRegionalSession *session,
                                   ArRegionalTimerPolicy *snapshot) {
  if (!snapshot || !Valid(session)) return false;
  const bool changed = memcmp(&session->requested.timers, &session->effective.timers,
                               sizeof(session->requested.timers)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  session->effective.timers = session->requested.timers;
  if (changed) ++session->revision;
  *snapshot = session->effective.timers;
  return true;
}

bool ArRegionalSession_RequestRetryScore(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.retry_score == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.retry_score = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRetryScore(ArRegionalSession *session, bool *clear_score) {
  if (!clear_score || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.town_wait == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.town_wait = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginTownWait(ArRegionalSession *session, uint16_t *updates) {
  if (!updates || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.fishing == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.fishing = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginFishing(ArRegionalSession *session, uint16_t *updates,
                                   bool *reconcile) {
  if (!updates || !reconcile || !Valid(session)) return false;
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
  if(!Valid(session) || session->revision!=revision)return false;
  ArRegionalDevelopmentPolicy next;
  if(!ArRegionalDevelopment_Init(&next,source))return false;
  if(!memcmp(&next,&session->requested.development,sizeof(next)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.development=next;++session->revision;return true;
}
bool ArRegionalSession_BeginDevelopment(ArRegionalSession *session,
                                       ArRegionalDevelopmentSnapshot *snapshot) {
  if(!snapshot || !Valid(session))return false;
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
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalRecoveryPolicy next;
  if (!ArRegionalRecovery_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.recovery, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.recovery = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRecovery(ArRegionalSession *session,
    ArRegionalRecoverySnapshot *snapshot, unsigned *changed) {
  if (!snapshot || !changed || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalQuakePolicy next;
  if (!ArRegionalQuake_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.quake, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.quake = next;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginQuake(ArRegionalSession *session, ArRegionalQuakeSnapshot *snapshot) {
  if (!snapshot || !Valid(session)) return false;
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

bool ArRegionalSession_RequestLevelGoals(ArRegionalSession *session,uint32_t revision,ArRegionalSource source) {
  bool unused;
  if (!Valid(session) || revision!=session->revision || !ArRegionalLevelGoals_Resolve(source,&unused)) return false;
  if (session->requested.level_goals==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.level_goals=source;++session->revision;return true;
}
bool ArRegionalSession_BeginLevelGoals(ArRegionalSession *session,bool *japanese) {
  if (!japanese || !Valid(session)) return false;
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
  if (!Valid(session) || revision!=session->revision || !ArRegionalTownStatus_Resolve(policy,&unused)) return false;
  if (!memcmp(policy,&session->requested.town_status,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.town_status=*policy; ++session->revision; return true;
}
bool ArRegionalSession_BeginTownStatus(ArRegionalSession *session, ArRegionalTownStatusSnapshot *snapshot) {
  if (!snapshot || !Valid(session)) return false;
  const bool changed=memcmp(&session->requested.town_status,&session->effective.town_status,sizeof(session->requested.town_status))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalTownStatusSnapshot next;
  if (!ArRegionalTownStatus_Resolve(&session->requested.town_status,&next)) return false;
  session->effective.town_status=session->requested.town_status;
  if (changed) ++session->revision;
  *snapshot=next; return true;
}

bool ArRegionalSession_RequestStory(ArRegionalSession *session, uint32_t revision, const ArRegionalStoryPolicy *policy) {
  ArRegionalStorySnapshot unused;
  if (!Valid(session) || revision!=session->revision || !ArRegionalStory_Resolve(policy,&unused)) return false;
  if (!memcmp(policy,&session->requested.story,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.story=*policy; ++session->revision; return true;
}
bool ArRegionalSession_BeginStory(ArRegionalSession *session, ArRegionalStorySnapshot *snapshot) {
  if (!snapshot || !Valid(session)) return false;
  const bool changed=memcmp(&session->requested.story,&session->effective.story,sizeof(session->requested.story))!=0;
  if (changed && session->revision==UINT32_MAX) return false;
  ArRegionalStorySnapshot next;
  if (!ArRegionalStory_Resolve(&session->requested.story,&next)) return false;
  session->effective.story=session->requested.story;
  if (changed) ++session->revision;
  *snapshot=next; return true;
}

bool ArRegionalSession_RequestSkullWait(ArRegionalSession *session, uint32_t revision, ArRegionalSource source) {
  if (!Valid(session) || revision!=session->revision || (unsigned)source>=kArRegionalSource_Count) return false;
  if (session->requested.skull_wait==source) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.skull_wait=source; ++session->revision;
  return true;
}
bool ArRegionalSession_BeginSkullWait(ArRegionalSession *session, uint16_t *frames) {
  if (!frames || !Valid(session)) return false;
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
  if (!Valid(session) || revision!=session->revision || !ArRegionalSources_Resolve(policy,&unused)) return false;
  if (!memcmp(&session->requested.sources,policy,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sources=*policy;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginSources(ArRegionalSession *session, ArRegionalSourcesSnapshot *snapshot) {
  if (!snapshot || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.score_page == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.score_page = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_RequestLivesDisplay(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!Valid(session) || revision != session->revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.lives_display == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.lives_display = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginLivesDisplay(ArRegionalSession *session, bool *zero_based) {
  if (!zero_based || !Valid(session)) return false;
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
  if (!enabled || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.menu_return == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.menu_return = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginMenuReturn(ArRegionalSession *session, bool *keep_open) {
  if (!keep_open || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.speed_range == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.speed_range = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginSpeedRange(ArRegionalSession *session, uint16_t *maximum) {
  if (!maximum || !Valid(session)) return false;
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
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.magic_gesture == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.magic_gesture = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginMagicGesture(ArRegionalSession *session,
                                        bool controls_released, bool *up_attack) {
  if (!up_attack || !Valid(session)) return false;
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
  if (!Valid(session) || revision!=session->revision || !ArRegionalSimCombat_Resolve(policy,&snapshot)) return false;
  if (!memcmp(policy,&session->requested.sim_combat,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sim_combat=*policy;++session->revision;return true;
}
bool ArRegionalSession_RequestSimAi(ArRegionalSession *session,uint32_t revision,const ArRegionalSimAiPolicy *policy) {
  uint16_t snapshot;
  if (!Valid(session) || revision!=session->revision || !ArRegionalSimAi_Resolve(policy,&snapshot)) return false;
  if (!memcmp(policy,&session->requested.sim_ai,sizeof(*policy))) return true;
  if (session->revision==UINT32_MAX) return false;
  session->requested.sim_ai=*policy;++session->revision;return true;
}
bool ArRegionalSession_BeginSimActor(ArRegionalSession *session,unsigned town,unsigned slot) {
  ArRegionalSimActorRules snapshot;
  if (!Valid(session) || town>=6 || slot>=4 || session->sim_actors.active_town_tag!=town+1 ||
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
/* The wire shape is shared, not the units: stable keys select the descriptor
 * for resource counts, initial BCD times, booleans or town service counts. */
static const char *Record(unsigned i, const uint16_t **values) {
  if (i>=kV24RecordCount) {
    const ArRegionalSimAiDescriptor *desc=ArRegionalSimAi_Descriptor((ArRegionalSimAiRule)(i-kV24RecordCount));
    *values=desc->value;return desc->key;
  }
  if (i>=kV23RecordCount) {
    const ArRegionalSimCombatDescriptor *desc=ArRegionalSimCombat_Descriptor((ArRegionalSimCombatRule)(i-kV23RecordCount));
    *values=desc->value;return desc->key;
  }
  if (i==kV22RecordCount) {
    const ArRegionalLevelGoalsDescriptor *desc=ArRegionalLevelGoals_Descriptor();
    *values=desc->japanese;return desc->key;
  }
  if (i >= kV21RecordCount) {
    const ArRegionalTownStatusDescriptor *desc=ArRegionalTownStatus_Descriptor((ArRegionalTownStatusRule)(i-kV21RecordCount));
    *values=desc->japanese; return desc->key;
  }
  if (i >= kV20RecordCount) {
    static const uint16_t japanese[kArRegionalSource_Count]={0,1,0};
    *values=japanese; return "lair_reload_japanese";
  }
  if (i >= kV19RecordCount) {
    const ArRegionalStoryDescriptor *desc=ArRegionalStory_Descriptor((ArRegionalStoryRule)(i-kV19RecordCount));
    *values=desc->value; return desc->key;
  }
  if (i == kV18RecordCount) {
    const ArRegionalSkullWaitDescriptor *desc=ArRegionalSkullWait_Descriptor();
    *values=desc->frames;
    return desc->key;
  }
  if (i >= kV17RecordCount) {
    const ArRegionalSourcesDescriptor *desc=ArRegionalSources_Descriptor((ArRegionalSourceItem)(i-kV17RecordCount));
    *values=desc->automatic;
    return desc->key;
  }
  if (i == kV16RecordCount) {
    const ArRegionalLivesDisplayDescriptor *desc = ArRegionalLivesDisplay_Descriptor();
    *values = desc->zero_based;
    return desc->key;
  }
  if (i >= kV14RecordCount) {
    const ArRegionalScoreDescriptor *desc=ArRegionalScore_Descriptor((ArRegionalScoreRule)(i-kV14RecordCount));
    *values=desc->japanese;
    return desc->key;
  }
  if (i == kV13RecordCount) {
    const ArRegionalHouseCreditDescriptor *desc = ArRegionalHouseCredit_Descriptor();
    *values = desc->tiered;
    return desc->key;
  }
  if (i >= kV12RecordCount) {
    const ArRegionalLairSeedDescriptor *desc = ArRegionalLair_SeedDescriptor(i-kV12RecordCount);
    *values = desc->stock;
    return desc->key;
  }
  if (i == kV10RecordCount) {
    const ArRegionalMagicGestureDescriptor *desc = ArRegionalMagicGesture_Descriptor();
    *values = desc->up_attack;
    return desc->key;
  }
  if (i == kV9RecordCount) {
    const ArRegionalSpeedRangeDescriptor *desc = ArRegionalSpeedRange_Descriptor();
    *values = desc->maximum;
    return desc->key;
  }
  if (i == kV8RecordCount) {
    const ArRegionalMenuReturnDescriptor *desc = ArRegionalMenuReturn_Descriptor();
    *values = desc->keep_open;
    return desc->key;
  }
  if (i == kV7RecordCount) {
    const ArRegionalScorePageDescriptor *desc = ArRegionalScorePage_Descriptor();
    *values = desc->enabled;
    return desc->key;
  }
  if (i >= kV6RecordCount) {
    const ArRegionalQuakeDescriptor *desc = ArRegionalQuake_Descriptor((ArRegionalQuakeRule)(i - kV6RecordCount));
    *values = desc->random;
    return desc->key;
  }
  if (i >= kV5RecordCount) {
    const ArRegionalRecoveryDescriptor *desc = ArRegionalRecovery_Descriptor(
        (ArRegionalRecoveryRule)(i - kV5RecordCount));
    *values = desc->value;
    return desc->key;
  }
  if(i>=kV4RecordCount) {
    const ArRegionalDevelopmentDescriptor *desc=ArRegionalDevelopment_Descriptor(
        (ArRegionalDevelopmentRule)(i-kV4RecordCount));
    *values=desc->updates;return desc->key;
  }
  if (i == kV3RecordCount) {
    const ArRegionalFishingDescriptor *desc = ArRegionalFishing_Descriptor();
    *values = desc->updates;
    return desc->key;
  }
  if (i == kV2RecordCount) {
    const ArRegionalTownWaitDescriptor *desc = ArRegionalTownWait_Descriptor();
    *values = desc->updates;
    return desc->key;
  }
  if (i == kV1RecordCount) {
    const ArRegionalRetryDescriptor *desc = ArRegionalRetry_Descriptor();
    *values = desc->clear_score;
    return desc->key;
  }
  if (i < kArRegionalCostRule_Count) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    *values = desc->price;
    return desc->key;
  }
  const ArRegionalTimerDescriptor *desc = ArRegionalTimers_Descriptor(
      (ArRegionalTimerRule)(i - kArRegionalCostRule_Count));
  *values = desc->bcd;
  return desc->key;
}

static ArRegionalSource RecordSource(const ArRegionalSession *session, unsigned i, bool requested) {
  if (i>=kV24RecordCount) return requested?session->requested.sim_ai.source[i-kV24RecordCount]:
      session->effective.sim_ai.source[i-kV24RecordCount];
  if (i>=kV23RecordCount) return requested?session->requested.sim_combat.source[i-kV23RecordCount]:
      session->effective.sim_combat.source[i-kV23RecordCount];
  if (i==kV22RecordCount) return requested?session->requested.level_goals:session->effective.level_goals;
  if (i >= kV21RecordCount) return requested?session->requested.town_status.source[i-kV21RecordCount]:
      session->effective.town_status.source[i-kV21RecordCount];
  if (i >= kV20RecordCount) return requested?session->requested.lair_reloads:session->effective.lair_reloads;
  if (i >= kV19RecordCount) return requested?session->requested.story.source[i-kV19RecordCount]:
      session->effective.story.source[i-kV19RecordCount];
  if (i == kV18RecordCount) return requested ? session->requested.skull_wait : session->effective.skull_wait;
  if (i >= kV17RecordCount) return requested ? session->requested.sources.source[i-kV17RecordCount] :
      session->effective.sources.source[i-kV17RecordCount];
  if (i == kV16RecordCount) return requested ? session->requested.lives_display : session->effective.lives_display;
  if (i >= kV14RecordCount) return requested ? session->requested.score_feedback.source[i-kV14RecordCount] :
                                             session->effective.score_feedback.source[i-kV14RecordCount];
  if (i == kV13RecordCount) return requested ? session->requested.house_credit : session->effective.house_credit;
  if (i >= kV12RecordCount) return requested ? session->requested.lair_seeds : session->effective.lair_seeds;
  if (i == kV10RecordCount) return requested ? session->requested.magic_gesture : session->effective.magic_gesture;
  if (i == kV9RecordCount) return requested ? session->requested.speed_range : session->effective.speed_range;
  if (i == kV8RecordCount) return requested ? session->requested.menu_return : session->effective.menu_return;
  if (i == kV7RecordCount) return requested ? session->requested.score_page : session->effective.score_page;
  if (i >= kV6RecordCount)
    return requested ? session->requested.quake.source[i - kV6RecordCount] :
                       session->effective.quake.source[i - kV6RecordCount];
  if (i >= kV5RecordCount)
    return requested ? session->requested.recovery.source[i - kV5RecordCount] :
                       session->effective.recovery.source[i - kV5RecordCount];
  if(i>=kV4RecordCount)
    return requested?session->requested.development.source[i-kV4RecordCount]:session->effective.development.source[i-kV4RecordCount];
  if (i == kV3RecordCount)
    return requested ? session->requested.fishing : session->effective.fishing;
  if (i == kV2RecordCount)
    return requested ? session->requested.town_wait : session->effective.town_wait;
  if (i == kV1RecordCount)
    return requested ? session->requested.retry_score : session->effective.retry_score;
  if (i < kArRegionalCostRule_Count)
    return requested ? session->requested.costs.source[i] : session->effective.costs.source[i];
  i -= kArRegionalCostRule_Count;
  return requested ? session->requested.timers.source[i] : session->effective.timers.source[i];
}

bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
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
  if (!Valid(session) || !quote || (unsigned)group >= kArRegionalCostGroup_Count) return false;
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

/* Compact explicit codec, never fwrite a C struct or persist enum ordinals.
 * Each named leaf carries requested/effective source keys AND resolved values.
 * A table/schema change that cannot reproduce those values fails visibly. */
static bool Encode(const ArRegionalSession *session, uint8_t *out, size_t *size) {
  if (!Valid(session)) return false;
  memset(out, 0, kHeaderBytes);
  memcpy(out, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(out + 8, 25);
  ByteOrder_WriteLe16(out + 10, kRecordCount);
  ByteOrder_WriteLe32(out + 12, session->slot);
  memcpy(out + 16, session->campaign, 16);
  ByteOrder_WriteLe32(out + 32, session->revision);
  size_t offset = kHeaderBytes;
  for (unsigned i = 0; i < kRecordCount; ++i) {
    const uint16_t *values;
    const char *key = Record(i, &values);
    const ArRegionalSource requested = RecordSource(session, i, true);
    const ArRegionalSource effective = RecordSource(session, i, false);
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
  if (!ArRegionalLairHistory_Encode(&session->lairs,out+offset,kPayloadCapacity-offset)) return false;
  offset += kArRegionalLairHistoryEncodedBytes;
  if (!ArRegionalLairReloads_Encode(&session->reloads,out+offset,kPayloadCapacity-offset)) return false;
  offset += kArRegionalLairReloadEncodedBytes;
  if (!ArRegionalSimActors_Encode(&session->sim_actors,out+offset,kPayloadCapacity-offset)) return false;
  *size = offset + kArRegionalSimActorsEncodedBytes;
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
  if (version < 1 || version > (pricing_only ? 1u : 25u)) return kSaveCheckpoint_Unsupported;
  const unsigned count = pricing_only ? kArRegionalCostRule_Count :
      version == 1 ? kV1RecordCount : version == 2 ? kV2RecordCount :
      version == 3 ? kV3RecordCount : version == 4 ? kV4RecordCount :
      version == 5 ? kV5RecordCount : version == 6 ? kV6RecordCount :
      version == 7 ? kV7RecordCount : version == 8 ? kV8RecordCount :
      version == 9 ? kV9RecordCount : version == 10 ? kV10RecordCount :
      version <= 12 ? kV12RecordCount : version == 13 ? kV13RecordCount :
      version == 14 ? kV14RecordCount : version == 15 ? kV15RecordCount :
      version == 16 ? kV16RecordCount : version == 17 ? kV17RecordCount :
      version == 18 ? kV18RecordCount : version == 19 ? kV19RecordCount :
      version == 20 ? kV20RecordCount : version == 21 ? kV21RecordCount :
      version == 22 ? kV22RecordCount : version == 23 ? kV23RecordCount :
      version == 24 ? kV24RecordCount : kRecordCount;
  if (ByteOrder_ReadLe16(bytes + 10) != count) return kSaveCheckpoint_Unsupported;
  ArRegionalSession next = {.slot = ByteOrder_ReadLe32(bytes + 12), .revision = ByteOrder_ReadLe32(bytes + 32)};
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
      const char *key = Record(rule, &values);
      if (strlen(key) == length && !memcmp(bytes + offset, key, length)) break;
    }
    if (rule == count) return kSaveCheckpoint_Unsupported;
    if (seen[rule]) return kSaveCheckpoint_Invalid;
    seen[rule] = true;
    offset += length;
    ArRegionalSource requested = DecodeSource(bytes + offset), effective = DecodeSource(bytes + offset + 2);
    if (requested == kArRegionalSource_Count || effective == kArRegionalSource_Count)
      return kSaveCheckpoint_Unsupported;
    if (values[requested] != ByteOrder_ReadLe16(bytes + offset + 4) ||
        values[effective] != ByteOrder_ReadLe16(bytes + offset + 6)) return kSaveCheckpoint_Unsupported;
    if (rule>=kV24RecordCount) {
      next.requested.sim_ai.source[rule-kV24RecordCount]=requested;
      next.effective.sim_ai.source[rule-kV24RecordCount]=effective;
    } else if (rule>=kV23RecordCount) {
      next.requested.sim_combat.source[rule-kV23RecordCount]=requested;
      next.effective.sim_combat.source[rule-kV23RecordCount]=effective;
    } else if (rule==kV22RecordCount) {
      next.requested.level_goals=requested;next.effective.level_goals=effective;
    } else if (rule >= kV21RecordCount) {
      next.requested.town_status.source[rule-kV21RecordCount]=requested;
      next.effective.town_status.source[rule-kV21RecordCount]=effective;
    } else if (rule >= kV20RecordCount) {
      next.requested.lair_reloads=requested; next.effective.lair_reloads=effective;
    } else if (rule >= kV19RecordCount) {
      next.requested.story.source[rule-kV19RecordCount]=requested;
      next.effective.story.source[rule-kV19RecordCount]=effective;
    } else if (rule == kV18RecordCount) {
      next.requested.skull_wait=requested; next.effective.skull_wait=effective;
    } else if (rule >= kV17RecordCount) {
      next.requested.sources.source[rule-kV17RecordCount]=requested;
      next.effective.sources.source[rule-kV17RecordCount]=effective;
    } else if (rule == kV16RecordCount) {
      next.requested.lives_display = requested; next.effective.lives_display = effective;
    } else if (rule >= kV14RecordCount) {
      next.requested.score_feedback.source[rule-kV14RecordCount]=requested;
      next.effective.score_feedback.source[rule-kV14RecordCount]=effective;
    } else if (rule == kV13RecordCount) {
      next.requested.house_credit = requested; next.effective.house_credit = effective;
    } else if (rule >= kV12RecordCount) {
      if (seed_seen && (next.requested.lair_seeds != requested || next.effective.lair_seeds != effective))
        return kSaveCheckpoint_Invalid;
      next.requested.lair_seeds = requested; next.effective.lair_seeds = effective;
      seed_seen = true;
    } else if (rule == kV10RecordCount) {
      next.requested.magic_gesture = requested; next.effective.magic_gesture = effective;
    } else if (rule == kV9RecordCount) {
      next.requested.speed_range = requested; next.effective.speed_range = effective;
    } else if (rule == kV8RecordCount) {
      next.requested.menu_return = requested; next.effective.menu_return = effective;
    } else if (rule == kV7RecordCount) {
      next.requested.score_page = requested; next.effective.score_page = effective;
    } else if (rule >= kV6RecordCount) {
      next.requested.quake.source[rule - kV6RecordCount] = requested;
      next.effective.quake.source[rule - kV6RecordCount] = effective;
    } else if (rule >= kV5RecordCount) {
      next.requested.recovery.source[rule - kV5RecordCount] = requested;
      next.effective.recovery.source[rule - kV5RecordCount] = effective;
    } else if(rule>=kV4RecordCount) {
      next.requested.development.source[rule-kV4RecordCount]=requested;
      next.effective.development.source[rule-kV4RecordCount]=effective;
    } else if (rule == kV3RecordCount) {
      next.requested.fishing = requested;
      next.effective.fishing = effective;
    } else if (rule == kV2RecordCount) {
      next.requested.town_wait = requested;
      next.effective.town_wait = effective;
    } else if (rule == kV1RecordCount) {
      next.requested.retry_score = requested;
      next.effective.retry_score = effective;
    } else if (rule < kArRegionalCostRule_Count) {
      next.requested.costs.source[rule] = requested;
      next.effective.costs.source[rule] = effective;
    } else {
      next.requested.timers.source[rule - kArRegionalCostRule_Count] = requested;
      next.effective.timers.source[rule - kArRegionalCostRule_Count] = effective;
    }
    offset += 8;
  }
  if (version>=12) {
    if (size-offset>=8 && !memcmp(bytes+offset,"ARLHIST",7) && bytes[offset+7]!='1')
      return kSaveCheckpoint_Unsupported;
    const size_t history_size=version>=21 ? kArRegionalLairHistoryEncodedBytes : size-offset;
    if (history_size>size-offset || !ArRegionalLairHistory_Decode(bytes+offset,history_size,&next.lairs))
      return kSaveCheckpoint_Invalid;
    offset+=history_size;
    if (version>=21) {
      if (size-offset>=8 && !memcmp(bytes+offset,"ARLDELY",7) && bytes[offset+7]!='1')
        return kSaveCheckpoint_Unsupported;
      const size_t reload_size=version>=24?kArRegionalLairReloadEncodedBytes:size-offset;
      if (reload_size>size-offset || !ArRegionalLairReloads_Decode(bytes+offset,reload_size,&next.reloads)) return kSaveCheckpoint_Invalid;
      offset+=reload_size;
      if (version>=24) {
        if (size-offset>=8 && !memcmp(bytes+offset,"ARSIMAC",7) && bytes[offset+7]!=(version==24?'1':'2')) return kSaveCheckpoint_Unsupported;
        if (!ArRegionalSimActors_Decode(bytes+offset,size-offset,&next.sim_actors)) return kSaveCheckpoint_Invalid;
      }
    }
  } else if (offset != size) return kSaveCheckpoint_Invalid;
  if (!Valid(&next)) return kSaveCheckpoint_Invalid;
  *session = next;
  return kSaveCheckpoint_Ready;
}

SaveCheckpointStatus ArRegionalSession_Load(ArRegionalSession *session,
    uint32_t slot, const char *path, const uint8_t *image, SaveError *error) {
  if (error) error->message[0] = 0;
  if (!session) return kSaveCheckpoint_Invalid;
  uint8_t payload[kSaveCheckpointPayloadMax];
  size_t size;
  SaveCheckpointStatus status = SaveCheckpoint_Read(path, image, payload, sizeof(payload), &size, error);
  if (status != kSaveCheckpoint_Ready) return status;
  ArRegionalSession next;
  status = Decode(payload, size, &next);
  if (status == kSaveCheckpoint_Ready && next.slot != slot) status = kSaveCheckpoint_Mismatch;
  if (status != kSaveCheckpoint_Ready) {
    if (error) snprintf(error->message, sizeof(error->message),
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
    const char *path, const uint8_t *expected, const uint8_t *image, SaveError *error) {
  uint8_t payload[kPayloadCapacity];
  size_t size;
  if (error) error->message[0] = 0;
  if (!Encode(session, payload, &size)) {
    if (error) snprintf(error->message, sizeof(error->message), "invalid regional rules session");
    return false;
  }
  uint32_t slot = session->slot;
  return SaveCheckpoint_Commit(format, path, expected, image, payload, size,
                               ValidatePayload, &slot, error);
}
