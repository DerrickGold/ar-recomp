/* World-map underlay: ROM residency, the town window table, live-shadow
 * adoption policy, and the bake. The window table is the part that would fail
 * silently in play — a wrong origin puts a town on someone else's terrain and
 * still looks plausible — so its structural invariants are asserted here
 * rather than left to a visual checkpoint. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "sim_world_map.h"
#include "sim_world_map_rows.h"

static int s_failures;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

enum {
  /* Must cover kPaletteOffset + 512, not just the tile art. At 0x80000 the
   * palette write in BuildRom landed ~400 KB PAST the allocation (heap
   * overflow), so every palette entry stayed zero and every tile baked to
   * blue 0 — which made colour comparisons vacuously equal and hid real
   * adoption/restore differences. Pre-existing; found while adding the F1
   * row-policy tests, whose assertions distinguish tiles by colour. */
  kRomSize = 0x100000,
  kTilemapOffset = 0x033341,
  kTilesOffset = 0x070000,
  kPaletteOffset = 0x0E3F93,
  kShadowWram = 0xC000,
  kWramSize = 0x20000,
};

/* Tile t is painted entirely with colour index t, and palette entry i is a
 * pure blue ramp, so a baked pixel names the tile it came from. */
static uint8_t *BuildRom(void) {
  uint8_t *rom = calloc(1, kRomSize);
  for (int tile = 0; tile < 256; tile++)
    memset(rom + kTilesOffset + tile * 64, (uint8_t)tile, 64);
  for (int i = 0; i < 256; i++) {
    uint16_t bgr555 = (uint16_t)((i & 0x1F) << 10);  /* blue = i & 31 */
    rom[kPaletteOffset + i * 2] = (uint8_t)(bgr555 & 0xFF);
    rom[kPaletteOffset + i * 2 + 1] = (uint8_t)(bgr555 >> 8);
  }
  for (int i = 0; i < kSimWorldMapBytes; i++)
    rom[kTilemapOffset + i] = (uint8_t)(i & 0x7F);
  return rom;
}

/* Colour BuildRom assigns to a tile: tile t is painted with palette index t,
 * and entry t is a pure blue ramp, so this reproduces ExpandBgr555 for that
 * ramp (red = green = 0). Independent of the module's bake path on purpose. */
static uint32_t ColorForTile(uint8_t tile) {
  uint32_t blue = (uint32_t)((tile & 0x1F) << 3) | ((tile & 0x1F) >> 2);
  return 0xFF000000u | blue;
}

/* A full, unconditional bake of `tilemap`, computed here rather than by the
 * module. Comparing the module's dirty-tracked bake against this catches both
 * failure modes: a tile that should have been re-baked but was not, and a
 * non-dirty tile left as whatever garbage the caller's buffer held. */
static void ReferenceFullBake(const uint8_t *tilemap, uint32_t *out) {
  for (int tile_y = 0; tile_y < kSimWorldMapTiles; tile_y++)
    for (int tile_x = 0; tile_x < kSimWorldMapTiles; tile_x++) {
      uint32_t color = ColorForTile(tilemap[tile_y * kSimWorldMapTiles + tile_x]);
      for (int row = 0; row < kSimWorldMapTilePixels; row++) {
        uint32_t *p = out +
            (size_t)(tile_y * kSimWorldMapTilePixels + row) * kSimWorldMapPixels +
            tile_x * kSimWorldMapTilePixels;
        for (int col = 0; col < kSimWorldMapTilePixels; col++) p[col] = color;
      }
    }
}

/* Mirror the module's adoption policy against a test-owned expected tilemap so
 * the reference bake can be computed from the same state the module holds: a
 * town frame adopts rows 8+, only a world-map frame adopts rows 0-7. */
