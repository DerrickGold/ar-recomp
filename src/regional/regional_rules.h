#ifndef AR_REGIONAL_RULES_H
#define AR_REGIONAL_RULES_H

#include "regional_costs.h"
#include "regional_timers.h"
#include "regional_retry.h"
#include "regional_town_wait.h"
#include "regional_fishing.h"
#include "regional_development.h"
#include "regional_recovery.h"
#include "regional_quake.h"
#include "regional_score_page.h"
#include "regional_lives_display.h"
#include "regional_sources.h"
#include "regional_skull_wait.h"
#include "regional_story_prerequisites.h"
#include "regional_menu_return.h"
#include "regional_speed_range.h"
#include "regional_magic_gesture.h"
#include "regional_score_feedback.h"
#include "regional_lair_history.h"
#include "regional_lair_reloads.h"
#include "regional_town_status.h"
#include "regional_level_goals.h"
#include "regional_sim_combat.h"
#include "regional_sim_ai.h"

/* Game-owned value snapshot. No campaign identity, persistence, native memory
 * or UI ownership. Each family keeps its own units and activation boundary;
 * this aggregate is not a request to activate all families together. */
typedef struct ArRegionalRules {
  ArRegionalCostPolicy costs;
  ArRegionalTimerPolicy timers;
  ArRegionalSource retry_score;
  ArRegionalSource town_wait;
  ArRegionalSource fishing;
  ArRegionalDevelopmentPolicy development;
  ArRegionalRecoveryPolicy recovery;
  ArRegionalQuakePolicy quake;
  ArRegionalSource score_page;
  ArRegionalSource menu_return;
  ArRegionalSource speed_range;
  ArRegionalSource magic_gesture;
  ArRegionalSource lair_seeds;
  ArRegionalSource house_credit;
  ArRegionalScorePolicy score_feedback;
  ArRegionalSource lives_display;
  ArRegionalSourcesPolicy sources;
  ArRegionalSource skull_wait;
  ArRegionalStoryPolicy story;
  ArRegionalSource lair_reloads;
  ArRegionalTownStatusPolicy town_status;
  ArRegionalSource level_goals;
  ArRegionalSimCombatPolicy sim_combat;
  ArRegionalSimAiPolicy sim_ai;
} ArRegionalRules;

/* One mapping for activation, observation and replay identity. The retained
 * projection bits remain feature-codec details, never rule-field ordinals. */
static inline ArRegionalLairAccounting ArRegionalRules_LairAccounting(const ArRegionalRules *rules) {
  ArRegionalLairAccounting result;
  result.seeds=rules->lair_seeds;
  result.house_credit=rules->house_credit;
  result.score_conversion=rules->score_feedback.source[kArRegionalScore_Conversion];
  result.score_operation=rules->score_feedback.source[kArRegionalScore_Operation];
  result.score_route=rules->score_feedback.source[kArRegionalScore_Route];
  return result;
}
static inline bool ArRegionalRules_SameAccounting(const ArRegionalRules *a, const ArRegionalRules *b) {
  if (a->lair_seeds!=b->lair_seeds || a->house_credit!=b->house_credit) return false;
  /* Phase is captured by the completion owner, not by a town stock switch. */
  for (unsigned i=0; i<kArRegionalScore_Phase; ++i)
    if (a->score_feedback.source[i]!=b->score_feedback.source[i]) return false;
  return true;
}

#endif
