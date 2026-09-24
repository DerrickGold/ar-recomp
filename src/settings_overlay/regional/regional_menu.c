#include "settings_overlay/regional/regional_menu.h"

#include <stdio.h>

/* Each visible row binds its catalog text to a narrow game-owned setting.
 * Only Presets expand profiles from regional/regional_profiles.c. */
static const OverlayRegionRow kPresets[] = {
    {
        /* Gameplay rules */
        .key = "regional_profile_gameplay",
        .label_key = "overlay.region.menu.regional_profile_gameplay.label",
        .help_key = "overlay.region.menu.regional_profile_gameplay.help",
        .kind = kOverlayRegionRow_Preset,
        .group = kArRegionalProfile_Gameplay,
    },
    {
        /* Presentation */
        .key = "regional_profile_presentation",
        .label_key = "overlay.region.menu.regional_profile_presentation.label",
        .help_key = "overlay.region.menu.regional_profile_presentation.help",
        .kind = kOverlayRegionRow_Preset,
        .group = kArRegionalProfile_Presentation,
    },
};
static const OverlayRegionRow kAction[] = {
    { .key = "regional_difficulty_level", .label_key = "overlay.region.menu.regional_difficulty_level.label",
      .help_key = "overlay.region.menu.regional_difficulty_level.help",
      .kind = kOverlayRegionRow_Difficulty, .group = kArRegionalProfile_Difficulty },
    { .key = "regional_terrain",
      .label_key = "overlay.region.menu.regional_terrain.label",
      .help_key = "overlay.region.menu.regional_terrain.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Stage,
      .setting = kActRaiserRegionalSetting_Terrain },
    { .key = "regional_enemy_placements",
      .label_key = "overlay.region.menu.regional_enemy_placements.label",
      .help_key = "overlay.region.menu.regional_enemy_placements.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Stage,
      .setting = kActRaiserRegionalSetting_EnemyPlacements },
    { .key = "regional_pickup_placements",
      .label_key = "overlay.region.menu.regional_pickup_placements.label",
      .help_key = "overlay.region.menu.regional_pickup_placements.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Stage,
      .setting = kActRaiserRegionalSetting_PickupPlacements },
    { .key = "regional_room_times",
      .label_key = "overlay.region.menu.regional_room_times.label",
      .help_key = "overlay.region.menu.regional_room_times.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Stage,
      .setting = kActRaiserRegionalSetting_RoomTimes },
    { .key = "regional_hazards",
      .label_key = "overlay.region.menu.regional_hazards.label",
      .help_key = "overlay.region.menu.regional_hazards.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_Hazards },
    { .key = "regional_actor_stats",
      .label_key = "overlay.region.menu.regional_actor_stats.label",
      .help_key = "overlay.region.menu.regional_actor_stats.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_ActorStats },
    { .key = "regional_action_motion",
      .label_key = "overlay.region.menu.regional_action_motion.label",
      .help_key = "overlay.region.menu.regional_action_motion.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_ActionMotion },
    { .key = "regional_collision",
      .label_key = "overlay.region.menu.regional_collision.label",
      .help_key = "overlay.region.menu.regional_collision.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_Collision },
    { .key = "regional_emitters",
      .label_key = "overlay.region.menu.regional_emitters.label",
      .help_key = "overlay.region.menu.regional_emitters.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_Emitters },
    { .key = "regional_statue_volley",
      .label_key = "overlay.region.menu.regional_statue_volley.label",
      .help_key = "overlay.region.menu.regional_statue_volley.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_StatueVolley },
    { .key = "regional_bosses",
      .label_key = "overlay.region.menu.regional_bosses.label",
      .help_key = "overlay.region.menu.regional_bosses.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_Bosses },
    { .key = "regional_platform_skull",
      .label_key = "overlay.region.menu.regional_platform_skull.label",
      .help_key = "overlay.region.menu.regional_platform_skull.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_PlatformSkull },
    { .key = "regional_fire_enemy",
      .label_key = "overlay.region.menu.regional_fire_enemy.label",
      .help_key = "overlay.region.menu.regional_fire_enemy.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Combat,
      .setting = kActRaiserRegionalSetting_FireEnemy },
    { .key = "regional_scrolls",
      .label_key = "overlay.region.menu.regional_scrolls.label",
      .help_key = "overlay.region.menu.regional_scrolls.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Magic,
      .setting = kActRaiserRegionalSetting_Scrolls },
    { .key = "regional_cast_hold",
      .label_key = "overlay.region.menu.regional_cast_hold.label",
      .help_key = "overlay.region.menu.regional_cast_hold.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Magic,
      .setting = kActRaiserRegionalSetting_CastHold },
    { .key = "regional_inventory",
      .label_key = "overlay.region.menu.regional_inventory.label",
      .help_key = "overlay.region.menu.regional_inventory.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Magic,
      .setting = kActRaiserRegionalSetting_Inventory },
    { .key = "regional_starting_health",
      .label_key = "overlay.region.menu.regional_starting_health.label",
      .help_key = "overlay.region.menu.regional_starting_health.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lives,
      .setting = kActRaiserRegionalSetting_StartingHealth },
    { .key = "regional_starting_lives",
      .label_key = "overlay.region.menu.regional_starting_lives.label",
      .help_key = "overlay.region.menu.regional_starting_lives.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lives,
      .setting = kActRaiserRegionalSetting_StartingLives },
    { .key = "regional_retry_score",
      .label_key = "overlay.region.menu.regional_retry_score.label",
      .help_key = "overlay.region.menu.regional_retry_score.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lives,
      .setting = kActRaiserRegionalSetting_RetryScore },
    { .key = "regional_score_lives",
      .label_key = "overlay.region.menu.regional_score_lives.label",
      .help_key = "overlay.region.menu.regional_score_lives.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lives,
      .setting = kActRaiserRegionalSetting_ScoreLives },
    { .key = "regional_mode_entry",
      .label_key = "overlay.region.menu.regional_mode_entry.label",
      .help_key = "overlay.region.menu.regional_mode_entry.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lives,
      .setting = kActRaiserRegionalSetting_ModeEntry },
};
static const OverlayRegionRow kTowns[] = {
    { .key = "regional_profile_population", .label_key = "overlay.region.menu.regional_profile_population.label",
      .help_key = "overlay.region.menu.regional_profile_population.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Population,
      .setting = kActRaiserRegionalSetting_Population },
    { .key = "regional_compass_return", .label_key = "overlay.region.menu.regional_compass_return.label",
      .help_key = "overlay.region.menu.regional_compass_return.help", .kind = kOverlayRegionRow_Setting,
      .group = kArRegionalProfile_Population, .setting = kActRaiserRegionalSetting_CompassReturn },
    { .key = "regional_construction",
      .label_key = "overlay.region.menu.regional_construction.label",
      .help_key = "overlay.region.menu.regional_construction.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Development,
      .setting = kActRaiserRegionalSetting_Construction },
    { .key = "regional_development",
      .label_key = "overlay.region.menu.regional_development.label",
      .help_key = "overlay.region.menu.regional_development.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Development,
      .setting = kActRaiserRegionalSetting_Development },
    { .key = "regional_town_wait",
      .label_key = "overlay.region.menu.regional_town_wait.label",
      .help_key = "overlay.region.menu.regional_town_wait.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Development,
      .setting = kActRaiserRegionalSetting_TownWait },
    { .key = "regional_fishing",
      .label_key = "overlay.region.menu.regional_fishing.label",
      .help_key = "overlay.region.menu.regional_fishing.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Development,
      .setting = kActRaiserRegionalSetting_Fishing },
    { .key = "regional_lair_reserves",
      .label_key = "overlay.region.menu.regional_lair_reserves.label",
      .help_key = "overlay.region.menu.regional_lair_reserves.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lairs,
      .setting = kActRaiserRegionalSetting_LairReserves },
    { .key = "regional_lair_reloads",
      .label_key = "overlay.region.menu.regional_lair_reloads.label",
      .help_key = "overlay.region.menu.regional_lair_reloads.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lairs,
      .setting = kActRaiserRegionalSetting_LairReloads },
    { .key = "regional_sim_combat",
      .label_key = "overlay.region.menu.regional_sim_combat.label",
      .help_key = "overlay.region.menu.regional_sim_combat.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lairs,
      .setting = kActRaiserRegionalSetting_SimCombat },
    { .key = "regional_sim_ai",
      .label_key = "overlay.region.menu.regional_sim_ai.label",
      .help_key = "overlay.region.menu.regional_sim_ai.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Lairs,
      .setting = kActRaiserRegionalSetting_SimAi },
    { .key = "regional_miracles",
      .label_key = "overlay.region.menu.regional_miracles.label",
      .help_key = "overlay.region.menu.regional_miracles.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_Miracles },
    { .key = "regional_sp_recovery",
      .label_key = "overlay.region.menu.regional_sp_recovery.label",
      .help_key = "overlay.region.menu.regional_sp_recovery.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_SpRecovery },
    { .key = "regional_angel_recovery",
      .label_key = "overlay.region.menu.regional_angel_recovery.label",
      .help_key = "overlay.region.menu.regional_angel_recovery.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_AngelRecovery },
    { .key = "regional_quake",
      .label_key = "overlay.region.menu.regional_quake.label",
      .help_key = "overlay.region.menu.regional_quake.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_Quake },
    { .key = "regional_house_credit",
      .label_key = "overlay.region.menu.regional_house_credit.label",
      .help_key = "overlay.region.menu.regional_house_credit.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_HouseCredit },
    { .key = "regional_score_feedback",
      .label_key = "overlay.region.menu.regional_score_feedback.label",
      .help_key = "overlay.region.menu.regional_score_feedback.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_ScoreFeedback },
    { .key = "regional_sources",
      .label_key = "overlay.region.menu.regional_sources.label",
      .help_key = "overlay.region.menu.regional_sources.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_Sources },
    { .key = "regional_skull_wait",
      .label_key = "overlay.region.menu.regional_skull_wait.label",
      .help_key = "overlay.region.menu.regional_skull_wait.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Resources,
      .setting = kActRaiserRegionalSetting_SkullWait },
    { .key = "regional_town_status",
      .label_key = "overlay.region.menu.regional_town_status.label",
      .help_key = "overlay.region.menu.regional_town_status.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Population,
      .setting = kActRaiserRegionalSetting_TownStatus },
    { .key = "regional_arrival",
      .label_key = "overlay.region.menu.regional_arrival.label",
      .help_key = "overlay.region.menu.regional_arrival.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Arrival,
      .setting = kActRaiserRegionalSetting_Arrival },
};
static const OverlayRegionRow kControls[] = {
    { .key = "regional_magic_gesture",
      .label_key = "overlay.region.menu.regional_magic_gesture.label",
      .help_key = "overlay.region.menu.regional_magic_gesture.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Interaction,
      .setting = kActRaiserRegionalSetting_MagicGesture, .binary = true },
    { .key = "regional_lives_display",
      .label_key = "overlay.region.menu.regional_lives_display.label",
      .help_key = "overlay.region.menu.regional_lives_display.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Interaction,
      .setting = kActRaiserRegionalSetting_LivesDisplay, .binary = true },
    { .key = "regional_score_page",
      .label_key = "overlay.region.menu.regional_score_page.label",
      .help_key = "overlay.region.menu.regional_score_page.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Interaction,
      .setting = kActRaiserRegionalSetting_ScorePage, .binary = true },
    { .key = "regional_menu_return",
      .label_key = "overlay.region.menu.regional_menu_return.label",
      .help_key = "overlay.region.menu.regional_menu_return.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Interaction,
      .setting = kActRaiserRegionalSetting_MenuReturn, .binary = true },
    { .key = "regional_speed_range",
      .label_key = "overlay.region.menu.regional_speed_range.label",
      .help_key = "overlay.region.menu.regional_speed_range.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Interaction,
      .setting = kActRaiserRegionalSetting_SpeedRange, .binary = true },
};
static const OverlayRegionRow kPresentation[] = {
    { .key = "regional_actor_art",
      .label_key = "overlay.region.menu.regional_actor_art.label",
      .help_key = "overlay.region.menu.regional_actor_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_ActorArt },
    { .key = "regional_death_heim_art",
      .label_key = "overlay.region.menu.regional_death_heim_art.label",
      .help_key = "overlay.region.menu.regional_death_heim_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_DeathHeimArt },
    { .key = "regional_action_item_art",
      .label_key = "overlay.region.menu.regional_action_item_art.label",
      .help_key = "overlay.region.menu.regional_action_item_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_ActionItemArt },
    { .key = "regional_follower_art",
      .label_key = "overlay.region.menu.regional_follower_art.label",
      .help_key = "overlay.region.menu.regional_follower_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_FollowerArt },
    { .key = "regional_lair_art",
      .label_key = "overlay.region.menu.regional_lair_art.label",
      .help_key = "overlay.region.menu.regional_lair_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_LairArt },
    { .key = "regional_pyramid_art",
      .label_key = "overlay.region.menu.regional_pyramid_art.label",
      .help_key = "overlay.region.menu.regional_pyramid_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_PyramidArt },
    { .key = "regional_title_art",
      .label_key = "overlay.region.menu.regional_title_art.label",
      .help_key = "overlay.region.menu.regional_title_art.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_TitleArt },
    { .key = "regional_aitos_poses",
      .label_key = "overlay.region.menu.regional_aitos_poses.label",
      .help_key = "overlay.region.menu.regional_aitos_poses.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_AitosPoses },
    { .key = "regional_mosaic",
      .label_key = "overlay.region.menu.regional_mosaic.label",
      .help_key = "overlay.region.menu.regional_mosaic.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Artwork,
      .setting = kActRaiserRegionalSetting_Mosaic },
    { .key = "regional_music",
      .label_key = "overlay.region.menu.regional_music.label",
      .help_key = "overlay.region.menu.regional_music.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Music,
      .setting = kActRaiserRegionalSetting_Music },
    { .key = "regional_sequences",
      .label_key = "overlay.region.menu.regional_sequences.label",
      .help_key = "overlay.region.menu.regional_sequences.help",
      .kind = kOverlayRegionRow_Setting, .group = kArRegionalProfile_Music,
      .setting = kActRaiserRegionalSetting_Sequences },
};
typedef struct Page {
  const OverlayRegionRow *rows;
  unsigned count;
} Page;
#define PAGE(rows) {rows, sizeof(rows) / sizeof(rows[0])}
static const Page kPages[] = {PAGE(kPresets), PAGE(kAction), PAGE(kTowns), PAGE(kControls),
                              PAGE(kPresentation)};
#undef PAGE
_Static_assert(sizeof(kPages) / sizeof(kPages[0]) == kOverlayRegionPage_Count,
               "regional menu pages");

unsigned OverlayRegionMenu_Count(OverlayRegionPage page) {
  return (unsigned)page < kOverlayRegionPage_Count ? kPages[page].count : 0;
}
const OverlayRegionRow *OverlayRegionMenu_Row(OverlayRegionPage page, unsigned row) {
  return row < OverlayRegionMenu_Count(page) ? &kPages[page].rows[row] : NULL;
}
const char *OverlayRegionMenu_Label(ArUiLocale locale, const OverlayRegionRow *row) {
  return row ? ArUiCatalog_Text(locale, row->label_key, NULL) : "";
}
const char *OverlayRegionMenu_ImpactLabel(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                          const OverlayRegionRow *row) {
  if (!row || !view) return "";
  const ArRegionalTownImpact impact = row->kind != kOverlayRegionRow_Difficulty &&
      !(row->kind == kOverlayRegionRow_Setting &&
        (row->setting == kActRaiserRegionalSetting_TownStatus || row->setting == kActRaiserRegionalSetting_CompassReturn))
                                          ? ArRegionalProfiles_TownImpact(row->group)
                                          : kArRegionalTownImpact_None;
  const char *key = impact == kArRegionalTownImpact_Redevelopment
                        ? "overlay.region.impact.redevelopment"
                    : impact == kArRegionalTownImpact_Future ? "overlay.region.impact.future"
                                                             : "overlay.region.impact.none";
  if (impact == kArRegionalTownImpact_Future && !view->new_game &&
      row->kind == kOverlayRegionRow_Setting &&
      (row->setting == kActRaiserRegionalSetting_LairReloads ? view->lair_reload_estimated : view->lair_history_estimated) &&
      (row->setting == kActRaiserRegionalSetting_LairReserves || row->setting == kActRaiserRegionalSetting_LairReloads ||
       row->setting == kActRaiserRegionalSetting_HouseCredit || row->setting == kActRaiserRegionalSetting_ScoreFeedback))
    key = "overlay.region.impact.estimated";
  return ArUiCatalog_Text(locale, key, NULL);
}
static uint16_t ConfirmationMask(const ActRaiserRegionalRulesView *view) {
  return !view->population_pending
             ? 0
             : ArRegionalProfiles_Mask(view->pending_profile ? view->pending_profile_group
                                                             : kArRegionalProfile_Population);
}
ArRegionalSource OverlayRegionMenu_Source(const ActRaiserRegionalRulesView *view,
                                          ArRegionalProfileGroup group, bool effective) {
  if (!view || (unsigned)group >= kArRegionalProfile_Count) return kArRegionalSource_Count;
  const uint16_t mask = ArRegionalProfiles_Mask(group);
  if (!effective && mask && (mask & ConfirmationMask(view)) == mask)
    return view->pending_population;
  return (effective ? view->active_profiles : view->profiles)[group].source;
}
bool OverlayRegionMenu_Pending(const ActRaiserRegionalRulesView *view,
                               ArRegionalProfileGroup group) {
  return view && !view->new_game &&
         ((view->pending_groups | ConfirmationMask(view)) & ArRegionalProfiles_Mask(group));
}
ArRegionalSource OverlayRegionMenu_RowSource(const ActRaiserRegionalRulesView *view,
    const OverlayRegionRow *row, bool effective) {
  if (!view || !row) return kArRegionalSource_Count;
  if (row->kind != kOverlayRegionRow_Setting)
    return OverlayRegionMenu_Source(view, row->group, effective);
  if (!effective && ((view->pending_profile && (ConfirmationMask(view) & ArRegionalProfiles_Mask(row->group))) ||
      (view->population_pending && row->setting == kActRaiserRegionalSetting_Population)))
    return view->pending_population;
  return effective ? view->choices[row->setting].active_source : view->choices[row->setting].source;
}
static bool RowPending(const ActRaiserRegionalRulesView *view, const OverlayRegionRow *row) {
  if (view->new_game) return false;
  if (row->kind != kOverlayRegionRow_Setting) return OverlayRegionMenu_Pending(view, row->group);
  return (view->pending_profile && (ConfirmationMask(view) & ArRegionalProfiles_Mask(row->group))) ||
      (view->population_pending && row->setting == kActRaiserRegionalSetting_Population) ||
      view->choices[row->setting].pending;
}
static SettingsOverlayRegionBadge Badge(ArRegionalSource source) {
  return source == kArRegionalSource_US       ? kOverlayRegionBadge_US
         : source == kArRegionalSource_Japan  ? kOverlayRegionBadge_Japan
         : source == kArRegionalSource_Europe ? kOverlayRegionBadge_Europe
                                              : kOverlayRegionBadge_Mixed;
}
/* Availability is separate from rule provenance: missing donor bytes never
 * turn a Japanese selection into Custom or change gameplay. */
static bool MissingMedia(const ActRaiserRegionalRulesView *view, const OverlayRegionRow *row,
                         bool effective) {
  const uint16_t mask = ArRegionalProfiles_Mask(row->group);
  const ArRegionalRules *rules = effective ? &view->effective : &view->requested;
  if (row->kind == kOverlayRegionRow_Setting) {
    int artwork = -1;
    switch (row->setting) {
      case kActRaiserRegionalSetting_DeathHeimArt: artwork = kArRegionalArtwork_DeathHeim; break;
      case kActRaiserRegionalSetting_ActionItemArt: artwork = kArRegionalArtwork_ActionItems; break;
      case kActRaiserRegionalSetting_FollowerArt: artwork = kArRegionalArtwork_FollowerSymbols; break;
      case kActRaiserRegionalSetting_LairArt: artwork = kArRegionalArtwork_LairSymbols; break;
      case kActRaiserRegionalSetting_PyramidArt: artwork = kArRegionalArtwork_PyramidDetail; break;
      case kActRaiserRegionalSetting_TitleArt: artwork = kArRegionalArtwork_TitleBackground; break;
      case kActRaiserRegionalSetting_ActorArt: {
        uint8_t requested = 0;
        return ArRegionalActorArtwork_Resolve(&rules->actor_artwork, &requested) && requested &&
            !view->actor_artwork_available;
      }
      case kActRaiserRegionalSetting_Sequences:
        for (unsigned i = 0; i < kArRegionalSequence_Count; ++i)
          if (rules->sequences.source[i] == kArRegionalSource_Japan && !(view->sequences_available & (1u << i))) return true;
        return false;
      default: return false;
    }
    uint8_t requested = 0;
    return ArRegionalArtwork_Resolve(&rules->artwork, &requested) &&
        (requested & (1u << artwork) & ~view->artwork_available);
  }
  if (mask & ArRegionalProfiles_Mask(kArRegionalProfile_Artwork)) {
    uint8_t requested = 0;
    if (ArRegionalArtwork_Resolve(&rules->artwork, &requested) &&
        (requested & ~view->artwork_available))
      return true;
    if (ArRegionalActorArtwork_Resolve(&rules->actor_artwork, &requested) && requested &&
        !view->actor_artwork_available)
      return true;
  }
  if (mask & ArRegionalProfiles_Mask(kArRegionalProfile_Music))
    for (unsigned i = 0; i < kArRegionalSequence_Count; ++i)
      if (rules->sequences.source[i] == kArRegionalSource_Japan &&
          !(view->sequences_available & (1u << i)))
        return true;
  return false;
}

const char *OverlayRegionMenu_DifficultyLabel(ArUiLocale locale, ArRegionalDifficultyChoice choice) {
  const char *const keys[] = {"overlay.region.difficulty.original", "overlay.region.difficulty.beginner",
      "overlay.region.difficulty.normal", "overlay.region.difficulty.expert", "overlay.region.custom"};
  return ArUiCatalog_Text(locale, keys[(unsigned)choice <= kArRegionalDifficultyChoice_Custom ? choice :
      kArRegionalDifficultyChoice_Custom], NULL);
}

static const char *BinaryValue(ArUiLocale locale, const OverlayRegionRow *row, ArRegionalSource source) {
  const bool jp = source == kArRegionalSource_Japan;
  const char *key = NULL;
  switch (row->setting) {
    case kActRaiserRegionalSetting_MagicGesture: key = jp ? "overlay.region.value.up_attack" : "overlay.region.value.magic_button"; break;
    case kActRaiserRegionalSetting_LivesDisplay: key = jp ? "overlay.region.value.spares" : "overlay.region.value.current_life"; break;
    case kActRaiserRegionalSetting_ScorePage: key = jp ? "overlay.region.value.hidden" : "overlay.region.value.shown"; break;
    case kActRaiserRegionalSetting_MenuReturn: key = jp ? "overlay.region.value.keep_menu" : "overlay.region.value.resume"; break;
    case kActRaiserRegionalSetting_SpeedRange: return jp ? "0-7" : "0-9";
    default: return "";
  }
  return ArUiCatalog_Text(locale, key, NULL);
}
bool OverlayRegionMenu_Value(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                             const OverlayRegionRow *row, bool effective, char *out,
                             size_t capacity, SettingsOverlayRegionBadge *badge) {
  if (!view || !row || !out || !capacity || !badge) return false;
  *badge = kOverlayRegionBadge_Mixed;
  if (row->kind == kOverlayRegionRow_Difficulty) {
    const char *value = OverlayRegionMenu_DifficultyLabel(locale, ActRaiserRegionalSettings_DifficultyChoice(view, effective));
    const int written = snprintf(out, capacity, "%s%s", value, !effective && RowPending(view, row) ? " *" : "");
    return written >= 0 && (size_t)written < capacity;
  }
  const ArRegionalSource source = OverlayRegionMenu_RowSource(view, row, effective);
  *badge = Badge(source);
  ArUiTextArgument args[] = {{"region", row->binary ? BinaryValue(locale, row, source) :
      SettingsOverlayRegions_BadgeCode(locale, *badge)}};
  const char *key = !effective && RowPending(view, row)
                        ? "overlay.region.menu.pending"
                        : "overlay.region.menu.value";
  return ArUiCatalog_Format(out, capacity, ArUiCatalog_Text(locale, key, NULL), args, 1);
}
bool OverlayRegionMenu_Description(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                   const OverlayRegionRow *row, char *out, size_t capacity) {
  if (!view || !row || !out || !capacity) return false;
  const ArRegionalSource source = OverlayRegionMenu_RowSource(view, row, false);
  const bool missing = MissingMedia(view, row, false);
  const char *state = "";
  char combined[1024];
  {
    const char *key = NULL;
    if ((view->pending_profile && (ConfirmationMask(view) & ArRegionalProfiles_Mask(row->group))) ||
        (view->population_pending && row->kind == kOverlayRegionRow_Setting && row->setting == kActRaiserRegionalSetting_Population))
      key = "overlay.region.menu.confirm_pending";
    else if (missing) {
      const char *missing_text = ArUiCatalog_Text(locale, "overlay.region.menu.media_missing", NULL);
      const char *pending =
          RowPending(view, row)
              ? ArUiCatalog_Text(locale, "overlay.region.menu.activation_pending", NULL)
              : "";
      const int written =
          snprintf(combined, sizeof(combined), "%s%s%s", missing_text, *pending ? " " : "", pending);
      if (written < 0 || (size_t)written >= sizeof(combined)) return false;
      state = combined;
    } else if (row->group == kArRegionalProfile_Arrival && view->arrival_locked)
      key = "overlay.region.menu.arrival_locked";
    else if (RowPending(view, row))
      key = "overlay.region.menu.activation_pending";
    if (key) state = ArUiCatalog_Text(locale, key, NULL);
  }
  const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale, Badge(source))}, {"state", missing ? "" : state},
      {"placements", ArUiCatalog_Text(locale, (view->pending_profile &&
          (ConfirmationMask(view) & ArRegionalProfiles_Mask(kArRegionalProfile_Stage))
          ? view->pending_population : view->requested.placements.enemies) == kArRegionalSource_Europe
          ? "overlay.region.difficulty.eu_placements" : "overlay.region.difficulty.other_placements", NULL)}};
  const char *key = row->help_key;
  if (row->kind == kOverlayRegionRow_Difficulty) {
    const ArRegionalDifficultyChoice choice = ActRaiserRegionalSettings_DifficultyChoice(view, false);
    const char *const help[] = {"overlay.region.difficulty.original_help", "overlay.region.difficulty.beginner_help",
        "overlay.region.difficulty.normal_help", "overlay.region.difficulty.expert_help", "overlay.region.difficulty.custom_help"};
    key = help[choice];
  }
  if (!missing) return ArUiCatalog_Format(out, capacity, ArUiCatalog_Text(locale, key, NULL), args, 3);
  char help[2048];
  if (!ArUiCatalog_Format(help, sizeof(help), ArUiCatalog_Text(locale, key, NULL), args, 3)) return false;
  // Missing donor data is an explanation, not a selectable "partial" mode.
  // Lead with it so the compact preview cannot hide the fallback.
  const int written = snprintf(out, capacity, "%s\n%s", state, help);
  return written >= 0 && (size_t)written < capacity;
}

bool OverlayRegionMenu_PresetWarning(ArUiLocale locale, const OverlayRegionRow *row,
    ArRegionalSource source, const ActRaiserRegionalEditImpact *impact, char *out, size_t capacity) {
  if (!row || row->kind != kOverlayRegionRow_Preset || !impact) return false;
  const char *consequence = impact->towns == kArRegionalTownImpact_Redevelopment
      ? "overlay.region.preset.redevelopment" : "overlay.region.preset.no_rebuild";
  const ArUiTextArgument args[] = {
    {"region", SettingsOverlayRegions_BadgeLabel(locale, Badge(source))},
    {"consequence", ArUiCatalog_Text(locale, consequence, NULL)},
    {"history", impact->estimated_history ? ArUiCatalog_Text(locale, "overlay.region.warning.estimated", NULL) : ""},
  };
  return ArUiCatalog_Format(out, capacity, ArUiCatalog_Text(locale, row->group == kArRegionalProfile_Gameplay
      ? "overlay.region.preset.gameplay" : "overlay.region.preset.presentation", NULL), args, 3);
}