static void ApplyRefreshToExpected(uint8_t *expected, const uint8_t *shadow,
                                   uint8_t map_number) {
  int first_row = (map_number == kActRaiserNonActionMap_WorldMap)
      ? 0 : kSimWorldMapVolatileRows;
  size_t offset = (size_t)first_row * kSimWorldMapTiles;
  memcpy(expected + offset, shadow + offset, kSimWorldMapBytes - offset);
}

static void TestUnavailableRom(void) {
  uint8_t tiny[16] = { 0 };
  CHECK(!SimWorldMap_Init(tiny, sizeof(tiny)));
  CHECK(!SimWorldMap_Available());
  CHECK(SimWorldMap_Serial() == 0);
  int x = -1, y = -1;
  /* The window table is static data, so it answers even with no ROM; what a
   * missing ROM must suppress is the serial, which is what gates drawing. */
  CHECK(SimWorldMap_OriginForTown(1, &x, &y));
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(!SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  free(pixels);
}

/* Every origin lands on a multiple of 16, the six windows fit inside the map
 * and never overlap. No other assignment of towns to the world map's six
 * cathedral icons has those properties, which is what pins this table. */
static void TestTownWindows(void) {
  int x[6], y[6];
  for (int town = 1; town <= 6; town++) {
    CHECK(SimWorldMap_OriginForTown((uint8_t)town, &x[town - 1],
                                    &y[town - 1]));
    CHECK(x[town - 1] % 16 == 0 && y[town - 1] % 16 == 0);
    CHECK(x[town - 1] >= 0 &&
          x[town - 1] + kSimTownCells <= kSimWorldMapTiles);
    CHECK(y[town - 1] >= 0 &&
          y[town - 1] + kSimTownCells <= kSimWorldMapTiles);
  }
  for (int a = 0; a < 6; a++)
    for (int b = a + 1; b < 6; b++) {
      bool disjoint_x = x[a] + kSimTownCells <= x[b] ||
          x[b] + kSimTownCells <= x[a];
      bool disjoint_y = y[a] + kSimTownCells <= y[b] ||
          y[b] + kSimTownCells <= y[a];
      CHECK(disjoint_x || disjoint_y);
    }
  CHECK(!SimWorldMap_OriginForTown(0, NULL, NULL));
  CHECK(!SimWorldMap_OriginForTown(7, NULL, NULL));

  /* Fillmore is the origin the whole table is anchored on: its cathedral
   * cell (13,13) under the world icon at (93,61). */
  CHECK(x[0] == 80 && y[0] == 48);
  /* Bloodpool abuts Fillmore, so its east edge is Fillmore's west edge —
   * this adjacency is the whole point of drawing live neighbours. */
  CHECK(x[1] + kSimTownCells == x[0] && y[1] == y[0]);
}

static void TestShadowAdoptionPolicy(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));
  uint32_t initial = SimWorldMap_Serial();
  CHECK(initial != 0);

  uint8_t *wram = calloc(1, kWramSize);
  memset(wram + kShadowWram, 0xAB, kSimWorldMapBytes);

  /* An action stage shares the address but not the meaning: never adopted. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);
  CHECK(SimWorldMap_Serial() == initial);

  /* In a town, rows 8+ are adopted and rows 0-7 are left at the baseline. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  uint32_t after_town = SimWorldMap_Serial();
  CHECK(after_town != initial);
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  /* Row 0 still shows the ROM tilemap's tile 0; row 8 shows shadow tile
   * $AB. Blue channel carries the tile index in the synthetic palette. */
  CHECK((pixels[0] & 0xFF) == 0);
  uint32_t row8 = pixels[(size_t)kSimWorldMapVolatileRows *
                         kSimWorldMapTilePixels * kSimWorldMapPixels];
  CHECK((row8 & 0xFF) == ((0xAB & 0x1F) << 3 | (0xAB & 0x1F) >> 2));

  /* Idempotent: an unchanged shadow must not churn the serial, because the
   * present thread rebuilds a 1024x1024 texture on every change. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Serial() == after_town);

  /* Only the world map itself may publish rows 0-7. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  CHECK(SimWorldMap_Serial() != after_town);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK((pixels[0] & 0xFF) == ((0xAB & 0x1F) << 3 | (0xAB & 0x1F) >> 2));

  free(pixels);
  free(wram);
  free(rom);
}

/* F1 (2026-07-26 handback): the corrupt top band, and per the tester it does NOT
 * reproduce every run. Rows 0-7 have exactly two possible histories — untouched
 * ROM baseline, or shadow bytes adopted during a world-map visit — so the two
 * candidate repairs are genuinely different and only one can be right. Both are
 * implemented; these tests pin each one's contract so the on-device A/B is
 * comparing what it claims to compare.
 *
 * The shared setup mirrors TestShadowAdoptionPolicy: shadow filled with $AB, so
 * "row shows $AB" means adopted and "row shows tile 0" means baseline. */
