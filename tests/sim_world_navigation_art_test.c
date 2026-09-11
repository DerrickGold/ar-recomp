#include "sim/sim_world_navigation_art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim/sim_town_ground_art.h"
#include "sim/sim_town_terrain.h"

static int failures;

#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
            #expression);                                                  \
    failures++;                                                            \
  }                                                                        \
} while (0)

static void TestTownFeatherAndScale(void) {
  const size_t source_count =
      (size_t)kSimWorldMapPixels * kSimWorldMapPixels;
  const size_t output_count =
      (size_t)kSimWorldNavigationArtPixels * kSimWorldNavigationArtPixels;
  uint32_t *developed = malloc(source_count * sizeof(*developed));
  uint32_t *baseline = malloc(source_count * sizeof(*baseline));
  uint32_t *output = malloc(output_count * sizeof(*output));
  CHECK(developed != NULL && baseline != NULL && output != NULL);
  if (!developed || !baseline || !output) {
    free(output);
    free(baseline);
    free(developed);
    return;
  }
  for (size_t i = 0; i < source_count; i++) {
    developed[i] = UINT32_C(0xff607c20);
    baseline[i] = UINT32_C(0xff587418);
  }
  CHECK(SimWorldNavigationArt_Build(
      output, kSimWorldNavigationArtPixels,
      developed, kSimWorldMapPixels, baseline, kSimWorldMapPixels));

  int origin_x = 0, origin_y = 0;
  CHECK(SimWorldMap_OriginForTown(1, &origin_x, &origin_y));
  const int edge_x = origin_x * kSimWorldMapTilePixels;
  const int edge_y = origin_y * kSimWorldMapTilePixels + 64;
  const int centre_x = edge_x + kSimTownCells * kSimWorldMapTilePixels / 2;
  const int centre_y = origin_y * kSimWorldMapTilePixels +
      kSimTownCells * kSimWorldMapTilePixels / 2;
  const size_t edge = (size_t)(edge_y * 2) *
      kSimWorldNavigationArtPixels + (size_t)(edge_x * 2);
  const size_t centre = (size_t)(centre_y * 2) *
      kSimWorldNavigationArtPixels + (size_t)(centre_x * 2);
  CHECK(SimWorldNavigationArt_TownWeight(edge_x, edge_y) == 0.0f);
  CHECK(SimWorldNavigationArt_TownWeight(centre_x, centre_y) == 1.0f);
  CHECK(SimWorldNavigationArt_TownWeight(0, 0) == 1.0f);
  CHECK(output[edge] == baseline[0]);
  CHECK(output[centre] == developed[0]);
  /* A cleansed lake remains blue even at the very first pixel of the town;
   * semantic state changes must not be feathered with the pristine red lake. */
  for (size_t i = 0; i < source_count; i++) {
    developed[i] = UINT32_C(0xff183cc0);
    baseline[i] = UINT32_C(0xff982010);
  }
  CHECK(SimWorldNavigationArt_Build(
      output, kSimWorldNavigationArtPixels,
      developed, kSimWorldMapPixels, baseline, kSimWorldMapPixels));
  CHECK(output[edge] == developed[0]);
  CHECK(output[centre] == developed[0]);
  CHECK(!SimWorldNavigationArt_Build(
      NULL, kSimWorldNavigationArtPixels,
      developed, kSimWorldMapPixels, baseline, kSimWorldMapPixels));
  CHECK(!SimWorldNavigationArt_Build(
      output, kSimWorldNavigationArtPixels - 1,
      developed, kSimWorldMapPixels, baseline, kSimWorldMapPixels));
  free(output);
  free(baseline);
  free(developed);
}

static uint32_t ReferencePixel(const uint32_t *pixels, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= kSimWorldMapPixels) x = kSimWorldMapPixels - 1;
  if (y >= kSimWorldMapPixels) y = kSimWorldMapPixels - 1;
  return pixels[y * kSimWorldMapPixels + x];
}

