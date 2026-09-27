#include "support/regional_test_values.h"
#include "support/test_assert.h"

#include <stddef.h>

static void PoisonGap(void *object, size_t begin, size_t end) {
  assert(begin <= end);
  memset((uint8_t *)object + begin, 0xa5, end - begin);
}

int main(void) {
  ArRegionalSession a, b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  /* The exact gap sizes depend on the ABI. Only padding receives poison. */
  PoisonGap(&b.lairs,
            offsetof(ArRegionalLairHistory, diverged_towns) + sizeof(b.lairs.diverged_towns),
            offsetof(ArRegionalLairHistory, stock));
  PoisonGap(&b.reloads,
            offsetof(ArRegionalLairReloads, diverged_towns) + sizeof(b.reloads.diverged_towns),
            offsetof(ArRegionalLairReloads, delay));
  PoisonGap(&b.sim_actors,
            offsetof(ArRegionalSimActors, active_town_tag) + sizeof(b.sim_actors.active_town_tag),
            sizeof(b.sim_actors));
  PoisonGap(&b.randomizer,
            offsetof(RandomizerConfig, regional_towns) + sizeof(b.randomizer.regional_towns),
            sizeof(b.randomizer));
  PoisonGap(&b, offsetof(ArRegionalSession, arrival_locked) + sizeof(b.arrival_locked),
            offsetof(ArRegionalSession, randomizer));
  assert(TestRegional_EqualSession(&a, &b));

  ArRegionalActionRoomSnapshot room_a, room_b;
  memset(&room_a, 0, sizeof(room_a));
  memset(&room_b, 0, sizeof(room_b));
  PoisonGap(&room_b,
            offsetof(ArRegionalActionRoomSnapshot, statue_volley) + sizeof(room_b.statue_volley),
            offsetof(ArRegionalActionRoomSnapshot, bosses));
  assert(TestRegional_EqualRoom(&room_a, &room_b));
  room_b.bosses = 1;
  assert(!TestRegional_EqualRoom(&room_a, &room_b));
  room_b.bosses = 0;
  room_b.actor_stats.changed = true;
  assert(!TestRegional_EqualRoom(&room_a, &room_b));
  room_b.actor_stats.changed = false;
  room_b.difficulty.single_tendril_bob = true;
  assert(!TestRegional_EqualRoom(&room_a, &room_b));

  /* Ignoring padding must not ignore persisted state, including array ends. */
#define DIFFERENT(field)                                                                           \
  do {                                                                                             \
    b.field = a.field + 1;                                                                         \
    assert(!TestRegional_EqualSession(&a, &b));                                                    \
    b.field = a.field;                                                                             \
  } while (0)
  DIFFERENT(campaign[15]);
  DIFFERENT(slot);
  DIFFERENT(revision);
  DIFFERENT(requested.hazards);
  DIFFERENT(requested.spell_inventory);
  DIFFERENT(effective.hazards);
  DIFFERENT(effective.spell_inventory);
  DIFFERENT(lairs.initialized_towns);
  DIFFERENT(lairs.approximate_towns);
  DIFFERENT(lairs.diverged_towns);
  DIFFERENT(lairs.stock[kArRegionalLairProjections - 1][kArRegionalLairCount - 1]);
  DIFFERENT(reloads.initialized_towns);
  DIFFERENT(reloads.approximate_towns);
  DIFFERENT(reloads.diverged_towns);
  DIFFERENT(reloads.delay[1][kArRegionalLairCount - 1]);
  DIFFERENT(sim_actors.active_town_tag);
  DIFFERENT(sim_actors.active[kArRegionalSimActorSlots - 1].combat);
  DIFFERENT(sim_actors.active[kArRegionalSimActorSlots - 1].ai);
  DIFFERENT(sim_actors.cached[kArRegionalSimActorSlots * kArRegionalSimActorTowns - 1].combat);
  DIFFERENT(sim_actors.cached[kArRegionalSimActorSlots * kArRegionalSimActorTowns - 1].ai);
  DIFFERENT(arrival_locked);
  DIFFERENT(randomizer.seed);
  DIFFERENT(randomizer.hp_percent);
  DIFFERENT(randomizer.attack_percent);
  DIFFERENT(randomizer.generator);
  DIFFERENT(randomizer.enabled);
  DIFFERENT(randomizer.enemy_types);
  DIFFERENT(randomizer.enemy_scope);
  DIFFERENT(randomizer.statue_drops);
  DIFFERENT(randomizer.statue_spots);
  DIFFERENT(randomizer.lair_spots);
  DIFFERENT(randomizer.lair_types);
  DIFFERENT(randomizer.regional_action);
  DIFFERENT(randomizer.regional_towns);
#undef DIFFERENT
  assert(TestRegional_EqualSession(&a, &b));
  return 0;
}