static void TestRowPolicyRestoreBaseline(void) {
  uint8_t *rom = BuildRom();
  uint8_t *wram = calloc(1, kWramSize);
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  /* The synthetic palette is a 5-bit blue ramp, so a tile's colour only carries
   * `tile & 0x1F` — and row 0's baseline (BuildRom writes i & 0x7F) covers all 32
   * of those values. So no shadow byte is distinguishable from the baseline by
   * colour alone at an arbitrary position; the assertions below must compare each
   * tile against ITS OWN expected colour via ColorForTile, never against a single
   * "shadow colour". Getting this wrong made an earlier revision of this test
   * pass only on an incremental build. */
  const uint8_t kShadowByte = 0xDD;
  memset(wram + kShadowWram, kShadowByte, kSimWorldMapBytes);

  /* Reproduce the bug first, under the CURRENT policy: visit the world map so
   * rows 0-7 adopt the shadow, then return to a town. Today the strip keeps the
   * adopted bytes — which is the garbage in the screenshot when that region is
   * not terrain. */
  SimWorldMap_SetRowPolicy(kSimWorldRows_Legacy);
  CHECK(SimWorldMap_Init(rom, kRomSize));
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1); /* back to town */
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  /* Every row-0 tile now shows the ADOPTED shadow byte, not its own baseline. */
  CHECK(pixels[0] == ColorForTile(kShadowByte));
  CHECK(pixels[5 * kSimWorldMapTilePixels] == ColorForTile(kShadowByte));
  CHECK(pixels[100 * kSimWorldMapTilePixels] == ColorForTile(kShadowByte));

  /* RestoreBaseline repairs it — including retroactively. The policy is switched
   * AFTER the bad adoption, exactly as toggling the setting mid-session would,
   * so the restore must not depend on having been on from the start. */
  SimWorldMap_SetRowPolicy(kSimWorldRows_RestoreBaseline);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  /* Each tile is back to ITS OWN baseline, which BuildRom set to (index & 0x7F).
   * Tile 0's baseline is 0 — the same thing a ZEROED snapshot buffer would
   * restore — so tile 0 alone cannot distinguish a real snapshot from a missing
   * one. Tiles 5 and 100 have non-zero baselines and do: dropping the Init-time
   * memcpy restores zeros and fails these two. */
  CHECK(pixels[0] == ColorForTile(0));
  CHECK(pixels[5 * kSimWorldMapTilePixels] == ColorForTile(5));
  CHECK(pixels[100 * kSimWorldMapTilePixels] == ColorForTile(100));
  /* Rows 8+ are untouched by this policy: it must repair the strip without
   * discarding the live development that makes the underlay worth drawing. */
  uint32_t row8 = pixels[(size_t)kSimWorldMapVolatileRows *
                         kSimWorldMapTilePixels * kSimWorldMapPixels];
  CHECK(row8 == ColorForTile(kShadowByte));

  /* Steady state: once repaired, further town frames are a no-op. A restore that
   * bumped the serial every frame would rebuild a 1024x1024 texture forever. */
  uint32_t settled = SimWorldMap_Serial();
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Serial() == settled);
  CHECK(SimWorldMap_RestoreVolatileRows() == 0);

  /* On the world-map screen the live rows ARE the authentic view, so even this
   * policy must show them rather than overwriting the screen the player is
   * looking at. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[0] == ColorForTile(kShadowByte));

  free(pixels);
  free(wram);
  free(rom);
  SimWorldMap_SetRowPolicy(kSimWorldRows_Legacy);
}

/* The coherence gate — F2's half, and the reason F1 and F2 are one defect.
 *
 * ShadowIsTrustworthy tests the map identity for THIS frame, but the corruption
 * it guards against is durable in the buffer. It correctly refuses adoption while
 * an act owns 7E:C000, then permits it on the first town frame afterwards — when
 * the act's bytes are still sitting there. Adopting then is unrecoverable: the
 * tilemap is mutated in place and the diff only takes bytes that DIFFER, so
 * tilemap == shadow == garbage from then on with no serial bump to re-bake.
 *
 * This test reproduces that sequence under the legacy policy first, then shows
 * the gate refusing it. */
