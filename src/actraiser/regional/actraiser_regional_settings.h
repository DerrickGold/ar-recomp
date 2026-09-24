#ifndef ACTRAISER_REGIONAL_SETTINGS_H
#define ACTRAISER_REGIONAL_SETTINGS_H

#include "regional/regional_profiles.h"

/* Public UI boundary. actraiser_regional_editor.c validates edits against fresh
 * authority supplied by actraiser_regional_runtime.c. Menu rows and translated
 * labels live in settings_overlay/regional; neither receives mutable state. */

/* Fine-grained integration/test edits; the player-facing bundles are declared
 * in regional_profiles.h. Neither enumeration is a wire-format identifier. */
typedef enum ActRaiserRegionalSettingGroup {
  kActRaiserRegionalSetting_Scrolls,
  kActRaiserRegionalSetting_Miracles,
  kActRaiserRegionalSetting_RoomTimes,
  kActRaiserRegionalSetting_RetryScore,
  kActRaiserRegionalSetting_TownWait,
  kActRaiserRegionalSetting_Fishing,
  kActRaiserRegionalSetting_Development,
  kActRaiserRegionalSetting_Recovery,
  kActRaiserRegionalSetting_Quake,
  kActRaiserRegionalSetting_ScorePage,
  kActRaiserRegionalSetting_MenuReturn,
  kActRaiserRegionalSetting_SpeedRange,
  kActRaiserRegionalSetting_MagicGesture,
  kActRaiserRegionalSetting_LairReserves,
  kActRaiserRegionalSetting_HouseCredit,
  kActRaiserRegionalSetting_ScoreFeedback,
  kActRaiserRegionalSetting_LivesDisplay,
  kActRaiserRegionalSetting_Sources,
  kActRaiserRegionalSetting_SkullWait,
  kActRaiserRegionalSetting_Story,
  kActRaiserRegionalSetting_LairReloads,
  kActRaiserRegionalSetting_TownStatus,
  kActRaiserRegionalSetting_LevelGoals,
  kActRaiserRegionalSetting_SimCombat,
  kActRaiserRegionalSetting_SimAi,
  kActRaiserRegionalSetting_Construction,
  kActRaiserRegionalSetting_Population,
  kActRaiserRegionalSetting_Arrival,
  kActRaiserRegionalSetting_ActionMotion,
  kActRaiserRegionalSetting_Emitters,
  kActRaiserRegionalSetting_StatueVolley,
  kActRaiserRegionalSetting_Bosses,
  kActRaiserRegionalSetting_Collision,
  kActRaiserRegionalSetting_PlatformSkull,
  kActRaiserRegionalSetting_ActorStats,
  kActRaiserRegionalSetting_CastHold,
  kActRaiserRegionalSetting_FireEnemy,
  kActRaiserRegionalSetting_DifficultyRules,
  kActRaiserRegionalSetting_DifficultyLevel,
  kActRaiserRegionalSetting_ScoreLives,
  kActRaiserRegionalSetting_ActionStart,
  kActRaiserRegionalSetting_Inventory,
  kActRaiserRegionalSetting_ModeEntry,
  kActRaiserRegionalSetting_Hazards,
  kActRaiserRegionalSetting_Terrain,
  kActRaiserRegionalSetting_Music,
  kActRaiserRegionalSetting_EnemyPlacements,
  kActRaiserRegionalSetting_PickupPlacements,
  kActRaiserRegionalSetting_Mosaic,
  kActRaiserRegionalSetting_DeathHeimArt,
  kActRaiserRegionalSetting_ActionItemArt,
  kActRaiserRegionalSetting_FollowerArt,
  kActRaiserRegionalSetting_LairArt,
  kActRaiserRegionalSetting_PyramidArt,
  kActRaiserRegionalSetting_TitleArt,
  kActRaiserRegionalSetting_AitosPoses,
  kActRaiserRegionalSetting_Sequences,
  kActRaiserRegionalSetting_ActorArt,
  kActRaiserRegionalSetting_CompassReturn,
  kActRaiserRegionalSetting_StartingHealth,
  kActRaiserRegionalSetting_StartingLives,
  kActRaiserRegionalSetting_SpRecovery,
  kActRaiserRegionalSetting_AngelRecovery,
  kActRaiserRegionalSetting_Count,
} ActRaiserRegionalSettingGroup;

