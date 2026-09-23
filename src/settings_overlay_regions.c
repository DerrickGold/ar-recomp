#include "settings_overlay_regions.h"

#include <stdio.h>

bool SettingsOverlayRegions_PopulationConfirmation(ArUiLocale locale,
    ArRegionalSource source,const uint16_t removed[6],char *output,size_t capacity) {
  if((unsigned)source>=kArRegionalSource_Count || !removed)return false;
  char towns[512]={0};size_t used=0;
  for(unsigned town=0;town<6;++town)if(removed[town]) {
    char key[32];snprintf(key,sizeof(key),"overlay.region.town.%u",town);
    const int count=snprintf(towns+used,sizeof(towns)-used,"%s%s %u",used?", ":"",
        ArUiCatalog_Text(locale,key,NULL),removed[town]);
    if(count<0 || (size_t)count>=sizeof(towns)-used)return false;
    used+=(size_t)count;
  }
  const SettingsOverlayRegionBadge badge=source==kArRegionalSource_Japan?kOverlayRegionBadge_Japan:
      source==kArRegionalSource_Europe?kOverlayRegionBadge_Europe:kOverlayRegionBadge_US;
  const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"towns",used?towns:ArUiCatalog_Text(locale,"overlay.region.population_none",NULL)}};
  return ArUiCatalog_Format(output,capacity,
      ArUiCatalog_Text(locale,"overlay.region.population_confirm",NULL),args,2);
}

const char *SettingsOverlayRegions_RowKey(ActRaiserRegionalSettingGroup group) {
  switch (group) {
    case kActRaiserRegionalSetting_Scrolls: return "regional_scroll_prices";
    case kActRaiserRegionalSetting_Miracles: return "regional_miracle_prices";
    case kActRaiserRegionalSetting_RoomTimes: return "regional_room_times";
    case kActRaiserRegionalSetting_RetryScore: return "regional_retry_score";
    case kActRaiserRegionalSetting_TownWait: return "regional_town_wait";
    case kActRaiserRegionalSetting_Fishing: return "regional_fishing";
    case kActRaiserRegionalSetting_Development: return "regional_development";
    case kActRaiserRegionalSetting_Recovery: return "regional_recovery";
    case kActRaiserRegionalSetting_Quake: return "regional_quake";
    case kActRaiserRegionalSetting_ScorePage: return "regional_score_page";
    case kActRaiserRegionalSetting_MenuReturn: return "regional_menu_return";
    case kActRaiserRegionalSetting_SpeedRange: return "regional_speed_range";
    case kActRaiserRegionalSetting_MagicGesture: return "regional_magic_gesture";
    case kActRaiserRegionalSetting_LairReserves: return "regional_lair_reserves";
    case kActRaiserRegionalSetting_HouseCredit: return "regional_house_credit";
    case kActRaiserRegionalSetting_ScoreFeedback: return "regional_score_feedback";
    case kActRaiserRegionalSetting_LivesDisplay: return "regional_lives_display";
    case kActRaiserRegionalSetting_Sources: return "regional_sources";
    case kActRaiserRegionalSetting_SkullWait: return "regional_skull_wait";
    case kActRaiserRegionalSetting_Story: return "regional_story";
    case kActRaiserRegionalSetting_LairReloads: return "regional_lair_reloads";
    case kActRaiserRegionalSetting_TownStatus: return "regional_town_status";
    case kActRaiserRegionalSetting_LevelGoals: return "regional_level_goals";
    case kActRaiserRegionalSetting_SimCombat: return "regional_sim_combat";
    case kActRaiserRegionalSetting_SimAi: return "regional_sim_ai";
    case kActRaiserRegionalSetting_Construction: return "regional_construction";
    case kActRaiserRegionalSetting_Population: return "regional_population";
    case kActRaiserRegionalSetting_Arrival: return "regional_arrival";
    case kActRaiserRegionalSetting_ActionMotion: return "regional_action_motion";
    case kActRaiserRegionalSetting_Emitters: return "regional_emitters";
    case kActRaiserRegionalSetting_StatueVolley: return "regional_statue_volley";
    case kActRaiserRegionalSetting_Bosses: return "regional_boss_rules";
    case kActRaiserRegionalSetting_Collision: return "regional_collision";
    case kActRaiserRegionalSetting_PlatformSkull: return "regional_platform_skull";
    case kActRaiserRegionalSetting_ActorStats: return "regional_actor_stats";
    default: return "";
  }
}

