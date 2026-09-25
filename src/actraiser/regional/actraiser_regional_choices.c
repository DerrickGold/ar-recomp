#include "actraiser/regional/actraiser_regional_settings.h"
#include "regional/regional_families.h"

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

/* One row per settings choice that reads a family directly. A row reads one
 * rule's source, the family's own group summary, or the source every rule
 * shares; Population, the placement level and the two cost groups need more
 * than one family and are handled after the table. */
typedef enum ChoiceRead {
  kChoiceRead_Rule,
  kChoiceRead_Group,
  kChoiceRead_Uniform,
} ChoiceRead;
typedef struct ChoiceRow {
  uint8_t setting;
  uint8_t family;
  uint8_t rule;
  uint8_t read;
} ChoiceRow;
static const ChoiceRow kChoiceRows[] = {
  {kActRaiserRegionalSetting_RetryScore, kArRegionalFamily_RetryScore, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_TownWait, kArRegionalFamily_TownWait, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Fishing, kArRegionalFamily_Fishing, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_ScorePage, kArRegionalFamily_ScorePage, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_MenuReturn, kArRegionalFamily_MenuReturn, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_SpeedRange, kArRegionalFamily_SpeedRange, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_MagicGesture, kArRegionalFamily_MagicGesture, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_LairReserves, kArRegionalFamily_LairSeeds, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_LairReloads, kArRegionalFamily_LairReloads, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_HouseCredit, kArRegionalFamily_HouseCredit, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_LivesDisplay, kArRegionalFamily_LivesDisplay, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_SkullWait, kArRegionalFamily_SkullWait, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_LevelGoals, kArRegionalFamily_LevelGoals, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Construction, kArRegionalFamily_Construction, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Arrival, kArRegionalFamily_Arrival, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_CompassReturn, kArRegionalFamily_Story,
   kArRegionalStory_ClearCompassPrerequisite, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_StatueVolley, kArRegionalFamily_StatueVolley, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_ScoreLives, kArRegionalFamily_ScoreLives, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Inventory, kArRegionalFamily_SpellInventory, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Hazards, kArRegionalFamily_Hazards, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Terrain, kArRegionalFamily_Terrain, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Music, kArRegionalFamily_Music, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_EnemyPlacements, kArRegionalFamily_Placements,
   kArRegionalPlacement_Enemies, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_PickupPlacements, kArRegionalFamily_Placements,
   kArRegionalPlacement_Pickups, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_Mosaic, kArRegionalFamily_Mosaic, 0, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_RoomTimes, kArRegionalFamily_Timers, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Development, kArRegionalFamily_Development, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Recovery, kArRegionalFamily_Recovery, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Quake, kArRegionalFamily_Quake, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_ScoreFeedback, kArRegionalFamily_ScoreFeedback, 0,
   kChoiceRead_Group},
  {kActRaiserRegionalSetting_Sources, kArRegionalFamily_Sources, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Story, kArRegionalFamily_Story, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_TownStatus, kArRegionalFamily_TownStatus, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_SimCombat, kArRegionalFamily_SimCombat, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_SimAi, kArRegionalFamily_SimAi, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_ActionMotion, kArRegionalFamily_ActionMotion, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Emitters, kArRegionalFamily_Emitters, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Bosses, kArRegionalFamily_Bosses, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_Collision, kArRegionalFamily_Collision, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_PlatformSkull, kArRegionalFamily_PlatformSkull, 0,
   kChoiceRead_Group},
  {kActRaiserRegionalSetting_ActorStats, kArRegionalFamily_ActorStats, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_CastHold, kArRegionalFamily_CastHold, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_FireEnemy, kArRegionalFamily_FireEnemy, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_DifficultyRules, kArRegionalFamily_Difficulty, 0,
   kChoiceRead_Group},
  {kActRaiserRegionalSetting_ActionStart, kArRegionalFamily_ActionStart, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_StartingHealth, kArRegionalFamily_ActionStart,
   kArRegionalActionStart_Health, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_StartingLives, kArRegionalFamily_ActionStart,
   kArRegionalActionStart_Spares, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_SpRecovery, kArRegionalFamily_Recovery, kArRegionalRecovery_SP,
   kChoiceRead_Rule},
  {kActRaiserRegionalSetting_AngelRecovery, kArRegionalFamily_Recovery,
   kArRegionalRecovery_Angel, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_ModeEntry, kArRegionalFamily_ModeEntry, 0, kChoiceRead_Group},
  {kActRaiserRegionalSetting_DeathHeimArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_DeathHeim, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_ActionItemArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_ActionItems, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_FollowerArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_FollowerSymbols, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_LairArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_LairSymbols, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_PyramidArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_PyramidDetail, kChoiceRead_Rule},
  {kActRaiserRegionalSetting_TitleArt, kArRegionalFamily_Artwork,
   kArRegionalArtwork_TitleBackground, kChoiceRead_Rule},
  /* Small presentation families have no public group resolver. */
  {kActRaiserRegionalSetting_AitosPoses, kArRegionalFamily_Poses, 0, kChoiceRead_Uniform},
  {kActRaiserRegionalSetting_ActorArt, kArRegionalFamily_ActorArtwork, 0, kChoiceRead_Uniform},
  {kActRaiserRegionalSetting_Sequences, kArRegionalFamily_Sequences, 0, kChoiceRead_Uniform},
};

