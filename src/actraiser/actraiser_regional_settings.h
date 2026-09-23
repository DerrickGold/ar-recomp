#ifndef ACTRAISER_REGIONAL_SETTINGS_H
#define ACTRAISER_REGIONAL_SETTINGS_H

#include "regional/regional_rules.h"

/* User-facing groups are not wire-format identifiers or low-level rule IDs. */
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
  kActRaiserRegionalSetting_Count,
} ActRaiserRegionalSettingGroup;

/* Game-thread settings boundary. UI gets value copies and an optimistic edit
 * token, never a session pointer, CPU/WRAM, or save-path ownership. Only the
 * integrated subsets are exposed, not a full regional preset. */
typedef struct ActRaiserRegionalRulesView {
  uint8_t campaign[16];
  uint32_t revision;
  ArRegionalRules requested, effective;
  bool editable;
  bool miracle_in_progress;
  bool lair_history_ready;
  bool lair_reload_ready;
  bool population_pending;
  ArRegionalSource pending_population;
  bool arrival_locked;
} ActRaiserRegionalRulesView;

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

/* False until an accepted New Game/Continue, without modifying output. */
bool ActRaiserRegional_CopyRulesView(ActRaiserRegionalRulesView *out);
/* The view's campaign/revision identifies the requested edit; its editable
 * and policy fields are display data, not authority. Validate again on apply.
 * Ordinary changes persist with the next completed native story save.
 * Population queues only a volatile request; the Palace owner confirms and
 * commits it with its recovery copy and compatible goal changes. */
ActRaiserRegionalEditResult ActRaiserRegional_RequestRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source);

#endif