static void TestRowPolicyCoherenceGate(void) {
  uint8_t *rom = BuildRom();
  uint8_t *wram = calloc(1, kWramSize);
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  /* Stand in for what an act leaves behind: not terrain, and (crucially) a
   * different value from the baseline so adoption is observable. */
  const uint8_t kActGarbage = 0x9E;
  const uint8_t kRealTerrain = 0x47;

  /* --- the defect, under today's policy ------------------------------------ */
  SimWorldMap_SetRowPolicy(kSimWorldRows_Legacy);
  CHECK(SimWorldMap_Init(rom, kRomSize));
  /* A world-map frame establishes real terrain across the whole map. */
  memset(wram + kShadowWram, kRealTerrain, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  /* An act runs and leaves its own data in the same WRAM. Nothing is adopted
   * while it owns the buffer — that part already works. */
  memset(wram + kShadowWram, kActGarbage, kSimWorldMapBytes);
  uint32_t during_act = SimWorldMap_Serial();
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);
  CHECK(SimWorldMap_Serial() == during_act);
  /* Back in town. The buffer STILL holds the act's bytes, but the map identity
   * now says "town", so legacy adopts them wholesale — including rows 8+. */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  size_t row64 = (size_t)64 * kSimWorldMapTilePixels * kSimWorldMapPixels;
  CHECK(pixels[row64] == ColorForTile(kActGarbage));  /* F2, reproduced */

  /* --- the gate refuses it ------------------------------------------------- */
  SimWorldMap_SetRowPolicy(kSimWorldRows_CoherenceGate);
  CHECK(SimWorldMap_Init(rom, kRomSize));
  memset(wram + kShadowWram, kRealTerrain, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[row64] == ColorForTile(kRealTerrain));  /* live map adopted */

  memset(wram + kShadowWram, kActGarbage, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);   /* the act */
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);  /* back to town */
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  /* The act's bytes are refused. Rows 8+ show the ROM baseline — the undeveloped
   * world, which is wrong-but-plausible rather than garbage. */
  CHECK(pixels[row64] != ColorForTile(kActGarbage));
  CHECK(pixels[row64] == ColorForTile((uint8_t)((64 * kSimWorldMapTiles) & 0x7F)));

  /* It stays refused while the buffer is unverified, however many town frames
   * pass — the gate is not a one-frame skip. */
  for (int i = 0; i < 3; i++)
    SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[row64] != ColorForTile(kActGarbage));

  /* A world-map frame is the evidence the buffer was rebuilt, so adoption
   * resumes — otherwise the gate would permanently freeze the underlay at the
   * ROM baseline and no development would ever show again. */
  memset(wram + kShadowWram, kRealTerrain, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[row64] == ColorForTile(kRealTerrain));

  /* Steady state: no serial churn once the restore has nothing left to do. A
   * gate that bumped every frame would rebuild a 1024x1024 texture forever. */
  memset(wram + kShadowWram, kActGarbage, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);  /* restores */
  uint32_t settled = SimWorldMap_Serial();
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Serial() == settled);

  free(pixels);
  free(wram);
  free(rom);
  SimWorldMap_SetRowPolicy(kSimWorldRows_Legacy);
}