static void TestNativeGroundComposition(void) {
  const int pitch = kSimWorldNavigationArtPixels + 3;
  const size_t count = (size_t)pitch * kSimWorldNavigationArtPixels;
  uint32_t *out = malloc(count * sizeof(*out));
  uint8_t *rom = calloc(0x100000, 1);
  SimWorldNavigationTownGround ground = {0};
  CHECK(out && rom);
  if (!out || !rom) { free(out); free(rom); return; }
  const uint32_t baseline = 0xFF102030u;
  for (size_t i = 0; i < count; i++) out[i] = baseline;
  /* Pure red ordinary terrain, blue river, green upright tree/mountain art. */
  rom[0xE3B95] = 31;
  rom[0xE3B98] = 0x7C;
  rom[0xE3B99] = 0xE0;
  rom[0xE3B9A] = 3;
  for (int tile = 0; tile < 256; tile++) {
    for (int q = 0; q < 4; q++)
      rom[0xC881B + tile * 8 + q * 2] = (uint8_t)tile;
    for (int y = 0; y < 8; y++) {
      const int index = tile == 0x41 || tile == 0x3A ? 2
          : tile == 0x78 || tile == 1 ? 3 : 1;
      rom[0x60000 + tile * 32 + y * 2] = index & 1 ? 255 : 0;
      rom[0x60001 + tile * 32 + y * 2] = index & 2 ? 255 : 0;
    }
  }
  CHECK(SimTownGroundArt_Init(rom, 0x100000));
  ground.enabled_town_mask = 1;
  ground.development_tier[0] = 1;
  memset(ground.terrain[0], 8, sizeof(ground.terrain[0]));
  int x = 8, y = 8;
  /* Find a horizontal-ground row for the fixture without assuming a cliff's
   * static location. The cliff exclusion is tested separately below. */
  for (; y < 24; y++) {
    bool flat = true;
    for (int dx = 0; dx < 5; dx++)
      if (SimTownTerrain_IsFaceCell(1, x + dx, y)) flat = false;
    if (flat) break;
  }
  CHECK(y < 24);
  ground.terrain[0][y * 32 + x + 1] = 1;
  ground.terrain[0][y * 32 + x + 2] = 0xE1;
  ground.terrain[0][y * 32 + x + 3] = 0x78;
  ground.terrain[0][y * 32 + x + 4] = 0xE3;
  ground.object_rows[0][y] = (1u << (x + 1)) | (1u << (x + 2));
  int ox, oy;
  CHECK(SimWorldMap_OriginForTown(1, &ox, &oy));
  const size_t at = (size_t)((oy + y) * 16 + 8) * pitch + (ox + x) * 16 + 8;
  CHECK(SimWorldNavigationArt_OverlayTownGround(out, pitch, &ground, true, false, 0));
  CHECK(out[at] == 0xFFFF0000u);
  CHECK(out[at + 16] == 0xFFFF0000u); /* forest footprint, not its angled art */
  CHECK(out[at + 32] == 0xFF0000FFu); /* original river under a real bridge */
  CHECK(out[at + 48] == baseline);   /* no flattened mountain silhouette */
  CHECK(out[at + 64] == baseline);   /* unresolved structure retains live art */
  CHECK(out[(size_t)(oy * 16) * pitch + ox * 16] == baseline);
  CHECK(out[0] == baseline);        /* outside town */
  for (int row = 0; row < kSimWorldNavigationArtPixels; row++)
    CHECK(out[(size_t)row * pitch + pitch - 1] == baseline);
  for (int cy = 4; cy < 28; cy++)
    for (int cx = 4; cx < 28; cx++)
      if (SimTownTerrain_IsFaceCell(1, cx, cy))
        CHECK(out[(size_t)((oy + cy) * 16 + 8) * pitch + (ox + cx) * 16 + 8] == baseline);
  CHECK(SimWorldNavigationArt_OverlayTownGround(out, pitch, &ground, true, true, 0));
  unsigned cliffs = 0;
  for (int cy = 4; cy < 28; cy++)
    for (int cx = 4; cx < 28; cx++)
      if (SimTownTerrain_IsFaceCell(1, cx, cy)) {
        CHECK(out[(size_t)((oy + cy) * 16 + 8) * pitch + (ox + cx) * 16 + 8] == 0xFFFF0000u);
        cliffs++;
      }
  CHECK(cliffs > 0);
  for (size_t i = 0; i < count; i++) out[i] = baseline;
  CHECK(SimWorldNavigationArt_OverlayTownGround(out, pitch, &ground, false, false, 0));
  CHECK(out[at] == 0xFFFF0000u);
  CHECK(out[at + 16] == baseline && out[at + 32] == baseline);
  ground.enabled_town_mask = 0;
  for (size_t i = 0; i < count; i++) out[i] = baseline;
  CHECK(SimWorldNavigationArt_OverlayTownGround(out, pitch, &ground, true, false, 0));
  CHECK(out[at] == baseline);
  SimTownGroundArt_Shutdown();
  CHECK(!SimWorldNavigationArt_OverlayTownGround(out, pitch, &ground, true, false, 0));
  CHECK(out[at] == baseline);
  CHECK(!SimWorldNavigationArt_OverlayTownGround(out, 1, &ground, true, false, 0));
  free(out);
  free(rom);
}

