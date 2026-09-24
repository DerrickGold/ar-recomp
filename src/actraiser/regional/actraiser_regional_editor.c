#include "actraiser/regional/actraiser_regional_editor.h"

#include <string.h>

/* Activation can advance revision while travelling to the Palace. Bind the
 * volatile intent to campaign + requested values; the Palace must still make
 * a fresh revision-checked preview before committing a destructive change. */
bool ActRaiserRegionalEditor_PopulationPending(const ActRaiserRegionalPopulationIntent *intent,
                                               const ArRegionalSession *session) {
  return intent && intent->pending && session &&
         !memcmp(intent->campaign, session->campaign, sizeof(intent->campaign)) &&
         !memcmp(&intent->base_rules, &session->requested, sizeof(session->requested));
}

void ActRaiserRegionalEditor_InvalidatePopulation(ActRaiserRegionalPopulationIntent *intent,
                                                  const ArRegionalSession *session) {
  if (intent && !ActRaiserRegionalEditor_PopulationPending(intent, session))
    intent->pending = false;
}

/* Prepare once for both preview and apply. No session/intent mutation, CPU,
 * prompts or I/O here; failed preparation leaves the live campaign untouched. */
static ActRaiserRegionalEditResult PrepareProfile(const ActRaiserRegionalEditContext *edit,
                                                  const ActRaiserRegionalRulesView *view,
                                                  ArRegionalProfileGroup group,
                                                  ArRegionalSource source,
                                                  ArRegionalSession *candidate,
                                                  ActRaiserRegionalEditImpact *impact) {
  ArRegionalSession *session = edit ? edit->session : NULL;
  if (!view || !session || !edit->population || (unsigned)group >= kArRegionalProfile_Count ||
      (unsigned)source >= kArRegionalSource_Count)
    return kActRaiserRegionalEdit_Invalid;
  if (!edit->editable) return kActRaiserRegionalEdit_Locked;
  if (view->new_game != edit->new_game || view->revision != session->revision ||
      memcmp(view->campaign, session->campaign, 16))
    return kActRaiserRegionalEdit_Stale;
  ArRegionalRules requested;
  if (!ArRegionalProfiles_Expand(&session->requested, group, source, &requested))
    return kActRaiserRegionalEdit_Invalid;
  const bool accounting_changed =
      !ArRegionalRules_SameAccounting(&session->requested, &requested) ||
      memcmp(&session->requested.score_feedback, &requested.score_feedback,
             sizeof(requested.score_feedback));
  if ((accounting_changed &&
       (session->lairs.initialized_towns != 63 || session->lairs.diverged_towns)) ||
      (session->requested.lair_reloads != requested.lair_reloads &&
       (session->reloads.initialized_towns != 63 || session->reloads.diverged_towns)))
    return kActRaiserRegionalEdit_HistoryUnavailable;
  ArRegionalSupportSnapshot before, after;
  if (!ArRegionalSupport_Resolve(&session->effective.support, &before) ||
      !ArRegionalSupport_Resolve(&requested.support, &after))
    return kActRaiserRegionalEdit_Invalid;
  const bool redevelop = memcmp(&before, &after, sizeof(before)) != 0;
  *candidate = *session;
  if (redevelop && !ArRegionalSession_SetPopulationProfile(candidate, candidate->revision, source))
    return kActRaiserRegionalEdit_Incompatible;
  if (!ArRegionalSession_RequestRules(candidate, candidate->revision, &requested))
    return kActRaiserRegionalEdit_Invalid;
  uint16_t changes = 0;
  if (!ArRegionalProfiles_Changes(&session->requested, &requested, &changes))
    return kActRaiserRegionalEdit_Invalid;
  *impact = (ActRaiserRegionalEditImpact){0};
  if (!edit->new_game) {
    bool affects_towns = false;
    for (unsigned member = 0; member < kArRegionalProfile_GroupCount; ++member)
      if ((changes & ArRegionalProfiles_Mask(member)) &&
          ArRegionalProfiles_TownImpact(member) != kArRegionalTownImpact_None)
        affects_towns = true;
    impact->towns = redevelop       ? kArRegionalTownImpact_Redevelopment
                    : affects_towns ? kArRegionalTownImpact_Future
                                    : kArRegionalTownImpact_None;
    impact->estimated_history =
        changes && ((accounting_changed && session->lairs.approximate_towns) ||
                    (session->requested.lair_reloads != requested.lair_reloads &&
                     session->reloads.approximate_towns));
  }
  return redevelop && !edit->new_game            ? kActRaiserRegionalEdit_Deferred
         : candidate->revision == view->revision ? kActRaiserRegionalEdit_Unchanged
                                                 : kActRaiserRegionalEdit_Applied;
}