static void TestRowPolicyTrustShadow(void) {
  uint8_t *rom = BuildRom();
  uint8_t *wram = calloc(1, kWramSize);
  uint32_t *pixels = calloc(kSimWorldMapPixels * kSimWorldMapPixels, 4);
  const uint8_t kShadowByte = 0xC6;   /* again distinct from every other test */
  memset(wram + kShadowWram, kShadowByte, kSimWorldMapBytes);

  /* TrustShadow adopts rows 0-7 on a town map too — no world-map visit needed,
   * which is the whole difference from legacy. */
  SimWorldMap_SetRowPolicy(kSimWorldRows_TrustShadow);
  CHECK(SimWorldMap_Init(rom, kRomSize));
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));
  CHECK(pixels[0] == ColorForTile(kShadowByte));

  /* Still gated on map identity: an action stage shares the address but not the
   * meaning, so trusting the shadow must not mean trusting it everywhere. */
  uint32_t before = SimWorldMap_Serial();
  memset(wram + kShadowWram, 0x11, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_Fillmore, 1);
  CHECK(SimWorldMap_Serial() == before);

  free(pixels);
  free(wram);
  free(rom);
  SimWorldMap_SetRowPolicy(kSimWorldRows_Legacy);
}

/* The policy arithmetic itself, independent of any ROM or bake. */
static void TestRowPolicyPure(void) {
  /* Legacy and RestoreBaseline both skip while adopting; TrustShadow does not. */
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_Legacy, false, 8) == 8);
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_RestoreBaseline, false, 8) == 8);
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_TrustShadow, false, 8) == 0);
  /* On the world-map screen every policy adopts from row 0. */
  for (int p = 0; p <= 3; p++)
    CHECK(SimWorldRows_FirstAdoptedRow((SimWorldRowPolicy)p, true, 8) == 0);
  /* CoherenceGate keeps the rows-0-7 skip on a town map (the Sky Palace metatile
   * page lands at 7E:C200 = rows 4-5), and still adopts from 0 on the world map. */
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_CoherenceGate, false, 8) == 8);
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_CoherenceGate, true, 8) == 0);

  /* The incoherence predicate. Only CoherenceGate ever fires, so the other
   * policies stay honest A/B comparisons against today's behaviour. */
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_Legacy, true, false));
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_RestoreBaseline, true, false));
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_TrustShadow, true, false));
  /* Trusted map, no rebuild seen -> the buffer is unverified, refuse it. */
  CHECK(SimWorldRows_ShadowIsIncoherent(kSimWorldRows_CoherenceGate, true, false));
  /* Rebuild seen -> coherent, adopt normally. Without this the underlay would
   * freeze at the ROM baseline and no development would ever appear. */
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_CoherenceGate, true, true));
  /* An untrusted frame adopts nothing anyway, so it is never "incoherent". */
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_CoherenceGate, false, false));
  CHECK(!SimWorldRows_ShadowIsIncoherent(kSimWorldRows_CoherenceGate, false, true));

  /* Only the whole-map policy needs the full baseline retained. A policy that
   * restored rows it never snapshotted would silently restore zeros. */
  CHECK(SimWorldRows_NeedsFullBaseline(kSimWorldRows_CoherenceGate));
  CHECK(!SimWorldRows_NeedsFullBaseline(kSimWorldRows_RestoreBaseline));
  CHECK(!SimWorldRows_NeedsFullBaseline(kSimWorldRows_Legacy));
  CHECK(!SimWorldRows_NeedsFullBaseline(kSimWorldRows_TrustShadow));

  /* Only RestoreBaseline restores, and never on the world-map screen. */
  CHECK(!SimWorldRows_ShouldRestoreBaseline(kSimWorldRows_Legacy, false));
  CHECK(!SimWorldRows_ShouldRestoreBaseline(kSimWorldRows_TrustShadow, false));
  CHECK(SimWorldRows_ShouldRestoreBaseline(kSimWorldRows_RestoreBaseline, false));
  CHECK(!SimWorldRows_ShouldRestoreBaseline(kSimWorldRows_RestoreBaseline, true));
  /* The gate does its repair through the incoherence path, not this one. */
  CHECK(!SimWorldRows_ShouldRestoreBaseline(kSimWorldRows_CoherenceGate, false));
  /* Degenerate inputs must not produce a negative row or an OOB span. */
  CHECK(SimWorldRows_FirstAdoptedRow(kSimWorldRows_Legacy, false, -1) == 0);
  size_t begin = 123, end = 456;
  SimWorldRows_BaselineSpan(8, 128, 16384, &begin, &end);
  CHECK(begin == 0 && end == 1024);
  SimWorldRows_BaselineSpan(0, 128, 16384, &begin, &end);
  CHECK(begin == 0 && end == 0);
  /* A bad constant must clamp rather than walk off the tilemap. */
  SimWorldRows_BaselineSpan(9999, 128, 16384, &begin, &end);
  CHECK(end == 16384);
  /* Names are used in logs and the settings UI; never NULL, including for a
   * value outside the enum (which SetRowPolicy coerces to legacy). */
  CHECK(SimWorldRows_PolicyName(kSimWorldRows_Legacy) != NULL);
  CHECK(SimWorldRows_PolicyName((SimWorldRowPolicy)99) != NULL);
  CHECK(!strcmp(SimWorldRows_PolicyName(kSimWorldRows_RestoreBaseline),
                "restore-baseline"));
  CHECK(!strcmp(SimWorldRows_PolicyName(kSimWorldRows_TrustShadow),
                "trust-shadow"));
  CHECK(!strcmp(SimWorldRows_PolicyName(kSimWorldRows_CoherenceGate),
                "coherence-gate"));
}