static void TestAnimatedGroundComposition(void) {
  const int pitch = kSimWorldNavigationArtPixels + 3;
  const size_t count = (size_t)pitch * kSimWorldNavigationArtPixels;
  uint32_t *first = malloc(count * sizeof(*first));
  uint32_t *second = malloc(count * sizeof(*second));
  uint8_t *rom = calloc(0x100000, 1);
  CHECK(first && second && rom);
  if (!first || !second || !rom) { free(first); free(second); free(rom); return; }
  rom[0x1098D] = 0x24;
  rom[0x1098E] = 8;
  for (int bank = 0; bank < 2; bank++) {
    const int palette = bank ? 0xE3D93 : 0xE3B93;
    rom[palette + 2] = 31;       /* ordinary red ground */
    rom[palette + 5] = 0x7C;     /* phase-zero blue water */
    rom[palette + 6] = 0xE0;     /* phase-one green water */
    rom[palette + 7] = 3;
    for (int y = 0; y < 8; y++) {
      const int chars = 0x60000 + bank * 0x4000;
      rom[chars + 32 * 32 + y * 2] = 255;
      rom[chars + y * 2 + 1] = 255;
      rom[chars + 0x100 + y * 2] = 255;
      rom[chars + 0x100 + y * 2 + 1] = 255;
    }
  }
  for (int tile = 0; tile < 256; tile++)
    for (int q = 0; q < 4; q++)
      rom[0xC881B + tile * 8 + q * 2] =
          tile == 0x25 || (tile == 0x2E && !(q & 1)) ? 0 : 32;
  CHECK(SimTownGroundArt_Init(rom, 0x100000));
  SimWorldNavigationTownGround ground = {.enabled_town_mask = 63};
  for (int town = 0; town < kSimTownCount; town++) {
    ground.development_tier[town] = town & 1 ? 2 : 1;
    memset(ground.terrain[town], 8, sizeof(ground.terrain[town]));
    ground.terrain[town][8 * 32 + 8] = 0x25;
    ground.terrain[town][8 * 32 + 9] = 0x2E;
  }
  for (size_t i = 0; i < count; i++) first[i] = second[i] = 0xFF102030u;
  CHECK(SimWorldNavigationArt_OverlayTownGround(first, pitch, &ground, true, true, 0));
  CHECK(SimWorldNavigationArt_OverlayTownGround(second, pitch, &ground, true, true, 1));
  size_t changed = 0;
  for (size_t i = 0; i < count; i++) {
    if (first[i] == second[i]) continue;
    CHECK(first[i] == 0xFF0000FFu && second[i] == 0xFF00FF00u);
    changed++;
  }
  CHECK(changed == kSimTownCount * (256 + 128));
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    CHECK(SimWorldMap_OriginForTown(town, &ox, &oy));
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 32; x++) {
        const size_t at = (size_t)((oy + 8) * 16 + y) * pitch + (ox + 8) * 16 + x;
        CHECK(second[at] == (x < 24 ? 0xFF00FF00u : 0xFFFF0000u));
      }
  }
  memcpy(first, second, count * sizeof(*first));
  CHECK(!SimWorldNavigationArt_OverlayTownGround(second, pitch, &ground, true, true, 4));
  CHECK(!memcmp(first, second, count * sizeof(*first)));
  /* Differential oracle: sparse phase updates must equal a fresh full bake
   * at every pixel, including feather strips, transparent phases, source
   * Scale2x neighbours, padded pitches, all town variants and rewinds. */
  const int source_pitch = kSimWorldMapPixels + 5;
  const size_t source_count = (size_t)source_pitch * kSimWorldMapPixels;
  uint32_t *developed = malloc(source_count * sizeof(*developed));
  uint32_t *baseline_pixels = malloc(source_count * sizeof(*baseline_pixels));
  CHECK(developed && baseline_pixels);
  if (developed && baseline_pixels) {
    for (size_t i = 0; i < source_count; i++) {
      developed[i] = i % 3 ? 0xFF102030u : 0xFF607080u;
      baseline_pixels[i] = i % 5 ? 0xFF182838u : 0xFF803010u;
    }
    for (int town = 0; town < kSimTownCount; town++) {
      for (int y = 0; y < 32; y++) {
        ground.terrain[town][y * 32] = 0x25;
        ground.terrain[town][y * 32 + 1] = 0x2E;
        ground.terrain[town][y * 32 + 31] = 0x25;
      }
      ground.terrain[town][9 * 32 + 8] = 0xE1;
      ground.object_rows[town][9] = (1u << 8);
      ground.terrain[town][10 * 32 + 8] = 0x25;
      ground.object_rows[town][10] = (1u << 8);
    }
    const uint8_t phases[] = {1, 2, 3, 0, 3, 1, 1, 0};
    for (int gates = 0; gates < 6; gates++) {
      const bool models = (gates & 1) != 0, cliffs = (gates & 2) != 0;
      const bool detailed = gates < 4;
      for (size_t i = 0; i < count; i++) first[i] = second[i] = 0xDEADBEEFu;
      CHECK(SimWorldNavigationArt_Build(first, pitch, developed, source_pitch,
          baseline_pixels, source_pitch));
      if (detailed) CHECK(SimWorldNavigationArt_OverlayTownGround(first, pitch, &ground, models, cliffs, 0));
      uint8_t previous = 0;
      for (size_t p = 0; p < sizeof(phases); p++) {
        SimWorldNavigationArtChanges changes;
        uint8_t world_cells[kSimWorldMapBytes] = {0};
        if (p % 2 == 0) {
          /* Change developed and baseline texels independently, including
           * exact cell boundaries whose Scale2x stencil crosses cells. */
          const int cx = p ? 48 : 0, cy = p ? 56 : 0;
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
              if (cx + dx >= 0 && cy + dy >= 0)
                world_cells[(cy + dy) * kSimWorldMapTiles + cx + dx] = 1;
          const size_t at = (size_t)cy * 8 * source_pitch + cx * 8;
          developed[at] ^= 0x00385868u;
          developed[at + 7] ^= 0x00603018u;
          baseline_pixels[at + source_pitch] ^= 0x00100808u;
        }
        if (p % 2) {
          SimWorldNavigationArtAnimation work;
          CHECK(SimWorldNavigationArt_PrepareAnimation(&work, first, pitch,
              developed, source_pitch, baseline_pixels, source_pitch,
              NULL, detailed ? &ground : NULL, models, cliffs, previous, phases[p]));
          /* Uneven, reordered ranges reproduce the full-bake oracle below.
           * Neighbouring source rows are immutable, not another job's output. */
          SimWorldNavigationArt_RenderAnimationRows(&work, 64, 128);
          SimWorldNavigationArt_RenderAnimationRows(&work, 1, 63);
          SimWorldNavigationArt_RenderAnimationRows(&work, 0, 1);
          SimWorldNavigationArt_RenderAnimationRows(&work, 63, 64);
          SimWorldNavigationArt_RenderAnimationRows(&work, 0, 0);
          SimWorldNavigationArt_RenderAnimationRows(&work, 0, 129);
          changes = work.changes;
        } else CHECK(SimWorldNavigationArt_UpdateAnimation(first, pitch,
            developed, source_pitch, baseline_pixels, source_pitch,
            world_cells, detailed ? &ground : NULL,
            models, cliffs, previous, phases[p], &changes));
        CHECK(SimWorldNavigationArt_Build(second, pitch, developed, source_pitch,
            baseline_pixels, source_pitch));
        if (detailed) CHECK(SimWorldNavigationArt_OverlayTownGround(second, pitch, &ground, models, cliffs, phases[p]));
        CHECK(!memcmp(first, second, count * sizeof(*first)));
        if (previous == phases[p] && p % 2) {
          const SimWorldNavigationArtChanges empty = {0};
          CHECK(!memcmp(&changes, &empty, sizeof(changes)));
        }
        previous = phases[p];
      }
    }
    SimWorldNavigationArtChanges unchanged, changes;
    memset(&unchanged, 0x5A, sizeof(unchanged));
    changes = unchanged;
    CHECK(!SimWorldNavigationArt_UpdateAnimation(first, pitch,
        developed, source_pitch, baseline_pixels, source_pitch,
        NULL, &ground, true, true, 0, 4, &changes));
    CHECK(!memcmp(&changes, &unchanged, sizeof(changes)));
    CHECK(!memcmp(first, second, count * sizeof(*first)));
    SimWorldNavigationArtAnimation work;
    CHECK(SimWorldNavigationArt_PrepareAnimation(&work, first, pitch,
        developed, source_pitch, baseline_pixels, source_pitch,
        NULL, &ground, true, true, 0, 1));
    CHECK(!SimWorldNavigationArt_PrepareAnimation(&work, first, pitch,
        developed, source_pitch, baseline_pixels, source_pitch,
        NULL, &ground, true, true, 0, 4));
    CHECK(!work.ready);
    SimWorldNavigationArt_RenderAnimationRows(&work, 0, kSimWorldMapTiles);
    CHECK(!memcmp(first, second, count * sizeof(*first)));
  }
  free(developed);
  free(baseline_pixels);
  ground.enabled_town_mask = 0;
  CHECK(SimWorldNavigationArt_OverlayTownGround(second, pitch, &ground, true, true, 2));
  CHECK(!memcmp(first, second, count * sizeof(*first)));
  SimTownGroundArt_Shutdown();
  free(rom);
  free(second);
  free(first);
}