const char *SettingsOverlayRegions_RowLabel(ArUiLocale locale, ActRaiserRegionalSettingGroup group) {
  switch (group) {
    case kActRaiserRegionalSetting_Scrolls:
      return ArUiCatalog_Text(locale, "overlay.region.scroll_label", "Spell scroll costs");
    case kActRaiserRegionalSetting_Miracles:
      return ArUiCatalog_Text(locale, "overlay.region.miracle_label", "Miracle SP costs");
    case kActRaiserRegionalSetting_RoomTimes:
      return ArUiCatalog_Text(locale, "overlay.region.time_label", "Initial room time");
    case kActRaiserRegionalSetting_RetryScore:
      return ArUiCatalog_Text(locale, "overlay.region.retry_label", "Checkpoint score");
    case kActRaiserRegionalSetting_TownWait:
      return ArUiCatalog_Text(locale, "overlay.region.wait_label", "Construction wait");
    case kActRaiserRegionalSetting_Fishing:
      return ArUiCatalog_Text(locale, "overlay.region.fishing_label", "Fillmore fishing");
    case kActRaiserRegionalSetting_Development:
      return ArUiCatalog_Text(locale, "overlay.region.development_label", "Development clock");
    case kActRaiserRegionalSetting_Recovery:
      return ArUiCatalog_Text(locale, "overlay.region.recovery_label", "Town recovery");
    case kActRaiserRegionalSetting_Quake:
      return ArUiCatalog_Text(locale, "overlay.region.quake_label", "Earthquake destruction");
    case kActRaiserRegionalSetting_ScorePage:
      return ArUiCatalog_Text(locale, "overlay.region.score_page_label", "Master score page");
    case kActRaiserRegionalSetting_MenuReturn:
      return ArUiCatalog_Text(locale, "overlay.region.menu_return_label", "Town menu return");
    case kActRaiserRegionalSetting_SpeedRange:
      return ArUiCatalog_Text(locale, "overlay.region.speed_range_label", "Message-speed range");
    case kActRaiserRegionalSetting_MagicGesture:
      return ArUiCatalog_Text(locale, "overlay.region.magic_gesture_label", "Magic controls");
    case kActRaiserRegionalSetting_LairReserves:
      return ArUiCatalog_Text(locale, "overlay.region.lair_label", "Monster reserves");
    case kActRaiserRegionalSetting_HouseCredit:
      return ArUiCatalog_Text(locale, "overlay.region.house_label", "House-loss feedback");
    case kActRaiserRegionalSetting_ScoreFeedback:
      return ArUiCatalog_Text(locale, "overlay.region.score_feedback_label", "Act-score feedback");
    case kActRaiserRegionalSetting_LivesDisplay:
      return ArUiCatalog_Text(locale, "overlay.region.lives_label", "Action HUD lives");
    case kActRaiserRegionalSetting_Sources:
      return ArUiCatalog_Text(locale, "overlay.region.sources_label", "Source activation");
    case kActRaiserRegionalSetting_SkullWait:
      return ArUiCatalog_Text(locale, "overlay.region.skull_label", "Magic Skull wait");
    case kActRaiserRegionalSetting_Story:
      return ArUiCatalog_Text(locale, "overlay.region.story_label", "Story prerequisites");
    case kActRaiserRegionalSetting_LairReloads:
      return ArUiCatalog_Text(locale,"overlay.region.reload_label","Lair respawn delays");
    case kActRaiserRegionalSetting_TownStatus:
      return ArUiCatalog_Text(locale,"overlay.region.town_status_label","Town growth reports");
    case kActRaiserRegionalSetting_LevelGoals:
      return ArUiCatalog_Text(locale,"overlay.region.level_label","Level population goals");
    case kActRaiserRegionalSetting_SimCombat:
      return ArUiCatalog_Text(locale,"overlay.region.sim_combat_label","Town monster combat");
    case kActRaiserRegionalSetting_SimAi:
      return ArUiCatalog_Text(locale,"overlay.region.sim_ai_label","Town monster behavior");
    case kActRaiserRegionalSetting_Construction:
      return ArUiCatalog_Text(locale,"overlay.region.construction_label","Construction growth cost");
    case kActRaiserRegionalSetting_Population:
      return ArUiCatalog_Text(locale,"overlay.region.population_label","Population rules");
    case kActRaiserRegionalSetting_Arrival:
      return ArUiCatalog_Text(locale,"overlay.region.arrival_label","Death Heim arrival");
    case kActRaiserRegionalSetting_ActionMotion:
      return ArUiCatalog_Text(locale,"overlay.region.action_motion_label","Enemy movement & recovery");
    case kActRaiserRegionalSetting_Emitters:
      return ArUiCatalog_Text(locale,"overlay.region.emitters_label","Cave fireball emitters");
    case kActRaiserRegionalSetting_StatueVolley:
      return ArUiCatalog_Text(locale,"overlay.region.statue_volley_label","Bloodpool statue volleys");
    case kActRaiserRegionalSetting_Bosses:
      return ArUiCatalog_Text(locale,"overlay.region.bosses_label","Boss attack patterns");
    case kActRaiserRegionalSetting_Collision:
      return ArUiCatalog_Text(locale,"overlay.region.collision_label","Enemy collision shapes");
    case kActRaiserRegionalSetting_PlatformSkull:
      return ArUiCatalog_Text(locale,"overlay.region.platform_skull_label","Aitos platform skulls");
    case kActRaiserRegionalSetting_ActorStats:
      return ArUiCatalog_Text(locale,"overlay.region.actor_stats_label","Enemy stats");
    default: return "";
  }
}

const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale, ActRaiserRegionalEditResult result) {
  switch (result) {
    case kActRaiserRegionalEdit_Applied:
    case kActRaiserRegionalEdit_Unchanged:
      return ArUiCatalog_Text(locale, "overlay.region.saved_with_story", NULL);
    case kActRaiserRegionalEdit_Locked:
      return ArUiCatalog_Text(locale, "overlay.region.replay_locked", NULL);
    case kActRaiserRegionalEdit_Stale:
      return ArUiCatalog_Text(locale, "overlay.region.stale", NULL);
    case kActRaiserRegionalEdit_HistoryUnavailable:
      return ArUiCatalog_Text(locale, "overlay.region.lair_unavailable", NULL);
    case kActRaiserRegionalEdit_Deferred:
      return ArUiCatalog_Text(locale,"overlay.region.population_pending",NULL);
    case kActRaiserRegionalEdit_Incompatible:
      return ArUiCatalog_Text(locale,"overlay.region.population_incompatible",NULL);
    default: return ArUiCatalog_Text(locale, "overlay.status.unavailable", NULL);
  }
}

