#ifndef AR_REGIONAL_RULES_H
#define AR_REGIONAL_RULES_H

#include "regional_costs.h"
#include "regional/action/regional_timers.h"
#include "regional/action/regional_retry.h"
#include "regional/towns/regional_town_wait.h"
#include "regional/towns/regional_fishing.h"
#include "regional/towns/regional_development.h"
#include "regional/towns/regional_recovery.h"
#include "regional/towns/regional_quake.h"
#include "regional/interaction/regional_score_page.h"
#include "regional/interaction/regional_lives_display.h"
#include "regional/towns/regional_sources.h"
#include "regional/towns/regional_skull_wait.h"
#include "regional/towns/regional_story_prerequisites.h"
#include "regional/interaction/regional_menu_return.h"
#include "regional/interaction/regional_speed_range.h"
#include "regional/interaction/regional_magic_gesture.h"
#include "regional/towns/regional_score_feedback.h"
#include "regional/towns/regional_lair_history.h"
#include "regional/towns/regional_lair_reloads.h"
#include "regional/towns/regional_town_status.h"
#include "regional/towns/regional_level_goals.h"
#include "regional/towns/regional_sim_combat.h"
#include "regional/towns/regional_sim_ai.h"
#include "regional/towns/regional_construction.h"
#include "regional/towns/regional_support.h"
#include "regional/interaction/regional_arrival.h"
#include "regional/action/regional_action_motion.h"
#include "regional/action/regional_emitters.h"
#include "regional/action/regional_volley.h"
#include "regional/action/regional_boss_rules.h"
#include "regional/action/regional_collision.h"
#include "regional/action/regional_platform_skull.h"
#include "regional/action/regional_actor_stats.h"
#include "regional/action/regional_cast_hold.h"
#include "regional/action/regional_fire_enemy.h"
#include "regional/action/regional_difficulty.h"
#include "regional/action/regional_score_lives.h"
#include "regional/action/regional_action_start.h"
#include "regional/action/regional_spell_inventory.h"
#include "regional/action/regional_mode_entry.h"
#include "regional/action/regional_hazards.h"
#include "regional/action/regional_terrain.h"
#include "regional/presentation/regional_music.h"
#include "regional/presentation/regional_mosaic.h"
#include "regional/presentation/regional_poses.h"
#include "regional/presentation/regional_artwork.h"
#include "regional/action/regional_placement_policy.h"

/* Game-owned value snapshot. No campaign identity, persistence, native memory
 * or UI ownership. Each family keeps its own units and activation boundary;
 * this aggregate is not a request to activate all families together. */
typedef struct ArRegionalRules {
  ArRegionalSource hazards;
  ArRegionalSource terrain;
  ArRegionalSource music;
  ArRegionalSequencePolicy sequences;
  ArRegionalSource mosaic;
  ArRegionalPosePolicy poses;
  ArRegionalArtworkPolicy artwork;
  ArRegionalActorArtworkPolicy actor_artwork;
  ArRegionalPlacementPolicy placements;
  ArRegionalModePolicy mode_entry;
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
  ArRegionalSource construction;
  ArRegionalSupportPolicy support;
  ArRegionalSource arrival;
  ArRegionalActionMotionPolicy action_motion;
  ArRegionalEmitterPolicy emitters;
  ArRegionalSource statue_volley;
  ArRegionalBossPolicy bosses;
  ArRegionalCollisionPolicy collision;
  ArRegionalPlatformSkullPolicy platform_skull;
  ArRegionalActorStatsPolicy actor_stats;
  ArRegionalCastHoldPolicy cast_hold;
  ArRegionalFirePolicy fire_enemy;
  ArRegionalDifficultyPolicy difficulty;
  ArRegionalSource score_lives;
  ArRegionalActionStartPolicy action_start;
  ArRegionalSource spell_inventory;
} ArRegionalRules;

/* Conservative supported mix: any reduced support coefficient requires the
 * Japanese level and population-event targets. Western support may use either
 * set of goals. This is a compatibility rule, not a claim about fixed caps.
 * The unrelated failed-Act2 Compass leaf remains independently selectable. */
static inline bool ArRegionalRules_PopulationCompatible(const ArRegionalRules *rules) {
  if (!rules) return false;
  ArRegionalSupportSnapshot support;
  ArRegionalStorySnapshot story;
  bool japanese;
  if (!ArRegionalSupport_Resolve(&rules->support,&support) ||
      !ArRegionalStory_Resolve(&rules->story,&story) ||
      !ArRegionalLevelGoals_Resolve(rules->level_goals,&japanese)) return false;
  bool reduced=false;
  for (unsigned i=0;i<kArRegionalSupport_Count;++i)
    reduced |= support.amount[i] < ArRegionalSupport_Descriptor((ArRegionalSupportRule)i)->amount[kArRegionalSource_US];
  return !reduced || (japanese &&
      story.value[kArRegionalStory_FillmoreHint]==ArRegionalStory_Descriptor(kArRegionalStory_FillmoreHint)->value[kArRegionalSource_Japan] &&
      story.value[kArRegionalStory_KasandoraTablet]==ArRegionalStory_Descriptor(kArRegionalStory_KasandoraTablet)->value[kArRegionalSource_Japan]);
}

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