ActRaiserRegionalEditResult ActRaiserRegionalEditor_PreviewProfile(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalProfileGroup group, ArRegionalSource source, ActRaiserRegionalEditImpact *out) {
  if (!out) return kActRaiserRegionalEdit_Invalid;
  ArRegionalSession candidate;
  ActRaiserRegionalEditImpact impact;
  const ActRaiserRegionalEditResult result =
      PrepareProfile(edit, view, group, source, &candidate, &impact);
  if (result == kActRaiserRegionalEdit_Applied || result == kActRaiserRegionalEdit_Unchanged ||
      result == kActRaiserRegionalEdit_Deferred)
    *out = impact;
  return result;
}

ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestProfile(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalProfileGroup group, ArRegionalSource source) {
  ArRegionalSession candidate;
  ActRaiserRegionalEditImpact impact;
  const ActRaiserRegionalEditResult result =
      PrepareProfile(edit, view, group, source, &candidate, &impact);
  if (result == kActRaiserRegionalEdit_Deferred) {
    edit->population->pending = true;
    edit->population->profile = true;
    edit->population->group = group;
    edit->population->source = source;
    edit->population->base_rules = edit->session->requested;
    memcpy(edit->population->campaign, view->campaign, 16);
    return kActRaiserRegionalEdit_Deferred;
  }
  if (result != kActRaiserRegionalEdit_Applied && result != kActRaiserRegionalEdit_Unchanged)
    return result;
  // Selecting the current population/gameplay profile cancels a queued reset.
  if (group == kArRegionalProfile_Population || group == kArRegionalProfile_Gameplay)
    edit->population->pending = false;
  *edit->session = candidate;
  ActRaiserRegionalEditor_InvalidatePopulation(edit->population, edit->session);
  return result;
}

ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestRules(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, ArRegionalSource source) {
  ArRegionalSession *session = edit ? edit->session : NULL;
  if (!view || !session || !edit->population ||
      (unsigned)group >= kActRaiserRegionalSetting_Count ||
      (unsigned)source >= kArRegionalSource_Count)
    return kActRaiserRegionalEdit_Invalid;
  if (!edit->editable) return kActRaiserRegionalEdit_Locked;
  if (view->new_game != edit->new_game || view->revision != session->revision ||
      memcmp(view->campaign, session->campaign, sizeof(view->campaign)))
    return kActRaiserRegionalEdit_Stale;
  bool ok;
  switch (group) {
    case kActRaiserRegionalSetting_StartingHealth:
    case kActRaiserRegionalSetting_StartingLives: {
      ArRegionalActionStartPolicy policy = session->requested.action_start;
      policy.source[group == kActRaiserRegionalSetting_StartingHealth
                        ? kArRegionalActionStart_Health : kArRegionalActionStart_Spares] = source;
      ok = ArRegionalSession_RequestActionStart(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_SpRecovery:
    case kActRaiserRegionalSetting_AngelRecovery: {
      ArRegionalRecoveryPolicy policy = session->requested.recovery;
      policy.source[group == kActRaiserRegionalSetting_SpRecovery
                        ? kArRegionalRecovery_SP : kArRegionalRecovery_Angel] = source;
      ok = ArRegionalSession_RequestRecoveryPolicy(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_CompassReturn: {
      ArRegionalStoryPolicy policy = session->requested.story;
      policy.source[kArRegionalStory_ClearCompassPrerequisite] = source;
      ok = ArRegionalSession_RequestStory(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_ActorArt: {
      ArRegionalActorArtworkPolicy policy;
      for (unsigned i = 0; i < kArRegionalActorArtwork_Count; ++i) policy.source[i] = source;
      ok = ArRegionalSession_RequestActorArtwork(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_ModeEntry: {
      ArRegionalModePolicy policy;
      ArRegionalMode_Init(&policy, source);
      ok = ArRegionalSession_RequestModeEntry(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Inventory:
      ok = ArRegionalSession_RequestInventory(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_EnemyPlacements:
    case kActRaiserRegionalSetting_PickupPlacements: {
      ArRegionalPlacementPolicy policy = session->requested.placements;
      if (group == kActRaiserRegionalSetting_EnemyPlacements)
        policy.enemies = source;
      else
        policy.pickups = source;
      ok = ArRegionalSession_RequestPlacements(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Music:
      ok = ArRegionalSession_RequestMusic(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Sequences: {
      ArRegionalSequencePolicy policy = {{source, source}};
      ok = ArRegionalSession_RequestSequences(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_DeathHeimArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision, kArRegionalArtwork_DeathHeim,
                                            source);
      break;
    case kActRaiserRegionalSetting_ActionItemArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision, kArRegionalArtwork_ActionItems,
                                            source);
      break;
    case kActRaiserRegionalSetting_FollowerArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision,
                                            kArRegionalArtwork_FollowerSymbols, source);
      break;
    case kActRaiserRegionalSetting_LairArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision, kArRegionalArtwork_LairSymbols,
                                            source);
      break;
    case kActRaiserRegionalSetting_PyramidArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision,
                                            kArRegionalArtwork_PyramidDetail, source);
      break;
    case kActRaiserRegionalSetting_TitleArt:
      ok = ArRegionalSession_RequestArtwork(session, view->revision,
                                            kArRegionalArtwork_TitleBackground, source);
      break;
    case kActRaiserRegionalSetting_AitosPoses: {
      const ArRegionalPosePolicy policy = {{source, source}};
      ok = ArRegionalSession_RequestPoses(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Mosaic:
      ok = ArRegionalSession_RequestMosaic(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Terrain:
      ok = ArRegionalSession_RequestTerrain(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Hazards:
      ok = ArRegionalSession_RequestHazards(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_ActionStart: {
      ArRegionalActionStartPolicy policy;
      ArRegionalActionStart_Init(&policy, source);
      ok = ArRegionalSession_RequestActionStart(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_ScoreLives:
      ok = ArRegionalSession_RequestScoreLives(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_DifficultyRules: {
      ArRegionalDifficultyPolicy policy;
      ArRegionalDifficulty_Init(&policy, source, session->requested.difficulty.level);
      ok = ArRegionalSession_RequestDifficulty(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_FireEnemy: {
      ArRegionalFirePolicy policy;
      ArRegionalFire_Init(&policy, source);
      ok = ArRegionalSession_RequestFire(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_CastHold: {
      ArRegionalCastHoldPolicy policy;
      ArRegionalCastHold_Init(&policy, source);
      ok = ArRegionalSession_RequestCastHold(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Bosses: {
      ArRegionalBossPolicy policy;
      ArRegionalBoss_Init(&policy, source);
      ok = ArRegionalSession_RequestBosses(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Collision: {
      ArRegionalCollisionPolicy policy;
      ArRegionalCollision_Init(&policy, source);
      ok = ArRegionalSession_RequestCollision(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_PlatformSkull: {
      ArRegionalPlatformSkullPolicy policy;
      ArRegionalPlatformSkull_Init(&policy, source);
      ok = ArRegionalSession_RequestPlatformSkull(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_ActorStats: {
      ArRegionalActorStatsPolicy policy;
      ArRegionalActorStats_Init(&policy, source);
      ok = ArRegionalSession_RequestActorStats(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_StatueVolley:
      ok = ArRegionalSession_RequestVolley(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Emitters: {
      ArRegionalEmitterPolicy policy;
      ArRegionalEmitter_Init(&policy, source);
      ok = ArRegionalSession_RequestEmitters(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_ActionMotion: {
      ArRegionalActionMotionPolicy policy;
      ArRegionalActionMotion_Init(&policy, source);
      ok = ArRegionalSession_RequestActionMotion(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_Arrival:
      ok = ArRegionalSession_RequestArrival(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Population: {
      ArRegionalSession candidate = *session;
      if (!ArRegionalSession_SetPopulationProfile(&candidate, view->revision, source))
        return kActRaiserRegionalEdit_Invalid;
      ArRegionalSupportSnapshot before, after;
      if (!ArRegionalSupport_Resolve(&session->effective.support, &before) ||
          !ArRegionalSupport_Resolve(&candidate.requested.support, &after))
        return kActRaiserRegionalEdit_Invalid;
      if (edit->new_game || !memcmp(&before, &after, sizeof(before))) {
        *session = candidate;
        edit->population->pending = false;
        return candidate.revision == view->revision ? kActRaiserRegionalEdit_Unchanged
                                                    : kActRaiserRegionalEdit_Applied;
      }
      if (candidate.revision == view->revision) {
        edit->population->pending = false;
        return kActRaiserRegionalEdit_Unchanged;
      }
      edit->population->pending = true;
      edit->population->source = source;
      edit->population->base_rules = session->requested;
      edit->population->profile = false;
      memcpy(edit->population->campaign, view->campaign, 16);
      return kActRaiserRegionalEdit_Deferred;
    }
    case kActRaiserRegionalSetting_Construction:
      ok = ArRegionalSession_RequestConstruction(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_SimAi: {
      ArRegionalSimAiPolicy policy;
      if (!ArRegionalSimAi_Init(&policy, source)) return kActRaiserRegionalEdit_Invalid;
      ok = ArRegionalSession_RequestSimAi(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_SimCombat: {
      ArRegionalSimCombatPolicy policy;
      if (!ArRegionalSimCombat_Init(&policy, source)) return kActRaiserRegionalEdit_Invalid;
      ok = ArRegionalSession_RequestSimCombat(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_LevelGoals:
      if (source != kArRegionalSource_Japan) {
        ArRegionalRules candidate = session->requested;
        candidate.level_goals = source;
        if (!ArRegionalRules_PopulationCompatible(&candidate))
          return kActRaiserRegionalEdit_Incompatible;
      }
      ok = ArRegionalSession_RequestLevelGoals(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_TownStatus: {
      ArRegionalTownStatusPolicy policy;
      ok = ArRegionalTownStatus_Init(&policy, source) &&
           ArRegionalSession_RequestTownStatus(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_LairReloads:
      if (session->reloads.initialized_towns != 0x3f || session->reloads.diverged_towns)
        return kActRaiserRegionalEdit_HistoryUnavailable;
      ok = ArRegionalSession_RequestLairReloads(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Story: {
      ArRegionalStoryPolicy policy;
      ArRegionalRules candidate = session->requested;
      ArRegionalStory_Init(&candidate.story, source);
      if (!ArRegionalRules_PopulationCompatible(&candidate))
        return kActRaiserRegionalEdit_Incompatible;
      ok = ArRegionalStory_Init(&policy, source) &&
           ArRegionalSession_RequestStory(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_SkullWait:
      ok = ArRegionalSession_RequestSkullWait(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Sources: {
      ArRegionalSourcesPolicy policy;
      ok = ArRegionalSources_Init(&policy, source) &&
           ArRegionalSession_RequestSources(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_LivesDisplay:
      ok = ArRegionalSession_RequestLivesDisplay(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_ScoreFeedback: {
      if (session->lairs.initialized_towns != 0x3f || session->lairs.diverged_towns)
        return kActRaiserRegionalEdit_HistoryUnavailable;
      ArRegionalScorePolicy policy;
      ok = ArRegionalScore_Init(&policy, source) &&
           ArRegionalSession_RequestScoreFeedback(session, view->revision, &policy);
      break;
    }
    case kActRaiserRegionalSetting_HouseCredit:
      if (session->lairs.initialized_towns != 0x3f || session->lairs.diverged_towns)
        return kActRaiserRegionalEdit_HistoryUnavailable;
      ok = ArRegionalSession_RequestHouseCredit(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_LairReserves:
      if (session->lairs.initialized_towns != 0x3f || session->lairs.diverged_towns)
        return kActRaiserRegionalEdit_HistoryUnavailable;
      ok = ArRegionalSession_RequestLairSeeds(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_RoomTimes:
      ok = ArRegionalSession_RequestTimers(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_RetryScore:
      ok = ArRegionalSession_RequestRetryScore(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_TownWait:
      ok = ArRegionalSession_RequestTownWait(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Fishing:
      ok = ArRegionalSession_RequestFishing(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Development:
      ok = ArRegionalSession_RequestDevelopment(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Recovery:
      ok = ArRegionalSession_RequestRecovery(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Quake:
      ok = ArRegionalSession_RequestQuake(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_ScorePage:
      ok = ArRegionalSession_RequestScorePage(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_MenuReturn:
      ok = ArRegionalSession_RequestMenuReturn(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_SpeedRange:
      ok = ArRegionalSession_RequestSpeedRange(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_MagicGesture:
      ok = ArRegionalSession_RequestMagicGesture(session, view->revision, source);
      break;
    case kActRaiserRegionalSetting_Scrolls:
      ok = ArRegionalSession_RequestCosts(session, view->revision, kArRegionalCostGroup_Scrolls,
                                          source);
      break;
    case kActRaiserRegionalSetting_Miracles:
      ok = ArRegionalSession_RequestCosts(session, view->revision, kArRegionalCostGroup_Miracles,
                                          source);
      break;
    default:
      return kActRaiserRegionalEdit_Invalid;
  }
  if (!ok) return kActRaiserRegionalEdit_Invalid;
  ActRaiserRegionalEditor_InvalidatePopulation(edit->population, session);
  return view->revision == session->revision ? kActRaiserRegionalEdit_Unchanged
                                             : kActRaiserRegionalEdit_Applied;
}

/* Run the same validated narrow edit against a local candidate. Neither the
 * live session nor its queued Palace intent can be changed by a preview. */
ActRaiserRegionalEditResult ActRaiserRegionalEditor_PreviewRules(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, ArRegionalSource source, ActRaiserRegionalEditImpact *out) {
  if (!edit || !edit->session || !edit->population || !out) return kActRaiserRegionalEdit_Invalid;
  ArRegionalSession candidate = *edit->session;
  ActRaiserRegionalPopulationIntent intent = *edit->population;
  const ActRaiserRegionalEditContext local = {&candidate, &intent, edit->new_game, edit->editable};
  const ActRaiserRegionalEditResult result = ActRaiserRegionalEditor_RequestRules(&local, view, group, source);
  if (result != kActRaiserRegionalEdit_Applied && result != kActRaiserRegionalEdit_Unchanged &&
      result != kActRaiserRegionalEdit_Deferred) return result;
  ActRaiserRegionalEditImpact impact = {0};
  if (!edit->new_game) {
    uint16_t changes;
    if (!ArRegionalProfiles_Changes(&edit->session->requested, &candidate.requested, &changes))
      return kActRaiserRegionalEdit_Invalid;
    for (unsigned i = 0; i < kArRegionalProfile_GroupCount; ++i)
      if (group != kActRaiserRegionalSetting_TownStatus && group != kActRaiserRegionalSetting_CompassReturn &&
          (changes & ArRegionalProfiles_Mask(i)) && ArRegionalProfiles_TownImpact(i))
        impact.towns = kArRegionalTownImpact_Future;
    if (result == kActRaiserRegionalEdit_Deferred) impact.towns = kArRegionalTownImpact_Redevelopment;
    const ArRegionalRules *before = &edit->session->requested, *after = &candidate.requested;
    impact.estimated_history =
        ((!ArRegionalRules_SameAccounting(before, after) ||
          memcmp(&before->score_feedback, &after->score_feedback, sizeof(before->score_feedback))) &&
         edit->session->lairs.approximate_towns) ||
        (before->lair_reloads != after->lair_reloads && edit->session->reloads.approximate_towns);
  }
  *out = impact;
  return result;
}

static ActRaiserRegionalEditResult RequestDifficultyPolicy(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    const ArRegionalDifficultyPolicy *policy) {
  ArRegionalSession *session = edit ? edit->session : NULL;
  if (!view || !session || !edit->population || !policy)
    return kActRaiserRegionalEdit_Invalid;
  if (!edit->editable) return kActRaiserRegionalEdit_Locked;
  if (view->new_game != edit->new_game || view->revision != session->revision ||
      memcmp(view->campaign, session->campaign, 16))
    return kActRaiserRegionalEdit_Stale;
  if (!ArRegionalSession_RequestDifficulty(session, view->revision, policy))
    return kActRaiserRegionalEdit_Invalid;
  ActRaiserRegionalEditor_InvalidatePopulation(edit->population, session);
  return view->revision == session->revision ? kActRaiserRegionalEdit_Unchanged
                                             : kActRaiserRegionalEdit_Applied;
}

ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestDifficulty(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalDifficulty level) {
  if (!edit || !edit->session || (unsigned)level >= kArRegionalDifficulty_Count)
    return kActRaiserRegionalEdit_Invalid;
  ArRegionalDifficultyPolicy policy = edit->session->requested.difficulty;
  policy.level = level;
  return RequestDifficultyPolicy(edit, view, &policy);
}

ActRaiserRegionalEditResult ActRaiserRegionalEditor_RequestDifficultyChoice(
    const ActRaiserRegionalEditContext *edit, const ActRaiserRegionalRulesView *view,
    ArRegionalDifficultyChoice choice) {
  ArRegionalDifficultyPolicy policy;
  if (!ArRegionalDifficulty_Select(choice, &policy)) return kActRaiserRegionalEdit_Invalid;
  return RequestDifficultyPolicy(edit, view, &policy);
}
