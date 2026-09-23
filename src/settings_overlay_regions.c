#include "settings_overlay_regions.h"

#include <stdio.h>

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