/* The bake is fully opaque everywhere. There is deliberately no hole for the
 * town being played: punching one out left a black gap, because the hole is
 * the town's whole 512x512-pixel territory while its ground quad only draws
 * the window the camera can see. Overlap is a draw-order concern, not a bake
 * concern, and this asserts the bake keeps its half of that bargain. */
static void TestBakeIsFullyCovered(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));
  size_t count = (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *pixels = malloc(count * 4);
  memset(pixels, 0, count * 4);
  CHECK(SimWorldMap_Bake(pixels, kSimWorldMapPixels));

  size_t transparent = 0;
  for (size_t i = 0; i < count; i++)
    if ((pixels[i] >> 24) != 0xFF) transparent++;
  CHECK(transparent == 0);

  /* Every tile still bakes its own colour, including inside each town's
   * window -- that is the low-fidelity stand-in for off-screen territory. */
  for (int town = 1; town <= 6; town++) {
    int origin_x = 0, origin_y = 0;
    CHECK(SimWorldMap_OriginForTown((uint8_t)town, &origin_x, &origin_y));
    uint32_t pixel = pixels[(size_t)origin_y * kSimWorldMapTilePixels *
                            kSimWorldMapPixels +
                            origin_x * kSimWorldMapTilePixels];
    int tile = (origin_y * kSimWorldMapTiles + origin_x) & 0x7F;
    CHECK((pixel & 0xFF) == (uint32_t)((tile & 0x1F) << 3 | (tile & 0x1F) >> 2));
  }
  free(pixels);
  free(rom);
}