static void TestScale2xNeighboursAndEdges(void) {
  const int width = kSimWorldMapPixels;
  const int pitch = kSimWorldNavigationArtPixels + 3;
  uint32_t *source = malloc((size_t)width * width * sizeof(*source));
  uint32_t *output = malloc((size_t)pitch * width * 2 * sizeof(*output));
  CHECK(source && output);
  if (!source || !output) { free(source); free(output); return; }
  const uint32_t colours[] = {0xff183cc0, 0xff607c20, 0xff587418};
  for (int y = 0; y < width; y++)
    for (int x = 0; x < width; x++)
      source[y * width + x] = colours[(x / 3 + y / 2) % 3];
  CHECK(SimWorldNavigationArt_Build(output, pitch, source, width, source, width));
  /* Compare every output texel with the previous scalar Scale2x rule,
   * including clamped world edges and a non-tight destination pitch. */
  for (int y = 0; y < width; y++) {
    for (int x = 0; x < width; x++) {
      const uint32_t b = ReferencePixel(source, x, y - 1);
      const uint32_t d = ReferencePixel(source, x - 1, y);
      const uint32_t e = ReferencePixel(source, x, y);
      const uint32_t f = ReferencePixel(source, x + 1, y);
      const uint32_t h = ReferencePixel(source, x, y + 1);
      const int at = y * 2 * pitch + x * 2;
      CHECK(output[at] == (d == b && d != h && b != f ? d : e));
      CHECK(output[at + 1] == (b == f && b != d && f != h ? f : e));
      CHECK(output[at + pitch] == (d == h && d != b && h != f ? d : e));
      CHECK(output[at + pitch + 1] == (h == f && d != h && b != f ? f : e));
    }
  }
  free(source);
  free(output);
}

