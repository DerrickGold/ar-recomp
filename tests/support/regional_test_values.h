#ifndef AR_REGIONAL_TEST_VALUES_H
#define AR_REGIONAL_TEST_VALUES_H
/* Value comparisons for persisted regional state. These test oracles do not
 * call the production codecs and never compare padding in mixed-type structs. */
#include "regional/session/regional_session.h"
#include "regional/session/regional_session_action.h"

#include <string.h>

static inline bool TestRegional_EqualRecipe(const RandomizerConfig *a, const RandomizerConfig *b) {
  return a->seed == b->seed && a->hp_percent == b->hp_percent &&
         a->attack_percent == b->attack_percent && a->generator == b->generator &&
         a->enabled == b->enabled && a->enemy_types == b->enemy_types &&
         a->enemy_scope == b->enemy_scope && a->statue_drops == b->statue_drops &&
         a->statue_spots == b->statue_spots && a->lair_spots == b->lair_spots &&
         a->lair_types == b->lair_types && a->regional_action == b->regional_action &&
         a->regional_towns == b->regional_towns;
}

static inline bool TestRegional_EqualLairs(const ArRegionalLairHistory *a,
                                           const ArRegionalLairHistory *b) {
  return a->initialized_towns == b->initialized_towns &&
         a->approximate_towns == b->approximate_towns && a->diverged_towns == b->diverged_towns &&
         !memcmp(a->stock, b->stock, sizeof(a->stock));
}

static inline bool TestRegional_EqualReloads(const ArRegionalLairReloads *a,
                                             const ArRegionalLairReloads *b) {
  return a->initialized_towns == b->initialized_towns &&
         a->approximate_towns == b->approximate_towns && a->diverged_towns == b->diverged_towns &&
         !memcmp(a->delay, b->delay, sizeof(a->delay));
}

static inline bool TestRegional_EqualActors(const ArRegionalSimActors *a,
                                            const ArRegionalSimActors *b) {
  if (a->active_town_tag != b->active_town_tag) return false;
  for (unsigned i = 0; i < kArRegionalSimActorSlots; ++i) {
    if (a->active[i].combat != b->active[i].combat || a->active[i].ai != b->active[i].ai)
      return false;
  }
  for (unsigned i = 0; i < kArRegionalSimActorSlots * kArRegionalSimActorTowns; ++i) {
    if (a->cached[i].combat != b->cached[i].combat || a->cached[i].ai != b->cached[i].ai)
      return false;
  }
  return true;
}

static inline bool TestRegional_EqualSession(const ArRegionalSession *a,
                                             const ArRegionalSession *b) {
  /* Rules consist entirely of regional enums and arrays of those enums. */
  return !memcmp(a->campaign, b->campaign, sizeof(a->campaign)) && a->slot == b->slot &&
         a->revision == b->revision &&
         !memcmp(&a->requested, &b->requested, sizeof(a->requested)) &&
         !memcmp(&a->effective, &b->effective, sizeof(a->effective)) &&
         TestRegional_EqualLairs(&a->lairs, &b->lairs) &&
         TestRegional_EqualReloads(&a->reloads, &b->reloads) &&
         TestRegional_EqualActors(&a->sim_actors, &b->sim_actors) &&
         a->arrival_locked == b->arrival_locked &&
         TestRegional_EqualRecipe(&a->randomizer, &b->randomizer);
}

static inline bool TestRegional_EqualRoom(const ArRegionalActionRoomSnapshot *a,
                                          const ArRegionalActionRoomSnapshot *b) {
  return a->hazards == b->hazards && a->terrain == b->terrain && a->mosaic == b->mosaic &&
         a->poses == b->poses && a->artwork == b->artwork &&
         a->placements.enemies == b->placements.enemies &&
         a->placements.pickups == b->placements.pickups &&
         a->placement_difficulty == b->placement_difficulty && a->motion == b->motion &&
         a->emitters == b->emitters && a->statue_volley == b->statue_volley &&
         a->bosses == b->bosses && a->difficulty.spawn_hp == b->difficulty.spawn_hp &&
         a->difficulty.contact_extra == b->difficulty.contact_extra &&
         a->difficulty.timer_reload == b->difficulty.timer_reload &&
         a->difficulty.skip_dragon_attack == b->difficulty.skip_dragon_attack &&
         a->difficulty.single_tendril_bob == b->difficulty.single_tendril_bob &&
         a->score_lives == b->score_lives && a->collision == b->collision &&
         a->platform_skull == b->platform_skull &&
         !memcmp(a->actor_stats.value, b->actor_stats.value, sizeof(a->actor_stats.value)) &&
         a->actor_stats.changed == b->actor_stats.changed && a->cast_hold == b->cast_hold &&
         a->fire_enemy == b->fire_enemy;
}
#endif
