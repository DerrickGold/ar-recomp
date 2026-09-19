#include "actraiser/actraiser_localization_routes.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static int failures;
#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++failures;                                                            \
  }                                                                        \
} while (0)

static ActRaiserLocalizationTextObservation Observation(uint32_t source,
                                                        uint32_t caller) {
  ActRaiserLocalizationTextObservation value;
  memset(&value, 0, sizeof(value));
  value.struct_size = sizeof(value);
  value.abi_version = ACTRAISER_LOCALIZATION_TEXT_OBSERVATION_ABI_VERSION;
  value.source_pc24 = source;
  value.caller_pc24 = caller;
  value.map_number = kActRaiserNonActionMap_SkyPalace;
  return value;
}

static ActRaiserLocalizationComposeObservation Compose(uint32_t source,
                                                       uint16_t destination) {
  ActRaiserLocalizationComposeObservation value;
  memset(&value, 0, sizeof(value));
  value.struct_size = sizeof(value);
  value.abi_version =
      ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION;
  value.source_pc24 = source;
  value.destination = destination;
  value.map_number = kActRaiserNonActionMap_SkyPalace;
  return value;
}

int main(void) {
  /* Modal ownership is independent of numeric surface IDs, and requires the
   * real producer continuation. The action title shares this destination. */
  ActRaiserLocalizationComposeObservation sound = Compose(0x029871, 0x080B);
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&sound));
  sound.caller_pc24 = 0x0297F0;
  const ActRaiserLocalizationComposeRoute *sound_route =
      ActRaiserLocalizationRoute_ResolveCompose(&sound);
  CHECK(sound_route && !strcmp(sound_route->semantic_id, "sound_test.menu.labels"));
  CHECK(sound_route && sound_route->region.column == 11 &&
        sound_route->region.row == 8 && sound_route->region.rows == 6);
  sound.map_group = 1;
  CHECK(ActRaiserLocalizationRoute_ResolveCompose(&sound) == sound_route);
  sound.source_pc24 = 0x029896; /* Closing spaces never acquire ownership. */
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&sound));

  /* The ending montage uses the ordinary simulation wrapper. The shared
   * source-table name is an author alias, not a second runtime consumer. */
  const uint32_t ending_sources[] = {
      0x04CC9A, 0x04CD8D, 0x04CF6A, 0x04D0E5,
      0x04D287, 0x04D3BE, 0x04D4FD};
  const uint16_t ending_units[] = {243, 477, 379, 418, 311, 319, 690};
  for (unsigned slot = 0; slot < 7; ++slot) {
    ActRaiserLocalizationTextObservation ending =
        Observation(ending_sources[slot], 0x019330);
    ending.context_pc24 = 0x038251;
    ending.map_number = slot ? slot : kActRaiserNonActionMap_SkyPalace;
    const ActRaiserLocalizationRoute *resolved =
        ActRaiserLocalizationRoute_ResolveDialogue(&ending);
    char id[80];
    snprintf(id, sizeof(id), "dialogue.event.wrapper_00.call_00.source_%02u", slot);
    CHECK(resolved && !strcmp(resolved->semantic_id, id));
    CHECK(resolved && resolved->native_page_count == 1 &&
          resolved->native_page_units[0] == ending_units[slot]);
    ending.map_number = kActRaiserNonActionMap_WorldMap;
    CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&ending));
    ending.map_group = kActRaiserMapGroup_Ending;
    CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&ending));
  }
  ActRaiserLocalizationTextObservation temple = Observation(0x04D7AF, 0x0193B2);
  temple.context_pc24 = 0x01884F;
  temple.map_number = kActRaiserNonActionMap_Temple;
  CHECK(ActRaiserLocalizationRoute_ResolveDialogue(&temple));

  ActRaiserLocalizationTextObservation observation =
      Observation(0x049048, 0x0193B2);
  observation.context_pc24 = 0x0185DA;
  const ActRaiserLocalizationRoute *route =
      ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route != NULL);
  CHECK(route && !strcmp(route->semantic_id,
                          "dialogue.event.wrapper_05.call_02.source_00"));
  CHECK(route && route->region.column == 5 && route->region.row == 19);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 0) == 43);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 1) == 73);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 99) == 73);

  observation = Observation(0x0490BC, 0x0193B2);
  observation.context_pc24 = 0x0185C1;
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route != NULL);
  CHECK(route && !strcmp(route->semantic_id,
                          "dialogue.event.wrapper_05.call_00.source_00"));
  CHECK(route && route->native_page_count == 3);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 0) == 20);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 1) == 83);
  CHECK(ActRaiserLocalizationRoute_PageUnitCount(route, 2) == 41);
  observation = Observation(0x01FA6A, 0x018AF1);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id, "system.save.cancelled"));
  observation = Observation(0x01FA7B, 0x018AFB);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "system.message_speed.choose"));
  observation = Observation(0x01FC06, 0x01886F);
  observation.map_number = kActRaiserNonActionMap_Temple;
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id, "sim.town.gratitude"));
  observation.map_number = kActRaiserNonActionMap_WorldMap;
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));
  observation.map_number = kActRaiserNonActionMap_Temple;
  observation.map_group = 1;
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));
  observation = Observation(0x01FAB1, 0x018B77);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "system.message_speed.sample"));
  observation = Observation(0x01FABE, 0x018B6A);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "system.message_speed.cancelled"));

  /* Aliased pointer targets resolve by the native selector retained in X. */
  observation = Observation(0x049B8C, 0x01888C);
  observation.selector_x = 0x93B5;
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "simulation.event.fillmore.slot_09"));
  observation.selector_x = 0x93B7;
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "simulation.event.fillmore.slot_10"));

  /* A wrapper source is not accepted without the outer call continuation. */
  observation = Observation(0x049048, 0x0193B2);
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));
  observation.context_pc24 = 0x0185DA;
  CHECK(ActRaiserLocalizationRoute_ResolveDialogue(&observation));

  /* Relay routes share wrapper context and use the current town identity. */
  observation = Observation(0x0492E2, 0x019390);
  observation.context_pc24 = 0x03868B;
  observation.map_number = 1;
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "dialogue.event.relay.fillmore"));
  observation.map_number = 2;
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));

  observation.map_group = 1;
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));
  observation.map_group = 0;
  observation = Observation(0x049049, 0x0193B2);
  CHECK(!ActRaiserLocalizationRoute_ResolveDialogue(&observation));

  ActRaiserLocalizationComposeObservation compose =
      Compose(0x01F298, 0x0512);
  const ActRaiserLocalizationComposeRoute *compose_route =
      ActRaiserLocalizationRoute_ResolveCompose(&compose);
  CHECK(compose_route &&
        !strcmp(compose_route->semantic_id, "sky.menu.choice.movement"));
  CHECK(compose_route && compose_route->surface_id == 2);
  CHECK(compose_route && compose_route->region.column == 18 &&
        compose_route->region.row == 5 &&
        compose_route->region.columns == 10 &&
        compose_route->region.rows == 2);

  /* Indexed sources require the table identity and logical selector captured
   * before the native pointer resolver destroys them. */
  compose = Compose(0x01F064, 0x0A12);
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  compose.source_table_pc24 = 0x01F04F;
  compose.source_selector = 1;
  compose_route = ActRaiserLocalizationRoute_ResolveCompose(&compose);
  CHECK(compose_route &&
        !strcmp(compose_route->semantic_id, "sky.menu.magic.stardust"));
  compose.source_pc24 = 0x01F158;
  compose.source_table_pc24 = 0x01F08E;
  compose.source_selector = 11;
  compose_route = ActRaiserLocalizationRoute_ResolveCompose(&compose);
  CHECK(compose_route &&
        !strcmp(compose_route->semantic_id, "sim.menu.possession.slot_11"));
  compose.source_selector = 12;
  compose_route = ActRaiserLocalizationRoute_ResolveCompose(&compose);
  CHECK(compose_route &&
        !strcmp(compose_route->semantic_id, "sim.menu.possession.slot_12"));

  compose = Compose(0x01F4DC, 0x0603);
  compose_route = ActRaiserLocalizationRoute_ResolveCompose(&compose);
  CHECK(compose_route &&
        !strcmp(compose_route->semantic_id,
                "status.report.cities_report"));
  compose.destination = 0x0512;
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  compose.destination = 0x0603;
  compose.map_group = 1;
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  compose.map_group = 0;
  compose.abi_version = 0;
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));

  /* The native pause producer/eraser shares one string and destination across
   * SIM and action. Keep the existing pack key, without admitting other action
   * cards into SIM or claiming title-screen text. */
  compose = Compose(0x00A8EF, 0x0B0D);
  compose.caller_pc24 = 0x02BF1F;
  for (unsigned group = 0; group <= kActRaiserActionMapGroup_Last; ++group) {
    compose.map_group = group;
    for (unsigned town = kActRaiserSimulationTown_First;
         town <= kActRaiserSimulationTown_Last; ++town) {
      compose.map_number = town;
      compose_route = ActRaiserLocalizationRoute_ResolveCompose(&compose);
      CHECK(compose_route && !strcmp(compose_route->semantic_id, "action.hud.pause"));
    }
  }
  compose.map_group = 0;
  compose.map_number = kActRaiserNonActionMap_Title;
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  compose.map_number = kActRaiserSimulationTown_First;
  compose.destination = 0x090D;
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  compose.source_pc24 = 0x00A8DF; /* READY remains action-only. */
  CHECK(!ActRaiserLocalizationRoute_ResolveCompose(&compose));
  puts("localization route checks passed");
  return failures ? 1 : 0;
}
