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
#include "regional_menu_return.h"
#include "regional_speed_range.h"
#include "regional_magic_gesture.h"

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
} ArRegionalRules;

#endif