static void TestOverviewAnimationRuns(void) {
  const int width = kSimWorldMapPixels, source_pitch = width + 3;
  const int pitch = kSimWorldNavigationArtPixels + 7;
  const size_t source_count = (size_t)source_pitch * width;
  const size_t count = (size_t)pitch * kSimWorldNavigationArtPixels;
  uint32_t *source = malloc(source_count * sizeof(*source));
  uint32_t *baseline = malloc(source_count * sizeof(*baseline));
  uint32_t *updated = malloc(count * sizeof(*updated));
  uint32_t *reference = malloc(count * sizeof(*reference));
  CHECK(source && baseline && updated && reference);
  if (!source || !baseline || !updated || !reference) goto done;
  for (size_t i = 0; i < source_count; i++) {
    source[i] = i % 3 ? 0xff607c20u : 0xff183cc0u;
    baseline[i] = i % 5 ? source[i] : 0xff587418u;
  }
  for (size_t i = 0; i < count; i++) updated[i] = reference[i] = 0xDEADBEEFu;
  CHECK(SimWorldNavigationArt_Build(updated, pitch, source, source_pitch, baseline, source_pitch));
  for (int pattern = 0; pattern < 4; pattern++) {
    uint8_t cells[kSimWorldMapBytes] = {0};
    for (int y = 0; y < kSimWorldMapTiles; y++)
      for (int x = 0; x < kSimWorldMapTiles; x++) {
        const bool dirty = pattern == 0 || (pattern == 1 && (x + y) % 2 == 0) ||
            (pattern == 2 && (y % 7 == 0 || x == 127)) ||
            (pattern == 3 && x == 127 && y == 127);
        if (!dirty) continue;
        cells[y * kSimWorldMapTiles + x] = 1;
        /* A changed interior texel has no dependency outside this cell.
         * Keep holes between runs genuinely clean, including row padding. */
        const size_t at = (size_t)(y * 8 + 3) * source_pitch + x * 8 + 4;
        source[at] ^= 0x00201008u;
        baseline[at + source_pitch] ^= 0x00081010u;
      }
    SimWorldNavigationArtChanges changes;
    CHECK(SimWorldNavigationArt_UpdateAnimation(updated, pitch, source, source_pitch,
        baseline, source_pitch, cells, NULL, false, false, 0, 0, &changes));
    CHECK(!memcmp(cells, changes.cells, sizeof(cells)));
    CHECK(SimWorldNavigationArt_Build(reference, pitch, source, source_pitch, baseline, source_pitch));
    CHECK(!memcmp(updated, reference, count * sizeof(*updated)));
    for (int y = 0; y < kSimWorldNavigationArtPixels; y++)
      for (int x = kSimWorldNavigationArtPixels; x < pitch; x++)
        CHECK(updated[(size_t)y * pitch + x] == 0xDEADBEEFu);
  }
done:
  free(source); free(baseline); free(updated); free(reference);
}

int main(void) {
  TestNativeGroundComposition();
  TestAnimatedGroundComposition();
  TestTownFeatherAndScale();
  TestScale2xNeighboursAndEdges();
  TestOverviewAnimationRuns();
  printf("sim world navigation art tests: %s\n",
         failures ? "FAIL" : "pass");
  return failures ? 1 : 0;
}
