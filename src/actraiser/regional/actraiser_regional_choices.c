#include "actraiser/regional/actraiser_regional_settings.h"

#include <string.h>

ArRegionalDifficultyChoice ActRaiserRegionalSettings_DifficultyChoice(
    const ActRaiserRegionalRulesView *view, bool effective) {
  if (!view) return kArRegionalDifficultyChoice_Custom;
  // The entire queued preset awaits Palace review, including difficulty.
  // Resolve its preview through the same inventory as application, without
  // exposing preset-expansion rules to the presentation layer.
  ArRegionalRules candidate;
  if (!effective && view->population_pending && view->pending_profile &&
      (ArRegionalProfiles_Mask(view->pending_profile_group) &
       ArRegionalProfiles_Mask(kArRegionalProfile_Difficulty)) &&
      ArRegionalProfiles_Expand(&view->requested, view->pending_profile_group,
                                view->pending_population, &candidate))
    return ArRegionalDifficulty_Choice(&candidate.difficulty);
  return ArRegionalDifficulty_Choice(effective ? &view->effective.difficulty : &view->requested.difficulty);
}

static ArRegionalSource PopulationSource(const ArRegionalRules *r) {
  ArRegionalSource source = kArRegionalSource_Count;
  bool japanese;
  ArRegionalStorySnapshot story;
  if (!ArRegionalSupport_GroupSource(&r->support, &source) ||
      !ArRegionalLevelGoals_Resolve(r->level_goals, &japanese) ||
      !ArRegionalStory_Resolve(&r->story, &story)) return kArRegionalSource_Count;
  if (japanese != (source == kArRegionalSource_Japan)) return kArRegionalSource_Count;
  for (unsigned i = kArRegionalStory_FillmoreHint; i <= kArRegionalStory_KasandoraTablet; ++i)
    if (story.value[i] != ArRegionalStory_Descriptor(i)->value[source]) return kArRegionalSource_Count;
  return source;
}

/* Mirrors the named edit families, not ROM addresses or wire ordinals. The UI
 * receives copied summaries, so it need not inspect policies or invent rules. */
void ActRaiserRegionalSettings_DescribeChoices(const ArRegionalRules *requested,
    const ArRegionalRules *effective, ActRaiserRegionalChoiceView out[kActRaiserRegionalSetting_Count]) {
  if (!requested || !effective || !out) return;
  for (unsigned i = 0; i < kActRaiserRegionalSetting_Count; ++i)
    out[i] = (ActRaiserRegionalChoiceView){kArRegionalSource_Count, kArRegionalSource_Count, false};
#define CHOICE(key, field, read) do { \
    ArRegionalSource source = kArRegionalSource_Count, active = kArRegionalSource_Count; \
    const ArRegionalRules *r = requested; read; source = value; \
    r = effective; read; active = value; \
    out[kActRaiserRegionalSetting_##key] = (ActRaiserRegionalChoiceView){source, active, \
        memcmp(&requested->field, &effective->field, sizeof(requested->field)) != 0}; \
  } while (0)
  ArRegionalSource value;
