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

/* Captured OAM, one snapshot per spell, from runs/20260806-232552 with all four
 * spells unlocked in the Sky Palace. These are the real bytes the ROM emitted,
 * not a reconstruction: slot 6 onward, x / y / attr / size-bit, read out of
 * snap_0{0,1,2,3}.oam.bin + .highoam.bin.
 *
 * Magical Fire spends four small slots on the icon; Stardust, Aura and Light
 * spend one large slot. Recognising only the first shape is what left the other
 * three drawing at their authentic centre-screen X while the widescreen HUD
 * moved out from under them. */
static void TestSkyPalaceMagicIconShapes(void) {
  const int kLarge16 = 16;

  /* snap_00, selected_magic=1 (Magical Fire): slots 6..9 small, tiles
   * $67/$67/$77/$77, attrs $39/$79/$39/$79. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(148, 0x0B, 0x39, 0, kLarge16) ==
        kActRaiserSkyPalaceMagicQuadOamCount);
  {
    const uint8 quad_y[4]    = { 0x0B, 0x0B, 0x13, 0x13 };
    const uint8 quad_attr[4] = { 0x39, 0x79, 0x39, 0x79 };
    for (int i = 0; i < kActRaiserSkyPalaceMagicQuadOamCount; i++) {
      uint8 y = 0, attr = 0;
      ActRaiser_SkyPalaceMagicQuadSlot(i, &y, &attr);
      CHECK(y == quad_y[i]);
      CHECK(attr == quad_attr[i]);
    }
  }

  /* snap_01/02/03, selected_magic=2/3/4: ONE large slot 6, tile $84/$86/$88,
   * attr $39. The tile differs per spell and is deliberately not part of the
   * signature — position, attribute and size are. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(148, 0x0B, 0x39, 1, kLarge16) ==
        kActRaiserSkyPalaceMagicWholeOamCount);

  /* A large sprite that OBSEL resolves to something other than 16x16 is not
   * this icon: present.c projects a fixed 16x16 chunk for whatever we promote. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(148, 0x0B, 0x39, 1, 32) == 0);

  /* Rejections, all measured from the same snapshots. The magic-select row at
   * y=109 reuses the very same tiles and attribute one row down (snap_01 slot
   * 11: x=168 y=109 tile=$84 attr=$39, snap_00 slot 10: x=152 y=109 attr=$39),
   * so y and x are what keep the HUD scan off it. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(168, 109, 0x39, 1, kLarge16) == 0);
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(152, 109, 0x39, 0, kLarge16) == 0);
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(148, 109, 0x39, 1, kLarge16) == 0);
  /* The right half of Fire's quad never leads the icon. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(156, 0x0B, 0x79, 0, kLarge16) == 0);
  /* snap_00 slot 0: the ANGEL/status text block, wrong everything. */
  CHECK(ActRaiser_SkyPalaceMagicIconSlots(32, 45, 0x3D, 1, kLarge16) == 0);
}

int main(void) {
  TestSimulationTownScope();
  TestPurePickerPredicate();
  TestLivePickerPredicate();
  TestSkyPalaceMagicIconShapes();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("actraiser_game_test: PASS");
  return 0;
}
