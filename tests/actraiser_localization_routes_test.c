#include "actraiser/actraiser_localization_routes.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static int failures;
static uint16_t vram_words[32768];
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

int main(void) {
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
  vram_words[0x5800u + 21u * 32u + 15u] = 0xA05Fu;
  ArTextCellRegion preserves[2];
  CHECK(ActRaiserLocalizationRoute_FindNativePreserves(
            route, 0x5800, 32, 32, vram_words,
            sizeof(vram_words) / sizeof(vram_words[0]),
            preserves, 2) == 1);
  CHECK(preserves[0].column == 15 && preserves[0].row == 21 &&
        preserves[0].columns == 1 && preserves[0].rows == 1);
  CHECK(ActRaiserLocalizationRoute_FindNativePreserves(
            route, 0x5800, 32, 32, vram_words,
            sizeof(vram_words) / sizeof(vram_words[0]),
            preserves, 0) == SIZE_MAX);

  observation = Observation(0x01FA6A, 0x018AF1);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id, "system.save.cancelled"));
  observation = Observation(0x01FA7B, 0x018AFB);
  route = ActRaiserLocalizationRoute_ResolveDialogue(&observation);
  CHECK(route && !strcmp(route->semantic_id,
                          "system.message_speed.choose"));
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
  puts("localization route checks passed");
  return failures ? 1 : 0;
}