/* Per-tile dirty tracking (finding O2) must never change what a bake produces,
 * only how much work it does. The guardrail is the streaming-texture lock: the
 * caller hands the bake a write-only buffer of undefined content, so we bake
 * into a buffer deliberately pre-filled with garbage (0xAA) and assert the
 * result is byte-identical to a full reference bake. If the optimisation ever
 * baked only dirty tiles into that buffer, the non-dirty tiles would keep the
 * 0xAA garbage and these comparisons would fail. */
static void TestDirtyTrackingMatchesFullBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  size_t count = (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc(count * 4);
  uint32_t *reference = malloc(count * 4);

  /* The tilemap the module holds right after Init is BuildRom's `i & 0x7F`. */
  uint8_t *expected = malloc(kSimWorldMapBytes);
  for (int i = 0; i < kSimWorldMapBytes; i++)
    expected[i] = (uint8_t)(i & 0x7F);

  /* (2) First bake after Init: all tiles dirty, so a bake into garbage memory
   * must still reproduce the whole ROM tilemap. */
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  /* (1) A single-tile Refresh, then a bake into garbage, must match a full
   * bake of the new state — proving the 16383 non-dirty tiles were preserved
   * from the persistent image rather than left as the 0xAA the caller's
   * write-only lock would really contain. */
  uint8_t *wram = calloc(1, kWramSize);
  for (int i = 0; i < kSimWorldMapBytes; i++)
    wram[kShadowWram + i] = (uint8_t)(i & 0x7F);      /* start == baseline */
  /* Change exactly one tile in an adopted row (row 8, well clear of the
   * volatile rows 0-7) to a value the baseline never uses there. */
  size_t edited = (size_t)kSimWorldMapVolatileRows * kSimWorldMapTiles + 5;
  wram[kShadowWram + edited] = 0x33;
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  expected[edited] = 0x33;

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);
  /* And the edited tile really did land — a sanity check that the comparison
   * above is not vacuously passing on an unchanged image. */
  {
    size_t px = (size_t)(edited / kSimWorldMapTiles) * kSimWorldMapTilePixels *
                kSimWorldMapPixels +
                (edited % kSimWorldMapTiles) * kSimWorldMapTilePixels;
    CHECK(baked[px] == ColorForTile(0x33));
  }

  /* (3) A mixed sequence of several Refreshes, then one bake, must also match a
   * full bake of the final state. Includes a world-map frame so rows 0-7 move
   * too, exercising the volatile-row offset in the diff. */
  memset(wram + kShadowWram, 0xAB, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);        /* rows 8+ */
  ApplyRefreshToExpected(expected, wram + kShadowWram, 1);
  wram[kShadowWram + 100] = 0x07;
  wram[kShadowWram + 4000] = 0x71;
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction, 1);
  ApplyRefreshToExpected(expected, wram + kShadowWram, 1);
  memset(wram + kShadowWram, 0x5C, kSimWorldMapBytes);
  SimWorldMap_Refresh(wram, kActRaiserMapGroup_NonAction,
                      kActRaiserNonActionMap_WorldMap);              /* rows 0+ */
  ApplyRefreshToExpected(expected, wram + kShadowWram,
                         kActRaiserNonActionMap_WorldMap);

  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  ReferenceFullBake(expected, reference);
  CHECK(memcmp(baked, reference, count * 4) == 0);

  /* A bake with no intervening Refresh (nothing dirty) still emits the full
   * image, because the caller's buffer is fresh garbage every frame. */
  memset(baked, 0xAA, count * 4);
  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));
  CHECK(memcmp(baked, reference, count * 4) == 0);

  free(expected);
  free(wram);
  free(reference);
  free(baked);
  free(rom);
}