static SettingsOverlayRegionBadge SourceBadge(ArRegionalSource source) {
  switch (source) {
    case kArRegionalSource_US: return kOverlayRegionBadge_US;
    case kArRegionalSource_Japan: return kOverlayRegionBadge_Japan;
    case kArRegionalSource_Europe: return kOverlayRegionBadge_Europe;
    default: return kOverlayRegionBadge_Mixed;
  }
}

bool SettingsOverlayRegions_ViewBadge(const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, bool effective, SettingsOverlayRegionBadge *badge) {
  if (!view || !badge || (unsigned)group >= kActRaiserRegionalSetting_Count) return false;
  if(group==kActRaiserRegionalSetting_ActorStats) {
    const ArRegionalActorStatsPolicy *policy=effective?&view->effective.actor_stats:&view->requested.actor_stats;
    ArRegionalActorStatsSnapshot snapshot;ArRegionalSource source;
    if(!ArRegionalActorStats_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalActorStats_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_PlatformSkull) {
    const ArRegionalPlatformSkullPolicy *policy=effective?&view->effective.platform_skull:&view->requested.platform_skull;
    uint8_t snapshot;ArRegionalSource source;
    if(!ArRegionalPlatformSkull_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalPlatformSkull_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_Collision) {
    const ArRegionalCollisionPolicy *policy=effective?&view->effective.collision:&view->requested.collision;
    uint8_t snapshot;ArRegionalSource source;
    if(!ArRegionalCollision_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalCollision_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_Bosses) {
    const ArRegionalBossPolicy *policy=effective?&view->effective.bosses:&view->requested.bosses;
    uint64_t snapshot;ArRegionalSource source;
    if(!ArRegionalBoss_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalBoss_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_StatueVolley) {
    const ArRegionalSource source=effective?view->effective.statue_volley:view->requested.statue_volley;
    bool unused;if(!ArRegionalVolley_Resolve(source,&unused))return false;
    *badge=SourceBadge(source);return true;
  }
  if(group==kActRaiserRegionalSetting_Emitters) {
    const ArRegionalEmitterPolicy *policy=effective?&view->effective.emitters:&view->requested.emitters;
    uint8_t snapshot;ArRegionalSource source;
    if(!ArRegionalEmitter_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalEmitter_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_ActionMotion) {
    const ArRegionalActionMotionPolicy *policy=effective?&view->effective.action_motion:&view->requested.action_motion;
    uint16_t snapshot;ArRegionalSource source;
    if(!ArRegionalActionMotion_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalActionMotion_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_Population) {
    if(!effective && view->population_pending) {*badge=SourceBadge(view->pending_population);return true;}
    const ArRegionalSupportPolicy *policy=effective?&view->effective.support:&view->requested.support;
    ArRegionalSupportSnapshot snapshot;ArRegionalSource source;
    if(!ArRegionalSupport_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalSupport_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_Arrival) {
    const ArRegionalSource source=effective?view->effective.arrival:view->requested.arrival;
    bool unused;if(!ArRegionalArrival_Resolve(source,&unused))return false;
    *badge=SourceBadge(source);return true;
  }
  if (group==kActRaiserRegionalSetting_Construction) {
    const ArRegionalSource source=effective?view->effective.construction:view->requested.construction;
    bool unused;
    if (!ArRegionalConstruction_Resolve(source,&unused)) return false;
    *badge=SourceBadge(source);return true;
  }
  if (group==kActRaiserRegionalSetting_SimAi) {
    const ArRegionalSimAiPolicy *policy=effective?&view->effective.sim_ai:&view->requested.sim_ai;
    uint16_t snapshot;ArRegionalSource source;
    if (!ArRegionalSimAi_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalSimAi_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;return true;
  }
  if (group==kActRaiserRegionalSetting_SimCombat) {
    const ArRegionalSimCombatPolicy *policy=effective?&view->effective.sim_combat:&view->requested.sim_combat;
    uint16_t snapshot;ArRegionalSource source;
    if (!ArRegionalSimCombat_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalSimCombat_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;return true;
  }
  if (group==kActRaiserRegionalSetting_LevelGoals) {
    const ArRegionalSource source=effective?view->effective.level_goals:view->requested.level_goals;
    bool unused;
    if (!ArRegionalLevelGoals_Resolve(source,&unused)) return false;
    *badge=SourceBadge(source);return true;
  }
  if (group==kActRaiserRegionalSetting_TownStatus) {
    const ArRegionalTownStatusPolicy *policy=effective?&view->effective.town_status:&view->requested.town_status;
    ArRegionalTownStatusSnapshot snapshot; ArRegionalSource source;
    if (!ArRegionalTownStatus_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalTownStatus_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group==kActRaiserRegionalSetting_LairReloads) {
    const ArRegionalSource source=effective?view->effective.lair_reloads:view->requested.lair_reloads;
    if ((unsigned)source>=kArRegionalSource_Count) return false;
    *badge=SourceBadge(source); return true;
  }
  if (group == kActRaiserRegionalSetting_Story) {
    const ArRegionalStoryPolicy *policy=effective?&view->effective.story:&view->requested.story;
    ArRegionalStorySnapshot snapshot; ArRegionalSource source;
    if (!ArRegionalStory_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalStory_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group == kActRaiserRegionalSetting_SkullWait) {
    const ArRegionalSource source=effective?view->effective.skull_wait:view->requested.skull_wait;
    uint16_t unused;
    if (!ArRegionalSkullWait_Resolve(source,&unused)) return false;
    *badge=SourceBadge(source); return true;
  }
  if (group == kActRaiserRegionalSetting_Sources) {
    const ArRegionalSourcesPolicy *policy=effective?&view->effective.sources:&view->requested.sources;
    ArRegionalSourcesSnapshot snapshot; ArRegionalSource source;
    if (!ArRegionalSources_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalSources_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group == kActRaiserRegionalSetting_LivesDisplay) {
    const ArRegionalSource source = effective ? view->effective.lives_display : view->requested.lives_display;
    bool unused;
    if (!ArRegionalLivesDisplay_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source); return true;
  }
  if (group == kActRaiserRegionalSetting_ScoreFeedback) {
    const ArRegionalScorePolicy *policy=effective?&view->effective.score_feedback:&view->requested.score_feedback;
    ArRegionalScoreSnapshot snapshot; ArRegionalSource source;
    if (!ArRegionalScore_Resolve(policy,&snapshot)) return false;
    *badge=ArRegionalScore_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group == kActRaiserRegionalSetting_HouseCredit) {
    const ArRegionalSource source = effective ? view->effective.house_credit : view->requested.house_credit;
    if ((unsigned)source >= kArRegionalSource_Count) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_LairReserves) {
    const ArRegionalSource source = effective ? view->effective.lair_seeds : view->requested.lair_seeds;
    if ((unsigned)source >= kArRegionalSource_Count) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_MagicGesture) {
    const ArRegionalSource source = effective ? view->effective.magic_gesture : view->requested.magic_gesture;
    bool unused;
    if (!ArRegionalMagicGesture_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_SpeedRange) {
    const ArRegionalSource source = effective ? view->effective.speed_range : view->requested.speed_range;
    uint16_t unused;
    if (!ArRegionalSpeedRange_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_MenuReturn) {
    const ArRegionalSource source = effective ? view->effective.menu_return : view->requested.menu_return;
    bool unused;
    if (!ArRegionalMenuReturn_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_ScorePage) {
    const ArRegionalSource source = effective ? view->effective.score_page : view->requested.score_page;
    bool unused;
    if (!ArRegionalScorePage_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source); return true;
  }
  if (group == kActRaiserRegionalSetting_Quake) {
    const ArRegionalQuakePolicy *policy = effective ? &view->effective.quake : &view->requested.quake;
    ArRegionalQuakeSnapshot snapshot; ArRegionalSource source;
    if (!ArRegionalQuake_Resolve(policy, &snapshot)) return false;
    *badge = ArRegionalQuake_GroupSource(policy, &source) ? SourceBadge(source) : kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group == kActRaiserRegionalSetting_Recovery) {
    const ArRegionalRecoveryPolicy *policy = effective ? &view->effective.recovery : &view->requested.recovery;
    ArRegionalRecoverySnapshot snapshot;
    ArRegionalSource source;
    if (!ArRegionalRecovery_Resolve(policy, &snapshot)) return false;
    *badge = ArRegionalRecovery_GroupSource(policy, &source) ? SourceBadge(source) : kOverlayRegionBadge_Mixed;
    return true;
  }
  if(group==kActRaiserRegionalSetting_Development) {
    const ArRegionalDevelopmentPolicy *policy=effective?&view->effective.development:&view->requested.development;
    ArRegionalDevelopmentSnapshot snapshot;ArRegionalSource source;
    if(!ArRegionalDevelopment_Resolve(policy,&snapshot))return false;
    *badge=ArRegionalDevelopment_GroupSource(policy,&source)?SourceBadge(source):kOverlayRegionBadge_Mixed;
    return true;
  }
  if (group == kActRaiserRegionalSetting_Fishing) {
    const ArRegionalSource source = effective ? view->effective.fishing : view->requested.fishing;
    uint16_t unused;
    if (!ArRegionalFishing_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_TownWait) {
    const ArRegionalSource source = effective ? view->effective.town_wait : view->requested.town_wait;
    uint16_t unused;
    if (!ArRegionalTownWait_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_RetryScore) {
    const ArRegionalSource source = effective ? view->effective.retry_score : view->requested.retry_score;
    bool unused;
    if (!ArRegionalRetry_Resolve(source, &unused)) return false;
    *badge = SourceBadge(source);
    return true;
  }
  if (group == kActRaiserRegionalSetting_Scrolls || group == kActRaiserRegionalSetting_Miracles)
    return SettingsOverlayRegions_CostBadge(effective ? &view->effective.costs : &view->requested.costs,
        group == kActRaiserRegionalSetting_Scrolls ? kArRegionalCostGroup_Scrolls : kArRegionalCostGroup_Miracles,
        badge);
  if (group != kActRaiserRegionalSetting_RoomTimes) return false;
  const ArRegionalTimerPolicy *policy = effective ? &view->effective.timers : &view->requested.timers;
  if (!ArRegionalTimers_Valid(policy)) return false;
  ArRegionalSource source;
  *badge = ArRegionalTimers_GroupSource(policy, &source) ? SourceBadge(source) : kOverlayRegionBadge_Mixed;
  return true;
}

bool SettingsOverlayRegions_NextSource(const ActRaiserRegionalRulesView *view,
                                      ActRaiserRegionalSettingGroup group, int direction,
                                      ArRegionalSource *source) {
  SettingsOverlayRegionBadge badge;
  if (!source || !SettingsOverlayRegions_ViewBadge(view, group, false, &badge)) return false;
  ArRegionalSource current;
  switch (badge) {
    case kOverlayRegionBadge_US: current = kArRegionalSource_US; break;
    case kOverlayRegionBadge_Japan: current = kArRegionalSource_Japan; break;
    case kOverlayRegionBadge_Europe: current = kArRegionalSource_Europe; break;
    default: current = kArRegionalSource_Count; break;
  }
  if (current == kArRegionalSource_Count) {
    *source = direction < 0 ? kArRegionalSource_Europe : kArRegionalSource_US;
  } else {
    *source = (ArRegionalSource)((current +
        (direction < 0 ? kArRegionalSource_Count - 1 : 1)) % kArRegionalSource_Count);
  }
  return true;
}

bool SettingsOverlayRegions_CostBadge(const ArRegionalCostPolicy *policy,
                                     ArRegionalCostGroup group,
                                     SettingsOverlayRegionBadge *badge) {
  ArRegionalCostSnapshot snapshot;
  if (!badge || (unsigned)group >= kArRegionalCostGroup_Count ||
      !ArRegionalCosts_Resolve(policy, &snapshot)) return false;
  ArRegionalSource source;
  if (!ArRegionalCosts_GroupSource(policy, group, &source)) {
    *badge = kOverlayRegionBadge_Mixed;
    return true;
  }
  *badge = SourceBadge(source);
  return true;
}

const char *SettingsOverlayRegions_BadgeLabel(ArUiLocale locale,
                                             SettingsOverlayRegionBadge badge) {
  switch (badge) {
    case kOverlayRegionBadge_US:
      return ArUiCatalog_Text(locale, "overlay.region.us", "US");
    case kOverlayRegionBadge_Japan:
      return ArUiCatalog_Text(locale, "overlay.region.japan", "Japan");
    case kOverlayRegionBadge_Europe:
      return ArUiCatalog_Text(locale, "overlay.region.europe", "Europe");
    case kOverlayRegionBadge_Mixed:
      return ArUiCatalog_Text(locale, "overlay.region.custom", "Custom");
    default: return "";
  }
}

bool SettingsOverlayRegions_CostDescription(ArUiLocale locale,
                                           const ArRegionalCostPolicy *policy,
                                           ArRegionalCostGroup group,
                                           char *output, size_t capacity) {
  ArRegionalCostSnapshot snapshot;
  SettingsOverlayRegionBadge badge;
  if (!ArRegionalCosts_Resolve(policy, &snapshot) ||
      !SettingsOverlayRegions_CostBadge(policy, group, &badge)) return false;

  char values[kArRegionalCostRule_Count][8];
  ArUiTextArgument args[kArRegionalCostRule_Count + 1];
  size_t count = 0;
  args[count++] = (ArUiTextArgument){"region", SettingsOverlayRegions_BadgeLabel(locale, badge)};
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    if (desc->group != group) continue;
    snprintf(values[i], sizeof(values[i]), "%u", (unsigned)snapshot.price[i]);
    /* Stable semantic keys are also named template arguments. */
    args[count++] = (ArUiTextArgument){desc->key, values[i]};
  }
  const char *key = group == kArRegionalCostGroup_Scrolls
      ? "overlay.region.scroll_prices" : "overlay.region.miracle_prices";
  const char *message = ArUiCatalog_Text(locale, key, NULL);
  return message[0] && ArUiCatalog_Format(output, capacity, message, args, count);
}

bool SettingsOverlayRegions_ViewDescription(ArUiLocale locale,
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    char *output, size_t capacity) {
  SettingsOverlayRegionBadge badge;
  if (!SettingsOverlayRegions_ViewBadge(view, group, false, &badge)) return false;
  if(group==kActRaiserRegionalSetting_ActorStats) {
    ArRegionalActorStatsSnapshot snapshot;
    if(!ArRegionalActorStats_Resolve(&view->requested.actor_stats,&snapshot))return false;
    char hp[8],reward[8],attack[8];
    snprintf(hp,sizeof(hp),"%u",snapshot.value[kArRegionalActorStat_TanzraMinionHp]);
    snprintf(reward,sizeof(reward),"%u",10u*snapshot.value[kArRegionalActorStat_TanzraMinionReward]);
    snprintf(attack,sizeof(attack),"%u",snapshot.value[kArRegionalActorStat_TanzraProjectileAttack]);
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"hp",hp},{"reward",reward},{"attack",attack}};
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.actor_stats",NULL),args,4);
  }
  if(group==kActRaiserRegionalSetting_PlatformSkull) {
    uint8_t snapshot;
    if(!ArRegionalPlatformSkull_Resolve(&view->requested.platform_skull,&snapshot))return false;
    const ArUiTextArgument args[]={
      {"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"reward",snapshot&2?"0":"200"},{"x",snapshot&4?"24":"32"},{"y",snapshot&8?"24":"64"}};
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,snapshot&1?
        "overlay.region.platform_skull_armored":"overlay.region.platform_skull_open",NULL),args,4);
  }
  if(group==kActRaiserRegionalSetting_Collision) {
    uint8_t snapshot;
    if(!ArRegionalCollision_Resolve(&view->requested.collision,&snapshot))return false;
    const ArUiTextArgument args[]={
      {"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"sword",snapshot&1?"36":"28"},{"arrow",snapshot&2?"16":"8"}};
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.collision",NULL),args,3);
  }
  if(group==kActRaiserRegionalSetting_Bosses) {
    static const char *names[]={"idle","throw_end","throw","jump","offset","wizard","ice","closing","clock","turn","minion"};
    _Static_assert(sizeof(names)/sizeof(names[0])==kArRegionalBoss_Count,"name every boss rule");
    char values[kArRegionalBoss_Count][8];ArUiTextArgument args[kArRegionalBoss_Count+1];
    args[0]=(ArUiTextArgument){"region",SettingsOverlayRegions_BadgeLabel(locale,badge)};
    for(unsigned i=0;i<kArRegionalBoss_Count;++i) {
      const ArRegionalBossDescriptor *desc=ArRegionalBoss_Descriptor(i);
      snprintf(values[i],sizeof(values[i]),"%u",desc->value[view->requested.bosses.source[i]]+desc->phase_extra_updates);
      args[i+1]=(ArUiTextArgument){names[i],values[i]};
    }
    args[kArRegionalBoss_TanzraClock+1].value=ArUiCatalog_Text(locale,
        view->requested.bosses.source[kArRegionalBoss_TanzraClock]==kArRegionalSource_Japan?
        "overlay.region.clock_stopped":"overlay.region.clock_running",NULL);
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.bosses",NULL),args,kArRegionalBoss_Count+1);
  }
  if(group==kActRaiserRegionalSetting_StatueVolley) {
    bool double_shot;
    if(!ArRegionalVolley_Resolve(view->requested.statue_volley,&double_shot))return false;
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"shots",double_shot?"2":"1"},{"cycle",double_shot?"153":"137"}};
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.statue_volley",NULL),args,3);
  }
  if(group==kActRaiserRegionalSetting_Emitters) {
    char interval[8];snprintf(interval,sizeof(interval),"%u",ArRegionalEmitter_Descriptor(0)->value[view->requested.emitters.source[0]]);
    const bool pal=ArRegionalEmitter_Descriptor(1)->value[view->requested.emitters.source[1]]!=0;
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"interval",interval},{"left",pal?"+6":"-8"},{"right",pal?"-22":"-8"}};
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.emitters",NULL),args,4);
  }
  if(group==kActRaiserRegionalSetting_ActionMotion) {
    static const char *names[]={"bird","leaper","cave","straight","high","low_cast","high_cast","sword","high_sword","arrow","short_head","long_head"};
    _Static_assert(sizeof(names)/sizeof(names[0])==kArRegionalActionMotion_Count,"name every motion value");
    char numbers[kArRegionalActionMotion_Count][8];ArUiTextArgument args[kArRegionalActionMotion_Count+1];
    args[0]=(ArUiTextArgument){"region",SettingsOverlayRegions_BadgeLabel(locale,badge)};
    for(unsigned i=0;i<kArRegionalActionMotion_Count;++i) {
      const ArRegionalActionMotionDescriptor *desc=ArRegionalActionMotion_Descriptor((ArRegionalActionMotionRule)i);
      snprintf(numbers[i],sizeof(numbers[i]),"%u",desc->value[view->requested.action_motion.source[i]]+desc->phase_extra_updates);
      args[i+1]=(ArUiTextArgument){names[i],numbers[i]};
    }
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,"overlay.region.action_motion",NULL),args,kArRegionalActionMotion_Count+1);
  }
  if(group==kActRaiserRegionalSetting_Arrival) {
    SettingsOverlayRegionBadge active;
    if(!SettingsOverlayRegions_ViewBadge(view,group,true,&active))return false;
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
        {"active",SettingsOverlayRegions_BadgeLabel(locale,active)}};
    const char *key=view->arrival_locked?"overlay.region.arrival_decided":
        badge==kOverlayRegionBadge_Japan?"overlay.region.arrival_jp":"overlay.region.arrival_us";
    return ArUiCatalog_Format(output,capacity,ArUiCatalog_Text(locale,key,NULL),args,2);
  }
  if (group==kActRaiserRegionalSetting_Population) {
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,badge==kOverlayRegionBadge_Japan?
        "overlay.region.population_jp":"overlay.region.population_us",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_Construction) {
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,badge==kOverlayRegionBadge_Japan?
        "overlay.region.construction_jp":"overlay.region.construction_us",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_SimAi) {
    const char *key=badge==kOverlayRegionBadge_Mixed?"overlay.region.sim_ai_mixed":
        badge==kOverlayRegionBadge_Japan?"overlay.region.sim_ai_jp":"overlay.region.sim_ai_us";
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,key,NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_SimCombat) {
    const char *key=badge==kOverlayRegionBadge_Mixed?"overlay.region.sim_combat_mixed":
        badge==kOverlayRegionBadge_Japan?"overlay.region.sim_combat_jp":"overlay.region.sim_combat_us";
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,key,NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_LevelGoals) {
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,badge==kOverlayRegionBadge_Japan?
        "overlay.region.level_jp":"overlay.region.level_us",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_TownStatus) {
    const char *key=badge==kOverlayRegionBadge_Mixed?"overlay.region.town_status_mixed":
        badge==kOverlayRegionBadge_Japan?"overlay.region.town_status_jp":"overlay.region.town_status_us";
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,key,NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group==kActRaiserRegionalSetting_LairReloads) {
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}};
    const char *message=ArUiCatalog_Text(locale,!view->lair_reload_ready?"overlay.region.lair_unavailable":
        view->requested.lair_reloads==kArRegionalSource_Japan?"overlay.region.reload_jp":"overlay.region.reload_us",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,1);
  }
  if (group == kActRaiserRegionalSetting_ScoreFeedback) {
    ArRegionalScoreSnapshot snapshot;
    if (!ArRegionalScore_Resolve(&view->requested.score_feedback,&snapshot)) return false;
    const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"conversion", ArUiCatalog_Text(locale,snapshot.japanese[kArRegionalScore_Conversion] ?
          "overlay.region.score_conversion_jp" : "overlay.region.score_conversion_us",NULL)},
      {"operation", ArUiCatalog_Text(locale,snapshot.japanese[kArRegionalScore_Operation] ?
          "overlay.region.score_operation_jp" : "overlay.region.score_operation_us",NULL)},
      {"route", ArUiCatalog_Text(locale,snapshot.japanese[kArRegionalScore_Route] ?
          "overlay.region.score_route_jp" : "overlay.region.score_route_us",NULL)},
      {"phase", ArUiCatalog_Text(locale,snapshot.japanese[kArRegionalScore_Phase] ?
          "overlay.region.score_phase_jp" : "overlay.region.score_phase_us",NULL)},
    };
    const char *message=ArUiCatalog_Text(locale,view->lair_history_ready ?
        "overlay.region.score_feedback_help" : "overlay.region.lair_unavailable",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,5);
  }
  if (group == kActRaiserRegionalSetting_LivesDisplay) {
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale, view->requested.lives_display == kArRegionalSource_Japan ?
        "overlay.region.lives_jp" : "overlay.region.lives_us", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_SkullWait) {
    uint16_t frames;
    if (!ArRegionalSkullWait_Resolve(view->requested.skull_wait,&frames)) return false;
    char value[4]; snprintf(value,sizeof(value),"%u",frames);
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},{"frames",value}};
    const char *message=ArUiCatalog_Text(locale,"overlay.region.skull_help",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,2);
  }
  if (group == kActRaiserRegionalSetting_Story) {
    ArRegionalStorySnapshot snapshot;
    if (!ArRegionalStory_Resolve(&view->requested.story,&snapshot)) return false;
    char hint[8],tablet[8];
    snprintf(hint,sizeof(hint),"%u",snapshot.value[kArRegionalStory_FillmoreHint]);
    snprintf(tablet,sizeof(tablet),"%u",snapshot.value[kArRegionalStory_KasandoraTablet]);
    const ArUiTextArgument args[]={
      {"region",SettingsOverlayRegions_BadgeLabel(locale,badge)}, {"hint",hint}, {"tablet",tablet},
      {"compass",ArUiCatalog_Text(locale,snapshot.value[kArRegionalStory_ClearCompassPrerequisite] ?
          "overlay.region.story_clear":"overlay.region.story_keep",NULL)},
    };
    const char *message=ArUiCatalog_Text(locale,"overlay.region.story_help",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,4);
  }
  if (group == kActRaiserRegionalSetting_Sources) {
    ArRegionalSourcesSnapshot snapshot;
    if (!ArRegionalSources_Resolve(&view->requested.sources,&snapshot)) return false;
    const ArUiTextArgument args[] = {
      {"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"life",ArUiCatalog_Text(locale,snapshot.automatic[kArRegionalSourceItem_Life] ?
          "overlay.region.sources_auto":"overlay.region.sources_manual",NULL)},
      {"magic",ArUiCatalog_Text(locale,snapshot.automatic[kArRegionalSourceItem_Magic] ?
          "overlay.region.sources_auto":"overlay.region.sources_manual",NULL)},
    };
    const char *message=ArUiCatalog_Text(locale,"overlay.region.sources_help",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,3);
  }
  if (group == kActRaiserRegionalSetting_HouseCredit) {
    const char *key = !view->lair_history_ready ? "overlay.region.lair_unavailable" :
        view->requested.house_credit == kArRegionalSource_Japan ? "overlay.region.house_jp" : "overlay.region.house_us";
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale, key, NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_LairReserves) {
    const char *key = !view->lair_history_ready ? "overlay.region.lair_unavailable" :
        view->requested.lair_seeds == kArRegionalSource_Japan ?
        "overlay.region.lair_jp" : "overlay.region.lair_us";
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale, key, NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_MagicGesture) {
    bool up_attack;
    if (!ArRegionalMagicGesture_Resolve(view->requested.magic_gesture, &up_attack)) return false;
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale,
        up_attack ? "overlay.region.magic_gesture_jp" : "overlay.region.magic_gesture_us", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_SpeedRange) {
    uint16_t maximum;
    if (!ArRegionalSpeedRange_Resolve(view->requested.speed_range, &maximum)) return false;
    char value[4]; snprintf(value, sizeof(value), "%u", maximum);
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}, {"maximum", value}};
    const char *message = ArUiCatalog_Text(locale, "overlay.region.speed_range", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 2);
  }
  if (group == kActRaiserRegionalSetting_MenuReturn) {
    bool keep_open;
    if (!ArRegionalMenuReturn_Resolve(view->requested.menu_return, &keep_open)) return false;
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale,
        keep_open ? "overlay.region.menu_return_keep" : "overlay.region.menu_return_close", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_ScorePage) {
    bool enabled;
    if (!ArRegionalScorePage_Resolve(view->requested.score_page, &enabled)) return false;
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    const char *message = ArUiCatalog_Text(locale,
        enabled ? "overlay.region.score_page_show" : "overlay.region.score_page_hide", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_Quake) {
    const char *key = badge == kOverlayRegionBadge_Japan ? "overlay.region.quake_jp" :
        badge == kOverlayRegionBadge_Mixed ? "overlay.region.quake_mixed" : "overlay.region.quake_us";
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)},
      {"policy", ArUiCatalog_Text(locale, key, NULL)}};
    const char *message = ArUiCatalog_Text(locale, "overlay.region.quake", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 2);
  }
  if (group == kActRaiserRegionalSetting_Recovery) {
    ArRegionalRecoverySnapshot snapshot;
    if (!ArRegionalRecovery_Resolve(&view->requested.recovery, &snapshot)) return false;
    const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale, badge)},
      {"sp", ArUiCatalog_Text(locale, snapshot.cycle_sp ? "overlay.region.recovery_sp_cycle" : "overlay.region.recovery_sp_none", NULL)},
      {"hp", ArUiCatalog_Text(locale, snapshot.angel_calls ? "overlay.region.recovery_hp_calls" : "overlay.region.recovery_hp_cycle", NULL)}};
    const char *message = ArUiCatalog_Text(locale, "overlay.region.recovery", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 3);
  }
  if(group==kActRaiserRegionalSetting_Development) {
    ArRegionalDevelopmentSnapshot snapshot;
    if(!ArRegionalDevelopment_Resolve(&view->requested.development,&snapshot))return false;
    char divider[8],cycle[8],effects[8];
    snprintf(divider,sizeof(divider),"%u",snapshot.service_divider);
    snprintf(cycle,sizeof(cycle),"%u",snapshot.long_cycle);
    snprintf(effects,sizeof(effects),"%u",snapshot.effect_divider);
    const ArUiTextArgument args[]={{"region",SettingsOverlayRegions_BadgeLabel(locale,badge)},
      {"divider",divider},{"cycle",cycle},{"effects",effects}};
    const char *message=ArUiCatalog_Text(locale,"overlay.region.development",NULL);
    return message[0] && ArUiCatalog_Format(output,capacity,message,args,4);
  }
  if (group == kActRaiserRegionalSetting_Fishing) {
    uint16_t updates;
    if (!ArRegionalFishing_Resolve(view->requested.fishing, &updates)) return false;
    char value[8]; snprintf(value, sizeof(value), "%u", updates);
    const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}, {"updates", value}};
    const char *message = ArUiCatalog_Text(locale, "overlay.region.fishing", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 2);
  }
  if (group == kActRaiserRegionalSetting_TownWait) {
    uint16_t updates;
    if (!ArRegionalTownWait_Resolve(view->requested.town_wait, &updates)) return false;
    char value[8];
    snprintf(value, sizeof(value), "%u", updates);
    const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}, {"updates", value}};
    const char *message = ArUiCatalog_Text(locale, "overlay.region.town_wait", NULL);
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 2);
  }
  if (group == kActRaiserRegionalSetting_RetryScore) {
    bool clear;
    if (!ArRegionalRetry_Resolve(view->requested.retry_score, &clear)) return false;
    const char *message = ArUiCatalog_Text(locale, clear ? "overlay.region.retry_clear" : "overlay.region.retry_keep", NULL);
    const ArUiTextArgument args[] = {{"region", SettingsOverlayRegions_BadgeLabel(locale, badge)}};
    return message[0] && ArUiCatalog_Format(output, capacity, message, args, 1);
  }
  if (group == kActRaiserRegionalSetting_Scrolls || group == kActRaiserRegionalSetting_Miracles)
    return SettingsOverlayRegions_CostDescription(locale, &view->requested.costs,
        group == kActRaiserRegionalSetting_Scrolls ? kArRegionalCostGroup_Scrolls : kArRegionalCostGroup_Miracles,
        output, capacity);
  if (group != kActRaiserRegionalSetting_RoomTimes) return false;
  char values[kArRegionalTimerRule_Count][8];
  ArUiTextArgument args[kArRegionalTimerRule_Count + 1];
  args[0] = (ArUiTextArgument){"region", SettingsOverlayRegions_BadgeLabel(locale, badge)};
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) {
    const ArRegionalTimerDescriptor *rule = ArRegionalTimers_Descriptor((ArRegionalTimerRule)i);
    /* BCD digits are already the displayed decimal digits. */
    snprintf(values[i], sizeof(values[i]), "%X", rule->bcd[view->requested.timers.source[i]]);
    args[i + 1] = (ArUiTextArgument){rule->key, values[i]};
  }
  const char *message = ArUiCatalog_Text(locale, "overlay.region.room_times", NULL);
  return message[0] && ArUiCatalog_Format(output, capacity, message, args, kArRegionalTimerRule_Count + 1);
}