#define SINGLE(key, field) CHOICE(key, field, value = r->field)
#define FAMILY(key, field, name) CHOICE(key, field, \
    value = kArRegionalSource_Count; (void)ArRegional##name##_GroupSource(&r->field, &value))
  SINGLE(RetryScore, retry_score);
  SINGLE(TownWait, town_wait);
  SINGLE(Fishing, fishing);
  SINGLE(ScorePage, score_page);
  SINGLE(MenuReturn, menu_return);
  SINGLE(SpeedRange, speed_range);
  SINGLE(MagicGesture, magic_gesture);
  SINGLE(LairReserves, lair_seeds);
  SINGLE(LairReloads, lair_reloads);
  SINGLE(HouseCredit, house_credit);
  SINGLE(LivesDisplay, lives_display);
  SINGLE(SkullWait, skull_wait);
  SINGLE(LevelGoals, level_goals);
  SINGLE(Construction, construction);
  SINGLE(Arrival, arrival);
  SINGLE(CompassReturn, story.source[kArRegionalStory_ClearCompassPrerequisite]);
  SINGLE(StatueVolley, statue_volley);
  SINGLE(ScoreLives, score_lives);
  SINGLE(Inventory, spell_inventory);
  SINGLE(Hazards, hazards);
  SINGLE(Terrain, terrain);
  SINGLE(Music, music);
  SINGLE(EnemyPlacements, placements.enemies);
  out[kActRaiserRegionalSetting_EnemyPlacements].pending |=
      requested->difficulty.level != effective->difficulty.level &&
      (requested->placements.enemies == kArRegionalSource_Europe || effective->placements.enemies == kArRegionalSource_Europe);
  SINGLE(PickupPlacements, placements.pickups);
  SINGLE(Mosaic, mosaic);
  FAMILY(RoomTimes, timers, Timers);
  FAMILY(Development, development, Development);
  FAMILY(Recovery, recovery, Recovery);
  FAMILY(Quake, quake, Quake);
  FAMILY(ScoreFeedback, score_feedback, Score);
  FAMILY(Sources, sources, Sources);
  FAMILY(Story, story, Story);
  FAMILY(TownStatus, town_status, TownStatus);
  FAMILY(SimCombat, sim_combat, SimCombat);
  FAMILY(SimAi, sim_ai, SimAi);
  CHOICE(Population, support, value = PopulationSource(r));
  out[kActRaiserRegionalSetting_Population].pending |= requested->level_goals != effective->level_goals ||
      requested->story.source[kArRegionalStory_FillmoreHint] != effective->story.source[kArRegionalStory_FillmoreHint] ||
      requested->story.source[kArRegionalStory_KasandoraTablet] != effective->story.source[kArRegionalStory_KasandoraTablet];
  FAMILY(ActionMotion, action_motion, ActionMotion);
  FAMILY(Emitters, emitters, Emitter);
  FAMILY(Bosses, bosses, Boss);
  FAMILY(Collision, collision, Collision);
  FAMILY(PlatformSkull, platform_skull, PlatformSkull);
  FAMILY(ActorStats, actor_stats, ActorStats);
  FAMILY(CastHold, cast_hold, CastHold);
  FAMILY(FireEnemy, fire_enemy, Fire);
  FAMILY(DifficultyRules, difficulty, Difficulty);
  FAMILY(ActionStart, action_start, ActionStart);
  SINGLE(StartingHealth, action_start.source[kArRegionalActionStart_Health]);
  SINGLE(StartingLives, action_start.source[kArRegionalActionStart_Spares]);
  SINGLE(SpRecovery, recovery.source[kArRegionalRecovery_SP]);
  SINGLE(AngelRecovery, recovery.source[kArRegionalRecovery_Angel]);
  FAMILY(ModeEntry, mode_entry, Mode);
  CHOICE(Scrolls, costs, value = kArRegionalSource_Count;
      (void)ArRegionalCosts_GroupSource(&r->costs, kArRegionalCostGroup_Scrolls, &value));
  CHOICE(Miracles, costs, value = kArRegionalSource_Count;
      (void)ArRegionalCosts_GroupSource(&r->costs, kArRegionalCostGroup_Miracles, &value));
  /* Cost families share a container but do not share pending state. */
  out[kActRaiserRegionalSetting_Scrolls].pending = false;
  out[kActRaiserRegionalSetting_Miracles].pending = false;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const unsigned key = ArRegionalCosts_Descriptor(i)->group == kArRegionalCostGroup_Scrolls
        ? kActRaiserRegionalSetting_Scrolls : kActRaiserRegionalSetting_Miracles;
    out[key].pending |= requested->costs.source[i] != effective->costs.source[i];
  }
  SINGLE(DeathHeimArt, artwork.source[kArRegionalArtwork_DeathHeim]);
  SINGLE(ActionItemArt, artwork.source[kArRegionalArtwork_ActionItems]);
  SINGLE(FollowerArt, artwork.source[kArRegionalArtwork_FollowerSymbols]);
  SINGLE(LairArt, artwork.source[kArRegionalArtwork_LairSymbols]);
  SINGLE(PyramidArt, artwork.source[kArRegionalArtwork_PyramidDetail]);
  SINGLE(TitleArt, artwork.source[kArRegionalArtwork_TitleBackground]);
  /* Small presentation families have no public group resolver. */
#define UNIFORM(key, field) CHOICE(key, field, \
    value = r->field.source[0]; \
    for (unsigned i = 1; i < sizeof(r->field.source) / sizeof(r->field.source[0]); ++i) \
      if (r->field.source[i] != value) value = kArRegionalSource_Count)
  UNIFORM(AitosPoses, poses);
  UNIFORM(ActorArt, actor_artwork);
  UNIFORM(Sequences, sequences);
#undef UNIFORM
#undef FAMILY
#undef SINGLE
#undef CHOICE
}
