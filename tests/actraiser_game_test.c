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

static void TestSimulationHourglassScanRange(void) {
  /* Ordinary town HUD: phase $EC in the first four slots. */
  static const uint16 kFixedOam[] = {
    0x0B94, 0x31EC, 0x0B9B, 0x71EC,
    0x1394, 0x31FC, 0x139B, 0x71FC,
  };
  static const uint8 kFixedHighOam[] = { 0 };

  /* Exact leading OAM words/high bits from gf61067 in
   * runs/20260810-231616. Menu sprites occupy slots 0-10; phase $EF begins at
   * slot 11 and retains the same four-small-sprite footprint. */
  static const uint16 kMenuOam[] = {
    0x2D20, 0x3D20, 0x3D20, 0x3B6C, 0x3D28, 0x7B6C,
    0x4520, 0x3B7C, 0x4528, 0x7B7C, 0x3D32, 0x3D6E,
    0x3D42, 0x3F80, 0x4D20, 0x3F60, 0x5D20, 0x3F4A,
    0x6D20, 0x3D2E, 0x7D20, 0x3D44,
    0x0B94, 0x31EF, 0x0B9B, 0x71EF,
    0x1394, 0x31FF, 0x139B, 0x71FF,
  };
  static const uint8 kMenuHighOam[] = { 0x02, 0xA8, 0x2A, 0x80 };
  uint8 malformed_high_oam[sizeof(kMenuHighOam)];

  CHECK(ActRaiser_FindSimulationHourglass(
      kFixedOam, kFixedHighOam,
      (int)(sizeof(kFixedOam) / sizeof(kFixedOam[0]) / 2)) == 0);
  CHECK(ActRaiser_FindSimulationHourglass(
      kMenuOam, kMenuHighOam,
      (int)(sizeof(kMenuOam) / sizeof(kMenuOam[0]) / 2)) == 11);

  /* A size/X-high bit on any companion invalidates the fixed-screen shape. */
  memcpy(malformed_high_oam, kMenuHighOam, sizeof(malformed_high_oam));
  malformed_high_oam[3] |= 0x02;  /* slot 12 size bit */
  CHECK(ActRaiser_FindSimulationHourglass(
      kMenuOam, malformed_high_oam,
      (int)(sizeof(kMenuOam) / sizeof(kMenuOam[0]) / 2)) == -1);
  CHECK(ActRaiser_FindSimulationHourglass(NULL, NULL, 0) == -1);
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
    const uint8 quad_x[4]    = { 0x94, 0x9C, 0x94, 0x9C };
    const uint8 quad_y[4]    = { 0x0B, 0x0B, 0x13, 0x13 };
    const uint8 quad_attr[4] = { 0x39, 0x79, 0x39, 0x79 };
    for (int i = 0; i < kActRaiserSkyPalaceMagicQuadOamCount; i++) {
      uint8 x = 0, y = 0, attr = 0;
      ActRaiser_SkyPalaceMagicQuadSlot(i, &x, &y, &attr);
      CHECK(x == quad_x[i]);
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

/* Exact leading OAM words from the two user snapshots in
 * runs/20260808-214848. With no dialog, Fire is the first allocation in slots
 * 0-3; the menu/dialog layout owns slots 0-5 and moves the same icon to 6-9.
 * Starting the scan at the formerly observed slot 6 therefore worked only in
 * the second state. */
static void TestSkyPalaceMagicIconScanRange(void) {
  static const uint16 kNoDialogOam[] = {
    0x0B94, 0x3967, 0x0B9C, 0x7967,
    0x1394, 0x3977, 0x139C, 0x7977,
    0x6878, 0x2002,
  };
  static const uint8 kNoDialogHighOam[] = { 0x00, 0x02 };
  static const uint16 kDialogOam[] = {
    0x2D20, 0x3920, 0x2D32, 0x3F22, 0x2D42, 0x3F24,
    0x3D20, 0x3D28, 0x4D20, 0x3D2E, 0x5D20, 0x3D44,
    0x0B94, 0x3967, 0x0B9C, 0x7967, 0x1394, 0x3977,
    0x139C, 0x7977, 0x6778, 0x2000,
  };
  static const uint8 kDialogHighOam[] = { 0xAA, 0x0A, 0xA0 };
  /* Stardust's captured one-large-sprite form at slot 6. The preceding six
   * zeroed slots deliberately prove the scanner decodes slot 6's size bit
   * from high-OAM byte 1, bit 5 rather than relying on Fire's small form. */
  static const uint16 kWholeIconOam[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x0B94, 0x3984,
  };
  static const uint8 kWholeIconHighOam[] = { 0x00, 0x20 };
  static const uint16 kNoIconOam[] = { 0, 0 };
  static const uint8 kNoIconHighOam[] = { 0 };
  uint16 malformed_oam[
      sizeof(kNoDialogOam) / sizeof(kNoDialogOam[0])];
  uint8 malformed_high_oam[
      sizeof(kNoDialogHighOam) / sizeof(kNoDialogHighOam[0])];
  const int kLarge16 = 16;
  int slot = -1, count = 0;

  CHECK(ActRaiser_FindSkyPalaceMagicIcon(
      kNoDialogOam, kNoDialogHighOam,
      (int)(sizeof(kNoDialogOam) / sizeof(kNoDialogOam[0]) / 2),
      kLarge16, &slot, &count));
  CHECK(slot == 0);
  CHECK(count == kActRaiserSkyPalaceMagicQuadOamCount);

  /* A lead-slot match must not claim three unrelated followers. */
  memcpy(malformed_oam, kNoDialogOam, sizeof(malformed_oam));
  malformed_oam[2] = (uint16)((malformed_oam[2] & 0xFF00) | 0x009D);
  CHECK(!ActRaiser_FindSkyPalaceMagicIcon(
      malformed_oam, kNoDialogHighOam,
      (int)(sizeof(malformed_oam) / sizeof(malformed_oam[0]) / 2),
      kLarge16, &slot, &count));

  memcpy(malformed_high_oam, kNoDialogHighOam,
         sizeof(malformed_high_oam));
  malformed_high_oam[0] |= 0x08;  /* slot 1 large bit */
  CHECK(!ActRaiser_FindSkyPalaceMagicIcon(
      kNoDialogOam, malformed_high_oam,
      (int)(sizeof(kNoDialogOam) / sizeof(kNoDialogOam[0]) / 2),
      kLarge16, &slot, &count));

  slot = -1;
  count = 0;
  CHECK(ActRaiser_FindSkyPalaceMagicIcon(
      kDialogOam, kDialogHighOam,
      (int)(sizeof(kDialogOam) / sizeof(kDialogOam[0]) / 2),
      kLarge16, &slot, &count));
  CHECK(slot == 6);
  CHECK(count == kActRaiserSkyPalaceMagicQuadOamCount);

  slot = -1;
  count = 0;
  CHECK(ActRaiser_FindSkyPalaceMagicIcon(
      kWholeIconOam, kWholeIconHighOam,
      (int)(sizeof(kWholeIconOam) / sizeof(kWholeIconOam[0]) / 2),
      kLarge16, &slot, &count));
  CHECK(slot == 6);
  CHECK(count == kActRaiserSkyPalaceMagicWholeOamCount);

  slot = 99;
  count = 99;
  CHECK(!ActRaiser_FindSkyPalaceMagicIcon(
      kNoIconOam, kNoIconHighOam, 1, kLarge16, &slot, &count));
  CHECK(slot == -1);
  CHECK(count == 0);
}

int main(void) {
  TestSimSpriteRangePolicy();
  TestSimulationTownScope();
  TestPurePickerPredicate();
  TestLivePickerPredicate();
  TestSimulationHourglassScanRange();
  TestSkyPalaceMagicIconShapes();
  TestSkyPalaceMagicIconScanRange();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  puts("actraiser_game_test: PASS");
  return 0;
}