typedef struct ActRaiserRegionalChoiceView {
  ArRegionalSource source, active_source;
  bool pending;
} ActRaiserRegionalChoiceView;
/* Pure display projection; no UI labels, mutable session or native memory. */
void ActRaiserRegionalSettings_DescribeChoices(const ArRegionalRules *requested,
    const ArRegionalRules *effective, ActRaiserRegionalChoiceView out[kActRaiserRegionalSetting_Count]);

/* Game-thread settings boundary. UI gets value copies and an optimistic edit
 * token, never a session pointer, CPU/WRAM, or save-path ownership. Only the
 * display summaries are resolved by the game owner, not inferred by the UI. */
typedef struct ActRaiserRegionalRulesView {
  uint8_t campaign[16];
  uint32_t revision;
  ArRegionalRules requested, effective;
  ArRegionalProfileSummary profiles[kArRegionalProfile_Count];
  ArRegionalProfileSummary active_profiles[kArRegionalProfile_Count];
  uint16_t pending_groups;
  ActRaiserRegionalChoiceView choices[kActRaiserRegionalSetting_Count];
  bool editable;
  bool new_game; /* title draft, discarded by Continue */
  bool miracle_in_progress;
  bool lair_history_ready;
  bool lair_reload_ready;
  bool lair_history_estimated;
  bool lair_reload_estimated;
  bool population_pending;
  ArRegionalSource pending_population;
  bool pending_profile;
  ArRegionalProfileGroup pending_profile_group;
  bool arrival_locked;
  uint8_t artwork_available; /* validated donor resources, independent of policy */
  uint8_t sequences_available;
  bool actor_artwork_available;
} ActRaiserRegionalRulesView;

/* Pure copied-view projection; requested includes a queued full preset. */
ArRegionalDifficultyChoice ActRaiserRegionalSettings_DifficultyChoice(
    const ActRaiserRegionalRulesView *view, bool effective);

typedef enum ActRaiserRegionalEditResult {
  kActRaiserRegionalEdit_Invalid,
  kActRaiserRegionalEdit_Locked,
  kActRaiserRegionalEdit_Stale,
  kActRaiserRegionalEdit_Unchanged,
  kActRaiserRegionalEdit_Applied,
  kActRaiserRegionalEdit_HistoryUnavailable,
  kActRaiserRegionalEdit_Deferred,
  kActRaiserRegionalEdit_Incompatible,
} ActRaiserRegionalEditResult;

/* Read-only, game-authoritative consequences for one proposed selection.
 * Advisory only: applying still revalidates the campaign/revision token. */
typedef struct ActRaiserRegionalEditImpact {
  ArRegionalTownImpact towns;
  bool estimated_history;
} ActRaiserRegionalEditImpact;
ActRaiserRegionalEditResult ActRaiserRegional_PreviewProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group,
    ArRegionalSource source, ActRaiserRegionalEditImpact *out);
ActRaiserRegionalEditResult ActRaiserRegional_PreviewRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source, ActRaiserRegionalEditImpact *out);

/* Title edits target an unsaved new-game draft. Continue loads its own rules;
 * outside title/gameplay this returns false without modifying output. */
bool ActRaiserRegional_CopyRulesView(ActRaiserRegionalRulesView *out);
ActRaiserRegionalEditResult ActRaiserRegional_RequestProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group,
    ArRegionalSource source);
/* The view's campaign/revision identifies the requested edit; its editable
 * and policy fields are display data, not authority. Validate again on apply.
 * Ordinary changes persist with the next completed native story save.
 * Population queues only a volatile request; the Palace owner confirms and
 * commits it with its recovery copy and compatible goal changes. */
ActRaiserRegionalEditResult ActRaiserRegional_RequestRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source);
ActRaiserRegionalEditResult ActRaiserRegional_RequestDifficulty(
    const ActRaiserRegionalRulesView *view, ArRegionalDifficulty level);
ActRaiserRegionalEditResult ActRaiserRegional_RequestDifficultyChoice(
    const ActRaiserRegionalRulesView *view, ArRegionalDifficultyChoice choice);

#endif
