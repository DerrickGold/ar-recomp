#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

uint8 g_ram[kActRaiserWramSize];

static int failures;

#define CHECK(expr) do {                                                    \
  if (!(expr)) {                                                            \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++;                                                             \
  }                                                                         \
} while (0)

static void TestSimulationTownScope(void) {
  CHECK(!ActRaiser_IsSimulationTown(kActRaiserMapGroup_NonAction,
                                    kActRaiserNonActionMap_Title));
  for (int town = kActRaiserSimulationTown_First;
       town <= kActRaiserSimulationTown_Last; town++) {
    CHECK(ActRaiser_IsSimulationTown(kActRaiserMapGroup_NonAction,
                                     (uint8)town));
  }
  CHECK(!ActRaiser_IsSimulationTown(kActRaiserMapGroup_NonAction,
                                    kActRaiserNonActionMap_SkyPalace));
  CHECK(!ActRaiser_IsSimulationTown(kActRaiserMapGroup_Fillmore,
                                    kActRaiserNonActionMap_Fillmore));
}

static void TestPurePickerPredicate(void) {
  CHECK(!ActRaiser_SimMapPickerActiveForState(
      kActRaiserMapGroup_NonAction, kActRaiserNonActionMap_Fillmore, 0));
  CHECK(ActRaiser_SimMapPickerActiveForState(
      kActRaiserMapGroup_NonAction, kActRaiserNonActionMap_Fillmore, 1));
  CHECK(ActRaiser_SimMapPickerActiveForState(
      kActRaiserMapGroup_NonAction, kActRaiserNonActionMap_Northwall, 0x0100));
  CHECK(!ActRaiser_SimMapPickerActiveForState(
      kActRaiserMapGroup_NonAction, kActRaiserNonActionMap_SkyPalace, 1));
  CHECK(!ActRaiser_SimMapPickerActiveForState(
      kActRaiserMapGroup_Fillmore, kActRaiserNonActionMap_Fillmore, 1));
}

static void TestLivePickerPredicate(void) {
  memset(g_ram, 0, sizeof(g_ram));
  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Kasandora;

  CHECK(!ActRaiser_SimMapPickerActive());
  ActRaiser_WriteWramMirror16(kActRaiserWram_SimMapPickerFlag, 1);
  CHECK(ActRaiser_SimMapPickerActive());
  ActRaiser_WriteWramMirror16(kActRaiserWram_SimMapPickerFlag, 0x8000);
  CHECK(ActRaiser_SimMapPickerActive());

  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
  CHECK(!ActRaiser_SimMapPickerActive());
}

int main(void) {
  TestSimulationTownScope();
  TestPurePickerPredicate();
  TestLivePickerPredicate();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("actraiser_game_test: PASS");
  return 0;
}