static ArRegionalSource ReadChoice(const ChoiceRow *row, const ArRegionalRules *rules) {
  const ArRegionalFamilyId family = (ArRegionalFamilyId)row->family;
  ArRegionalSource value = kArRegionalSource_Count;
  switch ((ChoiceRead)row->read) {
    case kChoiceRead_Rule:
      value = ArRegionalFamilies_Source(rules, family, row->rule);
      break;
    case kChoiceRead_Group:
      (void)ArRegionalFamilies_Get(family)->group_source(rules, &value);
      break;
    case kChoiceRead_Uniform:
      value = ArRegionalFamilies_Source(rules, family, 0);
      for (unsigned i = 1; i < ArRegionalFamilies_Get(family)->rules; ++i)
        if (ArRegionalFamilies_Source(rules, family, i) != value) value = kArRegionalSource_Count;
      break;
  }
  return value;
}

/* A choice is pending when its requested and effective fields differ: the
 * one rule it reads, or the whole family, whose difficulty policy also holds
 * the level. */
static bool ChoicePending(const ChoiceRow *row, const ArRegionalRules *requested,
                          const ArRegionalRules *effective) {
  const ArRegionalFamilyId family = (ArRegionalFamilyId)row->family;
  if (row->read == kChoiceRead_Rule)
    return ArRegionalFamilies_Source(requested, family, row->rule) !=
           ArRegionalFamilies_Source(effective, family, row->rule);
  for (unsigned i = 0; i < ArRegionalFamilies_Get(family)->rules; ++i)
    if (ArRegionalFamilies_Source(requested, family, i) !=
        ArRegionalFamilies_Source(effective, family, i))
      return true;
  return family == kArRegionalFamily_Difficulty &&
         requested->difficulty.level != effective->difficulty.level;
}

/* Mirrors the named edit families, not ROM addresses or wire ordinals. The UI
 * receives copied summaries, so it need not inspect policies or invent rules. */
void ActRaiserRegionalSettings_DescribeChoices(
    const ArRegionalRules *requested, const ArRegionalRules *effective,
    ActRaiserRegionalChoiceView out[kActRaiserRegionalSetting_Count]) {
  if (!requested || !effective || !out) return;
  for (unsigned i = 0; i < kActRaiserRegionalSetting_Count; ++i)
    out[i] = (ActRaiserRegionalChoiceView){kArRegionalSource_Count, kArRegionalSource_Count, false};
  for (unsigned i = 0; i < sizeof(kChoiceRows) / sizeof(kChoiceRows[0]); ++i) {
    const ChoiceRow *row = &kChoiceRows[i];
    out[row->setting] = (ActRaiserRegionalChoiceView){
        ReadChoice(row, requested), ReadChoice(row, effective),
        ChoicePending(row, requested, effective)};
  }
  /* EU placement markers also consume the difficulty level. */
  out[kActRaiserRegionalSetting_EnemyPlacements].pending |=
      requested->difficulty.level != effective->difficulty.level &&
      (requested->placements.enemies == kArRegionalSource_Europe ||
       effective->placements.enemies == kArRegionalSource_Europe);
  out[kActRaiserRegionalSetting_Population] = (ActRaiserRegionalChoiceView){
      PopulationSource(requested), PopulationSource(effective),
      memcmp(&requested->support, &effective->support, sizeof(requested->support)) != 0 ||
          requested->level_goals != effective->level_goals ||
          requested->story.source[kArRegionalStory_FillmoreHint] !=
              effective->story.source[kArRegionalStory_FillmoreHint] ||
          requested->story.source[kArRegionalStory_KasandoraTablet] !=
              effective->story.source[kArRegionalStory_KasandoraTablet]};
  /* Cost families share a container but not pending state. */
  _Static_assert(kArRegionalCostGroup_Count == 2, "each cost group has its own choice");
  static const struct { uint8_t setting; ArRegionalCostGroup group; } kCostChoices[] = {
    {kActRaiserRegionalSetting_Scrolls, kArRegionalCostGroup_Scrolls},
    {kActRaiserRegionalSetting_Miracles, kArRegionalCostGroup_Miracles},
  };
  for (unsigned c = 0; c < sizeof(kCostChoices) / sizeof(kCostChoices[0]); ++c) {
    ArRegionalSource source = kArRegionalSource_Count, active = kArRegionalSource_Count;
    (void)ArRegionalCosts_GroupSource(&requested->costs, kCostChoices[c].group, &source);
    (void)ArRegionalCosts_GroupSource(&effective->costs, kCostChoices[c].group, &active);
    bool pending = false;
    for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
      pending |= ArRegionalCosts_Descriptor(i)->group == kCostChoices[c].group &&
                 requested->costs.source[i] != effective->costs.source[i];
    out[kCostChoices[c].setting] = (ActRaiserRegionalChoiceView){source, active, pending};
  }
}
