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

static void TestActionMapDomain(void) {
  static const uint8_t kExpectedLastMap[] = { 0, 4, 8, 6, 7, 8, 8, 8 };
  CHECK(!ActRaiser_IsActionMapGroup(0));
  CHECK(!ActRaiser_IsActionMapGroup(8));
  for (uint8_t group = 1; group <= 7; group++) {
    CHECK(ActRaiser_ActionMapLast(group) == kExpectedLastMap[group]);
    CHECK(!ActRaiser_IsActionMap(group, 0));
    CHECK(ActRaiser_IsActionMap(group, 1));
    CHECK(ActRaiser_IsActionMap(group, kExpectedLastMap[group]));
    CHECK(!ActRaiser_IsActionMap(
        group, (uint8_t)(kExpectedLastMap[group] + 1)));
  }
  CHECK(ActRaiser_ActionMapLast(0) == 0);
  CHECK(ActRaiser_ActionMapLast(8) == 0);
}

static void TestSimSpriteRangePolicy(void) {
  ActRaiserSimSpriteRangePolicy policy =
      ActRaiser_ResolveSimSpriteRangePolicy(true, 256, false, 0);
  CHECK(policy.real_oam_horizontal == 0);
  CHECK(policy.extended_horizontal == 256);
  CHECK(policy.extended_vertical == 256);
  CHECK(policy.lifetime == 256);

  policy = ActRaiser_ResolveSimSpriteRangePolicy(true, 64, true, 120);
  CHECK(policy.real_oam_horizontal == 120);
  CHECK(policy.extended_horizontal == 120);
  CHECK(policy.extended_vertical == 64);
  CHECK(policy.lifetime == 64);

  policy = ActRaiser_ResolveSimSpriteRangePolicy(false, 256, true, 72);
  CHECK(policy.real_oam_horizontal == 72);
  CHECK(policy.extended_horizontal == 72);
  CHECK(policy.extended_vertical == 0);
  CHECK(policy.lifetime == 0);
}

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

static void TestActionEntryActivationPolicy(void) {
  static const uint16 arrival_handlers[] = {
    kActRaiserPlayerHandler_ArrivalApproach,
    kActRaiserPlayerHandler_ArrivalTransformFirst,
    kActRaiserPlayerHandler_ArrivalTransformFinal,
  };

  for (unsigned i = 0;
       i < sizeof(arrival_handlers) / sizeof(arrival_handlers[0]); i++) {
    CHECK(ActRaiser_PlayerArrivalAnimationActive(arrival_handlers[i]));
    CHECK(!ActRaiser_ShouldUseWideActionActivation(
        1, arrival_handlers[i]));
  }

  CHECK(!ActRaiser_PlayerArrivalAnimationActive(
      kActRaiserPlayerHandler_GroundControl));
  CHECK(ActRaiser_ShouldUseWideActionActivation(
      1, kActRaiserPlayerHandler_GroundControl));
  /* Normal movement handlers remain widened after the handoff. */
  CHECK(ActRaiser_ShouldUseWideActionActivation(1, 0x9884));
  /* The fidelity switch still wins regardless of player lifecycle. */
  CHECK(!ActRaiser_ShouldUseWideActionActivation(
      0, kActRaiserPlayerHandler_GroundControl));

  /* The fitted wide camera is 120 at Fillmore's left edge, but $02:B030's
   * authentic camera for the arrival subject at world x=80 is still zero. */
  CHECK(ActRaiser_AuthenticActionCameraX(80, 768) == 0);
  CHECK(ActRaiser_AuthenticActionCameraX(128, 768) == 0);
  CHECK(ActRaiser_AuthenticActionCameraX(248, 768) == 120);
  CHECK(ActRaiser_AuthenticActionCameraX(500, 768) == 372);
  CHECK(ActRaiser_AuthenticActionCameraX(1000, 768) == 512);
  CHECK(ActRaiser_AuthenticActionCameraX(80, 224) == 0);

  /* The vertical activation exception is an exact room + retained spawn
   * source + loaded graphics tuple. Neither priority nor a visually similar
   * composition is part of the identity. */
  CHECK(ActRaiser_IsAitosStatueFireActor(
      kActRaiserMapGroup_Aitos, 6, 0xD5B1, 0x4000, 0x7E));
  CHECK(ActRaiser_IsAitosStatueFireActor(
      kActRaiserMapGroup_Aitos, 6, 0xD5C0, 0x4000, 0x7E));
  CHECK(!ActRaiser_IsAitosStatueFireActor(
      kActRaiserMapGroup_Aitos, 5, 0xD5B1, 0x4000, 0x7E));
  CHECK(!ActRaiser_IsAitosStatueFireActor(
      kActRaiserMapGroup_Aitos, 6, 0xD5B0, 0x4000, 0x7E));
  CHECK(!ActRaiser_IsAitosStatueFireActor(
      kActRaiserMapGroup_Aitos, 6, 0xD5B1, 0x5000, 0x7E));
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
  TestActionMapDomain();
  TestSimSpriteRangePolicy();
  TestSimulationTownScope();
  TestActionEntryActivationPolicy();
  TestPurePickerPredicate();
  TestLivePickerPredicate();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("actraiser_game_test: PASS");
  return 0;
}