/* The blur mip must be a box average of the SAME image Bake emits, computed
 * from the module's persistent copy rather than from the caller's buffer.
 *
 * The predecessor built this mip by reading back the streaming-texture lock it
 * had just baked into — legal-looking on Metal, garbage on a Vulkan/Mesa
 * backend where the mapping is write-combined (that is the macOS-fine /
 * Steam-Deck-garbled split). The test that catches a reintroduction is one that
 * hands Downsample a destination full of garbage and byte-compares against an
 * independent reference: if the implementation ever reads its own destination,
 * or reads anything other than the baked image, the comparison fails.
 *
 * Also asserts the pitch is honored, since the destination's pitch comes from
 * SDL and need not equal the extent. */
static void TestDownsampleMatchesBake(void) {
  uint8_t *rom = BuildRom();
  CHECK(SimWorldMap_Init(rom, kRomSize));

  const int count = kSimWorldMapPixels * kSimWorldMapPixels;
  uint32_t *baked = malloc((size_t)count * 4);
  const int divisor = 4;
  const int extent = kSimWorldMapPixels / divisor;
  /* Over-wide destination: pitch deliberately != extent. */
  const int pitch = extent + 7;
  uint32_t *mip = malloc((size_t)pitch * extent * 4);
  uint32_t *reference = malloc((size_t)extent * extent * 4);

  CHECK(SimWorldMap_Bake(baked, kSimWorldMapPixels));

  /* Independent box average over the baked image. */
  for (int y = 0; y < extent; y++) {
    for (int x = 0; x < extent; x++) {
      uint32_t a = 0, r = 0, g = 0, b = 0;
      for (int sy = 0; sy < divisor; sy++) {
        for (int sx = 0; sx < divisor; sx++) {
          uint32_t texel =
              baked[(size_t)(y * divisor + sy) * kSimWorldMapPixels +
                    (size_t)x * divisor + sx];
          a += (texel >> 24) & 0xFF; r += (texel >> 16) & 0xFF;
          g += (texel >> 8) & 0xFF;  b += texel & 0xFF;
        }
      }
      const uint32_t taps = (uint32_t)(divisor * divisor);
      reference[(size_t)y * extent + x] =
          ((a / taps) << 24) | ((r / taps) << 16) | ((g / taps) << 8) |
          (b / taps);
    }
  }

  memset(mip, 0xAA, (size_t)pitch * extent * 4);
  CHECK(SimWorldMap_Downsample(mip, pitch, divisor));
  for (int y = 0; y < extent; y++) {
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);
  }

  /* Repeating it with nothing dirty must reproduce the same mip — the caller's
   * buffer is fresh garbage every frame, exactly as for Bake. */
  memset(mip, 0x5C, (size_t)pitch * extent * 4);
  CHECK(SimWorldMap_Downsample(mip, pitch, divisor));
  for (int y = 0; y < extent; y++) {
    CHECK(memcmp(mip + (size_t)y * pitch, reference + (size_t)y * extent,
                 (size_t)extent * 4) == 0);
  }

  /* Argument validation: a divisor that does not divide the extent, a
   * nonsensical divisor, a short pitch, and a NULL destination. */
  CHECK(!SimWorldMap_Downsample(mip, pitch, 3));
  CHECK(!SimWorldMap_Downsample(mip, pitch, 0));
  CHECK(!SimWorldMap_Downsample(mip, extent - 1, divisor));
  CHECK(!SimWorldMap_Downsample(NULL, pitch, divisor));

  free(reference);
  free(mip);
  free(baked);
  free(rom);
}

int main(void) {
  TestUnavailableRom();
  TestTownWindows();
  TestShadowAdoptionPolicy();
  TestRowPolicyPure();
  TestRowPolicyRestoreBaseline();
  TestRowPolicyTrustShadow();
  TestRowPolicyCoherenceGate();
  TestBakeIsFullyCovered();
  TestDirtyTrackingMatchesFullBake();
  TestDownsampleMatchesBake();
  SimWorldMap_Shutdown();
  printf("sim world map tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
