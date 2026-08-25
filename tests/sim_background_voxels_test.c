#include "sim/sim_background_voxels.h"
#include "sim/sim_background_voxel_models.h"
#include "sim/sim_background_voxel_region.h"
#include "sim/sim_structure_visuals.h"
#include "sim/sim_town_terrain.h"
#include "fixtures/sim_aitos_house_build_gf14690.h"
#include "fixtures/sim_bridge_build_programs.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramBytes = 0x20000,
  kCellMaps = 0x12000,
  kCellMapBytes = 0x400,
  kRecords = 0x16BE7,
  kRecordsPerTown = 0x200,
  kStepSlots = 0x177E7,
  kStepSlotBytes = 8,
  kVramWords = 0x8000,
  kMarahnaTown = 5,
};

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static size_t CellIndex(int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kCellMaps + quadrant * 0x100 + (y & 15) * 16 + (x & 15);
}

static size_t TownCellIndex(int town, int x, int y) {
  return CellIndex(x, y) + (size_t)town * kCellMapBytes;
}

static size_t CanvasTileWord(int tile_x, int tile_y) {
  int quadrant = (tile_y >= 32 ? 2 : 0) + (tile_x >= 32 ? 1 : 0);
  return (size_t)quadrant * kSimTownQuadrantWords +
      (size_t)(tile_y & 31) * 32 + (tile_x & 31);
}

static void SetCanvasTile(uint8_t *wram, int tile_x, int tile_y,
                          uint16_t entry) {
  size_t at = kSimTownTilemapWram + CanvasTileWord(tile_x, tile_y) * 2;
  wram[at] = (uint8_t)entry;
  wram[at + 1] = (uint8_t)(entry >> 8);
}

static void SetSolidColourOneTile(uint16_t *vram, int tile) {
  for (int row = 0; row < 8; row++)
    vram[tile * 16 + row] = 0x00FF;
}

static void FillCell(uint32_t *pixels, int x, int y, uint32_t colour) {
  for (int row = 0; row < 16; row++)
    for (int column = 0; column < 16; column++)
      pixels[(size_t)(y * 16 + row) * kSimTownCanvasPixels + x * 16 + column] =
          colour;
}

/* Terrain metatile ids shared by all six towns: $0B is evergreen forest
 * interior, $0E the broad mangrove family, $22 the mostly-ground forest
 * fringe, $01 the single clearable bush, $09 the clearable palm and $3D
 * Bloodpool marsh. */
enum {
  kTileForest = 0x0B,
  kTileBroadleaf = 0x0E,
  kTileForestEdge = 0x22,
  kTileShrub = 0x01,
  kTileGrass = 0x08,
  kTileMarsh = 0x3D,
  kTilePalm = 0x09,
  kTileWater = 0x10,
  kTerrainDefinitions = 0x2100,
};

static void SetTerrainDefinition(uint8_t *wram, int metatile,
                                 uint16_t a, uint16_t b,
                                 uint16_t c, uint16_t d) {
  const uint16_t entries[4] = {a, b, c, d};
  size_t at = kTerrainDefinitions + (size_t)metatile * 8;
  for (int entry = 0; entry < 4; entry++) {
    wram[at + entry * 2] = (uint8_t)entries[entry];
    wram[at + entry * 2 + 1] = (uint8_t)(entries[entry] >> 8);
  }
}

/* The four live 8x8 entries a cell is currently displaying. */
static void SetCanvasCell(uint8_t *wram, int x, int y,
                          uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  SetCanvasTile(wram, x * 2, y * 2, a);
  SetCanvasTile(wram, x * 2 + 1, y * 2, b);
  SetCanvasTile(wram, x * 2, y * 2 + 1, c);
  SetCanvasTile(wram, x * 2 + 1, y * 2 + 1, d);
}

static const SimBackgroundVoxelObject *FindKind(
    const SimBackgroundVoxelScene *scene, SimBackgroundVoxelKind kind) {
  for (uint16_t i = 0; i < scene->object_count; i++)
    if (scene->objects[i].kind == kind) return &scene->objects[i];
  return NULL;
}

enum { kStructureDefinitions = 0x3100 };

static void SetStructureDefinition(uint8_t *wram, int metatile,
                                   uint16_t a, uint16_t b,
                                   uint16_t c, uint16_t d) {
  const uint16_t entries[4] = {a, b, c, d};
  size_t at = kStructureDefinitions + (size_t)metatile * 8;
  for (int entry = 0; entry < 4; entry++) {
    wram[at + entry * 2] = (uint8_t)entries[entry];
    wram[at + entry * 2 + 1] = (uint8_t)(entries[entry] >> 8);
  }
}

static int MaterialFaces(const SimBackgroundVoxelModel *model,
                         SimBackgroundVoxelMaterial material) {
  int count = 0;
  for (uint16_t face = 0; face < model->face_count; face++)
    if (model->faces[face].material == material) count++;
  return count;
}

static uint64_t ModelHash(const SimBackgroundVoxelModel *model) {
  const uint8_t *bytes = (const uint8_t *)model->faces;
  size_t byte_count = model->face_count * sizeof(model->faces[0]);
  uint64_t hash = 1469598103934665603ull;
  for (size_t at = 0; at < byte_count; at++) {
    hash ^= bytes[at];
    hash *= 1099511628211ull;
  }
  return hash ^ model->face_count;
}

/* Give every catalog frame a unique synthetic atlas definition while keeping
 * all live words below attribute bit 9, which the native copier strips. */
static bool CatalogFrameEntries(SimStructureVisualFamily target_family,
                                uint8_t target_metatile,
                                uint16_t entries[4]) {
  int ordinal = 0;
  for (int family = 0; family < kSimStructureVisualFamilyCount; family++) {
    size_t count = 0;
    const SimStructureVisualFrame *frames = SimStructureVisuals_Frames(
        (SimStructureVisualFamily)family, &count);
    for (size_t frame = 0; frame < count; frame++, ordinal++)
      if (family == target_family &&
          frames[frame].metatile == target_metatile) {
        for (int entry = 0; entry < 4; entry++)
          entries[entry] = (uint16_t)(0x0100 + ordinal * 4 + entry);
        return true;
      }
  }
  return false;
}

static void SeedStructureVisualCatalog(uint8_t *wram) {
  for (int family = 0; family < kSimStructureVisualFamilyCount; family++) {
    size_t count = 0;
    const SimStructureVisualFrame *frames = SimStructureVisuals_Frames(
        (SimStructureVisualFamily)family, &count);
    for (size_t frame = 0; frame < count; frame++) {
      uint16_t entries[4];
      CHECK(CatalogFrameEntries((SimStructureVisualFamily)family,
                                frames[frame].metatile, entries));
      SetStructureDefinition(wram, frames[frame].metatile,
                             entries[0], entries[1], entries[2], entries[3]);
    }
  }
}

static void SetCatalogFrame(uint8_t *wram, SimStructureVisualFamily family,
                            uint8_t metatile, int cell_x, int cell_y) {
  uint16_t entries[4] = {0};
  CHECK(CatalogFrameEntries(family, metatile, entries));
  SetCanvasCell(wram, cell_x, cell_y,
                entries[0], entries[1], entries[2], entries[3]);
}

/* A mountain hides only what is BEHIND it, and only within the reach of its
 * own shear. The camera looks from the south, so a mass north of a cell cannot
 * clip what stands there; a mass south of it can, but only for the few cells
 * its raised art is displaced across. An unbounded scan put nearly every
 * ground cell in Aitos behind a mountain, which is no split at all. */
static void CheckMountainOcclusionReach(void) {
  static uint8_t wram[kWramBytes];
  memset(wram, 0, sizeof(wram));
  /* One two-cell mass in column 10, occupying rows 20 and 21. */
  wram[CellIndex(10, 20)] = 0x78;
  wram[CellIndex(10, 21)] = 0x78;
  /* The queries read the PUBLISHED scene, so this has to go through Build
   * rather than Classify. */
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  static uint16_t vram[kVramWords];
  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1, 1, true);

  CHECK(SimBackgroundVoxels_CellIsMountain(10, 20));
  CHECK(SimBackgroundVoxels_CellIsMountain(10, 21));
  CHECK(!SimBackgroundVoxels_CellIsMountain(10, 19));
  /* Directly behind the mass: occluded. */
  CHECK(SimBackgroundVoxels_MountainInFrontOf(10, 19));
  CHECK(SimBackgroundVoxels_MountainInFrontOf(10, 17));
  /* Beyond the shear's reach: the mass is too far south to overlap. */
  CHECK(!SimBackgroundVoxels_MountainInFrontOf(10, 10));
  /* In front of the mass: never occluded, however tall the art reaches. */
  CHECK(!SimBackgroundVoxels_MountainInFrontOf(10, 22));
  CHECK(!SimBackgroundVoxels_MountainInFrontOf(10, 31));
  /* A different column is unaffected. */
  CHECK(!SimBackgroundVoxels_MountainInFrontOf(11, 19));
}

/* SimTownCanvas owns detection of Marahna's earthquake redraw. Once that
 * serial changes, the enhanced ground copy must publish the revealed pixels
 * too; otherwise the ordinary town canvas would show land while object
 * cutouts and replacements kept sampling the old water image. */
static void CheckMarahnaEarthquakeCanvasRebuild(void) {
  static uint8_t wram[kWramBytes];
  static uint16_t vram[kVramWords];
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  const int revealed_x = 15, revealed_y = 17;
  const uint32_t water = 0xFF204878;
  const uint32_t land = 0xFF647814;
  const uint32_t water_canvas_serial = 1;
  const uint32_t land_canvas_serial = water_canvas_serial + 1;
  memset(wram, 0, sizeof(wram));
  memset(vram, 0, sizeof(vram));
  for (size_t at = 0;
       at < (size_t)kSimTownCanvasPixels * kSimTownCanvasPixels; at++)
    pixels[at] = water;

  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(
      kMarahnaTown, wram, pixels, vram,
      water_canvas_serial, water_canvas_serial, true);
  const size_t centre =
      (size_t)(revealed_y * kSimBackgroundCellPixels +
               kSimBackgroundCellPixels / 2) * kSimTownCanvasPixels +
      revealed_x * kSimBackgroundCellPixels +
      kSimBackgroundCellPixels / 2;
  const uint32_t water_serial = SimBackgroundVoxels_Serial();
  CHECK(water_serial != 0);
  CHECK(SimBackgroundVoxels_GroundPixels()[centre] == water);

  FillCell(pixels, revealed_x, revealed_y, land);
  SimBackgroundVoxels_Build(
      kMarahnaTown, wram, pixels, vram,
      water_canvas_serial, water_canvas_serial, true);
  CHECK(SimBackgroundVoxels_Serial() == water_serial);
  CHECK(SimBackgroundVoxels_GroundPixels()[centre] == water);

  SimBackgroundVoxels_Build(
      kMarahnaTown, wram, pixels, vram,
      land_canvas_serial, land_canvas_serial, true);
  CHECK(SimBackgroundVoxels_Serial() != water_serial);
  CHECK(SimBackgroundVoxels_GroundPixels()[centre] == land);
}

typedef struct GroundSourceFadeFixture {
  uint8_t wram[kWramBytes];
  uint16_t vram[kVramWords];
  uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
} GroundSourceFadeFixture;

static GroundSourceFadeFixture s_ground_source_fade;
static const uint32_t kGroundSourceBlack = 0xFF000000;
static const uint32_t kGroundSourceWater = 0xFF204878;
static const uint32_t kGroundSourceLand = 0xFF647814;
static const uint32_t kGroundSourceObject = 0xFFC06020;
static const uint32_t kGroundSourceCliff = 0xFF809098;

static void BeginGroundSourceFade(uint8_t town) {
  memset(&s_ground_source_fade, 0, sizeof(s_ground_source_fade));
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++)
      s_ground_source_fade.wram[
          TownCellIndex(town - 1, cell_x, cell_y)] = kTileWater;
  for (size_t at = 0;
       at < (size_t)kSimTownCanvasPixels * kSimTownCanvasPixels; at++)
    s_ground_source_fade.pixels[at] = kGroundSourceBlack;
}

static void RevealGroundSourcePixels(void) {
  for (size_t at = 0;
       at < (size_t)kSimTownCanvasPixels * kSimTownCanvasPixels; at++)
    s_ground_source_fade.pixels[at] = kGroundSourceWater;
}

static size_t CellCentre(int cell_x, int cell_y) {
  return (size_t)(cell_y * kSimBackgroundCellPixels +
                  kSimBackgroundCellPixels / 2) * kSimTownCanvasPixels +
      cell_x * kSimBackgroundCellPixels + kSimBackgroundCellPixels / 2;
}

/* Map entry can publish the scene during a black fade, when an RGB-only source
 * search cannot distinguish land from Marahna's water. The selected source is
 * retained across the later pixel-only fade frames, so its terrain identity has
 * to be valid before colours become visible. */
static void CheckMarahnaGroundSourceRejectsWaterDuringFade(void) {
  const int shrub_x = 4, shrub_y = 4;
  const int land_x = 20, land_y = 20;
  BeginGroundSourceFade(kMarahnaTown);
  s_ground_source_fade.wram[
      TownCellIndex(kMarahnaTown - 1, shrub_x, shrub_y)] = kTileShrub;
  s_ground_source_fade.wram[
      TownCellIndex(kMarahnaTown - 1, land_x, land_y)] = kTileGrass;

  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(
      kMarahnaTown, s_ground_source_fade.wram,
      s_ground_source_fade.pixels, s_ground_source_fade.vram, 1, 1, true);
  const uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  CHECK(FindKind(SimBackgroundVoxels_Scene(),
                 kSimBackgroundVoxel_Shrub) != NULL);

  RevealGroundSourcePixels();
  FillCell(s_ground_source_fade.pixels,
           land_x, land_y, kGroundSourceLand);
  FillCell(s_ground_source_fade.pixels,
           shrub_x, shrub_y, kGroundSourceObject);
  SimBackgroundVoxels_Build(
      kMarahnaTown, s_ground_source_fade.wram,
      s_ground_source_fade.pixels, s_ground_source_fade.vram, 2, 1, true);
  CHECK(SimBackgroundVoxels_SceneSerial() == scene_serial);
  CHECK(SimBackgroundVoxels_GroundPixels()[CellCentre(shrub_x, shrub_y)] ==
        kGroundSourceLand);
}

/* Marahna's plateau walls are a separate authored topology, not part of the
 * common mountain tile range. On a black first frame the wall and the later
 * horizontal candidate tie by colour, so scan order makes this fail if the
 * face classifier is ever removed from the source policy. */
static void CheckMarahnaGroundSourceRejectsCliffDuringFade(void) {
  const int shrub_x = 4, shrub_y = 4;
  const int cliff_x = 8, cliff_y = 21;
  const int land_x = 14, land_y = 21;
  CHECK(SimTownTerrain_FaceKind(kMarahnaTown, cliff_x, cliff_y) ==
        kSimTownTerrainFace_Cliff);
  CHECK(!SimTownTerrain_IsFaceCell(kMarahnaTown, land_x, land_y));

  BeginGroundSourceFade(kMarahnaTown);
  s_ground_source_fade.wram[
      TownCellIndex(kMarahnaTown - 1, shrub_x, shrub_y)] = kTileShrub;
  s_ground_source_fade.wram[
      TownCellIndex(kMarahnaTown - 1, cliff_x, cliff_y)] = kTileGrass;
  s_ground_source_fade.wram[
      TownCellIndex(kMarahnaTown - 1, land_x, land_y)] = kTileGrass;

  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(
      kMarahnaTown, s_ground_source_fade.wram,
      s_ground_source_fade.pixels, s_ground_source_fade.vram, 1, 1, true);
  const uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  CHECK(FindKind(SimBackgroundVoxels_Scene(),
                 kSimBackgroundVoxel_Shrub) != NULL);

  RevealGroundSourcePixels();
  FillCell(s_ground_source_fade.pixels,
           cliff_x, cliff_y, kGroundSourceCliff);
  FillCell(s_ground_source_fade.pixels,
           land_x, land_y, kGroundSourceLand);
  FillCell(s_ground_source_fade.pixels,
           shrub_x, shrub_y, kGroundSourceObject);
  SimBackgroundVoxels_Build(
      kMarahnaTown, s_ground_source_fade.wram,
      s_ground_source_fade.pixels, s_ground_source_fade.vram, 2, 1, true);
  CHECK(SimBackgroundVoxels_SceneSerial() == scene_serial);
  CHECK(SimBackgroundVoxels_GroundPixels()[CellCentre(shrub_x, shrub_y)] ==
        kGroundSourceLand);
}

static void CheckIndependentSceneAndPixelPublications(void) {
  static uint8_t wram[kWramBytes];
  static uint16_t vram[kVramWords];
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  memset(wram, 0, sizeof(wram));
  memset(vram, 0, sizeof(vram));
  for (size_t at = 0;
       at < (size_t)kSimTownCanvasPixels * kSimTownCanvasPixels; at++)
    pixels[at] = 0xFF647814;

  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_ResetBuildStats();
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1, 1, true);
  SimBackgroundVoxelBuildStats stats = SimBackgroundVoxels_BuildStats();
  CHECK(stats.build_calls == 1);
  CHECK(stats.scene_rebuilds == 1);
  CHECK(stats.pixel_refreshes == 1);
  uint32_t scene_serial = SimBackgroundVoxels_SceneSerial();
  uint32_t ground_serial = SimBackgroundVoxels_GroundSerial();
  uint32_t atlas_serial = SimBackgroundVoxels_AtlasSerial();
  int x, y, width, height;
  CHECK(SimBackgroundVoxels_TakeGroundDirtyRect(
      &x, &y, &width, &height));
  CHECK(x == 0 && y == 0 && width == kSimTownCanvasPixels &&
        height == kSimTownCanvasPixels);
  CHECK(!SimBackgroundVoxels_TakeGroundDirtyRect(
      &x, &y, &width, &height));

  /* A live pixel update keeps the classified scene and mountain atlas, and
   * publishes only the enhanced-ground pixel that actually changed. */
  pixels[0] = 0xFF102030;
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 2, 1, true);
  stats = SimBackgroundVoxels_BuildStats();
  CHECK(stats.build_calls == 2);
  CHECK(stats.scene_rebuilds == 1);
  CHECK(stats.pixel_refreshes == 2);
  CHECK(SimBackgroundVoxels_SceneSerial() == scene_serial);
  CHECK(SimBackgroundVoxels_GroundSerial() != ground_serial);
  CHECK(SimBackgroundVoxels_AtlasSerial() == atlas_serial);
  CHECK(SimBackgroundVoxels_TakeGroundDirtyRect(
      &x, &y, &width, &height));
  CHECK(x == 0 && y == 0 && width == 1 && height == 1);
  CHECK(!SimBackgroundVoxels_TakeGroundDirtyRect(
      &x, &y, &width, &height));

  /* A quiet call does no publication work. */
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 2, 1, true);
  stats = SimBackgroundVoxels_BuildStats();
  CHECK(stats.build_calls == 3);
  CHECK(stats.scene_rebuilds == 1);
  CHECK(stats.pixel_refreshes == 2);

  /* Record +3 is a live action byte, not a classification input. */
  wram[kRecords + 3] = 1;
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 2, 1, true);
  stats = SimBackgroundVoxels_BuildStats();
  CHECK(stats.build_calls == 4);
  CHECK(stats.scene_rebuilds == 1);
  CHECK(stats.pixel_refreshes == 2);

  /* Structure records are an independent topology source. Detect them even
   * before the live tilemap/image serial advances. */
  wram[kRecords + 2] = 0x80;
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 2, 1, true);
  stats = SimBackgroundVoxels_BuildStats();
  CHECK(stats.scene_rebuilds == 2);
  CHECK(stats.pixel_refreshes == 3);
  CHECK(SimBackgroundVoxels_SceneSerial() != scene_serial);
}

static void CheckStructureVisualCatalog(void) {
  static const uint8_t expected_metatiles[kSimStructureVisualFamilyCount][32] = {
    {
      0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B,
      0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B,
      0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2A, 0x2B,
      0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x3A, 0x3B,
    },
    {0x4C, 0x44, 0x4D, 0x45, 0xEA, 0xE2, 0xEB, 0xE3},
    {0x04, 0x06, 0x14, 0x24, 0x26, 0x16},
    {0x34, 0x36},
  };
  static const size_t expected_counts[kSimStructureVisualFamilyCount] = {
    32, 8, 6, 2,
  };
  bool seen[256] = {false};
  for (int family = 0; family < kSimStructureVisualFamilyCount; family++) {
    size_t count = 0;
    const SimStructureVisualFrame *frames = SimStructureVisuals_Frames(
        (SimStructureVisualFamily)family, &count);
    CHECK(frames != NULL);
    CHECK(count == expected_counts[family]);
    for (size_t frame = 0; frame < count; frame++) {
      CHECK(frames[frame].metatile == expected_metatiles[family][frame]);
      CHECK(!seen[frames[frame].metatile]);
      seen[frames[frame].metatile] = true;
      CHECK(frames[frame].state > kSimStructureVisualState_Unknown);
      CHECK(frames[frame].state < kSimStructureVisualStateCount);
      if (family == kSimStructureVisual_House) {
        uint8_t position = (uint8_t)(frame % 4);
        uint8_t expected_state = position == 0
            ? kSimStructureVisualState_Construction0
            : position == 1
                ? kSimStructureVisualState_Construction1
                : kSimStructureVisualState_Finished;
        CHECK(frames[frame].state == expected_state);
      } else if (family == kSimStructureVisual_Bridge) {
        CHECK(frames[frame].state == ((frame & 1)
            ? kSimStructureVisualState_Finished
            : kSimStructureVisualState_Construction0));
      } else if (family == kSimStructureVisual_Windmill) {
        CHECK(frames[frame].animation_phase == (uint8_t)(frame % 3));
        CHECK(frames[frame].state == (frame < 3
            ? kSimStructureVisualState_Construction0 + frame
            : kSimStructureVisualState_Finished));
      } else {
        CHECK(frames[frame].state == (frame == 0
            ? kSimStructureVisualState_Construction0
            : kSimStructureVisualState_Finished));
      }
    }
  }
  size_t invalid_count = 99;
  CHECK(SimStructureVisuals_Frames(
      kSimStructureVisualFamilyCount, &invalid_count) == NULL);
  CHECK(invalid_count == 0);
}

/* Full ownership matrix: every native house frame must survive every regional
 * model identity, development level and facing through classification and
 * geometry compilation. This is the seam the former isolated tests missed. */
static void CheckHouseFramesEndToEnd(void) {
  static uint8_t wram[kWramBytes];
  memset(wram, 0, sizeof(wram));
  SeedStructureVisualCatalog(wram);
  size_t frame_count = 0;
  const SimStructureVisualFrame *frames = SimStructureVisuals_Frames(
      kSimStructureVisual_House, &frame_count);
  uint64_t construction_hash[2] = {0, 0};
  const int cell_x = 6, cell_y = 6;
  for (int town = 1; town <= kSimBackgroundTownCount; town++)
    for (int level = 0;
         level < kSimBackgroundDevelopmentLevelCount; level++)
      for (int facing = 0; facing < 2; facing++)
        for (size_t frame = 0; frame < frame_count; frame++) {
          uint8_t *record = wram + kRecords +
              (size_t)(town - 1) * kRecordsPerTown;
          record[0] = cell_x;
          record[1] = cell_y;
          record[2] = (uint8_t)(0x80 | level << 4 |
              (facing ? 0x40 : 0));
          SetCatalogFrame(wram, kSimStructureVisual_House,
                          frames[frame].metatile, cell_x, cell_y);
          SimBackgroundVoxelScene scene;
          SimBackgroundVoxels_Classify((uint8_t)town, wram, true, &scene);
          const SimBackgroundVoxelObject *house =
              FindKind(&scene, kSimBackgroundVoxel_House);
          CHECK(scene.unmatched_visual_count == 0);
          CHECK(house != NULL);
          if (!house) continue;
          CHECK(house->visual_metatile == frames[frame].metatile);
          CHECK(house->visual_state == frames[frame].state);
          CHECK(house->animation_phase == frames[frame].animation_phase);
          CHECK(((house->flags & kSimBackgroundVoxel_AlternateFacing) != 0) ==
                (facing != 0));
          bool under_construction =
              SimStructureVisuals_IsConstruction(frames[frame].state);
          CHECK(((house->flags & kSimBackgroundVoxel_UnderConstruction) != 0) ==
                under_construction);

          SimBackgroundVoxelModel model;
          SimBackgroundVoxelModel_BuildStyled(
              house, kSimBackgroundVoxelDetail_Balanced,
              kSimBackgroundVoxelStyle_Basic, &model);
          CHECK(!model.overflow && model.face_count > 0);
          if (under_construction) {
            CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) > 0);
            CHECK(MaterialFaces(&model, kSimVoxelMaterial_Roof) == 0);
            uint8_t phase = frames[frame].animation_phase ? 1 : 0;
            uint64_t hash = ModelHash(&model);
            if (!construction_hash[phase]) construction_hash[phase] = hash;
            CHECK(construction_hash[phase] == hash);
          }
        }
  CHECK(construction_hash[0] != 0 && construction_hash[1] != 0);
  CHECK(construction_hash[0] != construction_hash[1]);
}

static void CheckAitosSnapshotHouseFrame(void) {
  static uint8_t wram[kWramBytes];
  memset(wram, 0, sizeof(wram));
  uint8_t *record = wram + kRecords +
      (size_t)(kAitosBuildTown - 1) * kRecordsPerTown +
      kAitosBuildRecordSlot * 4;
  memcpy(record, kAitosBuildRecord, sizeof(kAitosBuildRecord));
  memcpy(wram + kStepSlots + kAitosBuildRecordSlot * kStepSlotBytes,
         kAitosBuildStepSlot, sizeof(kAitosBuildStepSlot));
  SetStructureDefinition(wram, kAitosBuildMetatile,
                         kAitosBuildDefinition[0], kAitosBuildDefinition[1],
                         kAitosBuildDefinition[2], kAitosBuildDefinition[3]);
  SetCanvasCell(wram, kAitosBuildCellX, kAitosBuildCellY,
                kAitosBuildLiveCell[0], kAitosBuildLiveCell[1],
                kAitosBuildLiveCell[2], kAitosBuildLiveCell[3]);
  wram[TownCellIndex(kAitosBuildTown - 1,
                     kAitosBuildCellX, kAitosBuildCellY)] =
      kAitosBuildCellMarker;

  SimBackgroundVoxelScene scene;
  SimBackgroundVoxels_Classify(kAitosBuildTown, wram, true, &scene);
  const SimBackgroundVoxelObject *house =
      FindKind(&scene, kSimBackgroundVoxel_House);
  CHECK(scene.unmatched_visual_count == 0);
  CHECK(house != NULL);
  if (!house) return;
  CHECK(house->record_slot == kAitosBuildRecordSlot);
  CHECK(house->town == kAitosBuildTown);
  CHECK(house->development_level == 2);
  CHECK(house->visual_metatile == kAitosBuildMetatile);
  CHECK(house->visual_state == kSimStructureVisualState_Construction0);
  CHECK(house->flags & kSimBackgroundVoxel_UnderConstruction);

  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_Build(
      house, kSimBackgroundVoxelDetail_Balanced, &model);
  CHECK(!model.overflow && model.face_count > 0);
  CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) > 0);
  CHECK(MaterialFaces(&model, kSimVoxelMaterial_Roof) == 0);
  CHECK(model.max_z == 8.0f);

  /* Replay the ROM's exact $30 -> $31 -> $32 sequence from the resident Aitos
   * atlas. The enhanced models must progress scaffold -> taller scaffold ->
   * finished rather than collapsing the first publication into the last. */
  static const uint8_t expected_state[3] = {
    kSimStructureVisualState_Construction0,
    kSimStructureVisualState_Construction1,
    kSimStructureVisualState_Finished,
  };
  uint64_t sequence_hash[3] = {0};
  for (int step = 0; step < 3; step++) {
    SetStructureDefinition(
        wram, kAitosBuildSequenceMetatiles[step],
        kAitosBuildSequenceDefinitions[step][0],
        kAitosBuildSequenceDefinitions[step][1],
        kAitosBuildSequenceDefinitions[step][2],
        kAitosBuildSequenceDefinitions[step][3]);
    SetCanvasCell(
        wram, kAitosBuildCellX, kAitosBuildCellY,
        kAitosBuildSequenceDefinitions[step][0],
        kAitosBuildSequenceDefinitions[step][1],
        kAitosBuildSequenceDefinitions[step][2],
        kAitosBuildSequenceDefinitions[step][3]);
    SimBackgroundVoxels_Classify(kAitosBuildTown, wram, true, &scene);
    house = FindKind(&scene, kSimBackgroundVoxel_House);
    CHECK(house != NULL && scene.unmatched_visual_count == 0);
    if (!house) continue;
    CHECK(house->visual_state == expected_state[step]);
    SimBackgroundVoxelModel_Build(
        house, kSimBackgroundVoxelDetail_Balanced, &model);
    sequence_hash[step] = ModelHash(&model);
  }
  CHECK(sequence_hash[0] != sequence_hash[1]);
  CHECK(sequence_hash[1] != sequence_hash[2]);
  CHECK(sequence_hash[0] != sequence_hash[2]);
}

static void CheckUnknownFramesFailClosed(void) {
  static uint8_t wram[kWramBytes];
  static uint16_t vram[kVramWords];
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  static const struct {
    uint8_t structure_class;
    uint8_t kind;
    uint8_t family;
  } record_cases[] = {
    {0, kSimBackgroundVoxel_House, kSimStructureVisual_House},
    {3, kSimBackgroundVoxel_Windmill, kSimStructureVisual_Windmill},
    {4, kSimBackgroundVoxel_Factory, kSimStructureVisual_Factory},
  };
  for (size_t at = 0; at < sizeof(record_cases) / sizeof(record_cases[0]); at++) {
    memset(wram, 0, sizeof(wram));
    uint8_t *record = wram + kRecords;
    record[0] = 4;
    record[1] = 5;
    record[2] = (uint8_t)(0x80 | record_cases[at].structure_class);
    SimBackgroundVoxelScene scene;
    SimBackgroundVoxels_Classify(1, wram, true, &scene);
    CHECK(FindKind(&scene, (SimBackgroundVoxelKind)record_cases[at].kind) ==
          NULL);
    CHECK(scene.unmatched_visual_count == 1);
    CHECK(scene.unmatched_visuals[0].family == record_cases[at].family);
    CHECK(scene.unmatched_visuals[0].record_slot == 0);
  }

  memset(wram, 0, sizeof(wram));
  wram[CellIndex(10, 10)] = 0xE2;
  SimBackgroundVoxelScene bridge_scene;
  SimBackgroundVoxels_Classify(1, wram, true, &bridge_scene);
  CHECK(FindKind(&bridge_scene, kSimBackgroundVoxel_Bridge) == NULL);
  CHECK(bridge_scene.unmatched_visual_count == 1);
  CHECK(bridge_scene.unmatched_visuals[0].family ==
        kSimStructureVisual_Bridge);

  /* The fail-safe is visible behavior, not just a diagnostic counter: an
   * unknown house source cell remains byte-for-byte authentic in the enhanced
   * ground and is absent from the replacement atlas. */
  memset(wram, 0, sizeof(wram));
  memset(vram, 0, sizeof(vram));
  for (size_t at = 0;
       at < (size_t)kSimTownCanvasPixels * kSimTownCanvasPixels; at++)
    pixels[at] = 0xFF2468AC;
  uint8_t *record = wram + kRecords;
  record[0] = 4;
  record[1] = 5;
  record[2] = 0x80;
  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1, 1, true);
  size_t centre = (size_t)(5 * 16 + 8) * kSimTownCanvasPixels + 4 * 16 + 8;
  CHECK(SimBackgroundVoxels_GroundPixels()[centre] == 0xFF2468AC);
  CHECK((SimBackgroundVoxels_AtlasPixels()[centre] >> 24) == 0);
}

static void CheckBridgeFramesEndToEnd(void) {
  static uint8_t wram[kWramBytes];
  const int cell_x = 10, cell_y = 10, record_slot = 7;
  for (size_t at = 0;
       at < sizeof(kBridgeBuildProgramFrames) /
                sizeof(kBridgeBuildProgramFrames[0]); at++) {
    const SimBridgeBuildProgramFixture *frame =
        &kBridgeBuildProgramFrames[at];
    memset(wram, 0, sizeof(wram));
    uint16_t entries[4];
    CHECK(CatalogFrameEntries(
        kSimStructureVisual_Bridge, frame->metatile, entries));
    SetStructureDefinition(wram, frame->metatile,
                           entries[0], entries[1], entries[2], entries[3]);
    SetCanvasCell(wram, cell_x, cell_y,
                  entries[0], entries[1], entries[2], entries[3]);
    wram[TownCellIndex(frame->town - 1, cell_x, cell_y)] = frame->marker;
    uint8_t *record = wram + kRecords +
        (size_t)(frame->town - 1) * kRecordsPerTown + record_slot * 4;
    record[0] = cell_x;
    record[1] = cell_y;
    record[2] = (uint8_t)(0x81 | (frame->marker == 0xE1 ? 0x10 : 0));

    SimBackgroundVoxelScene scene;
    SimBackgroundVoxels_Classify(frame->town, wram, true, &scene);
    const SimBackgroundVoxelObject *bridge =
        FindKind(&scene, kSimBackgroundVoxel_Bridge);
    CHECK(scene.unmatched_visual_count == 0);
    CHECK(bridge != NULL);
    if (!bridge) continue;
    CHECK(bridge->record_slot == record_slot);
    CHECK(bridge->visual_metatile == frame->metatile);
    CHECK(bridge->visual_state == frame->state);
    CHECK(frame->program == (uint16_t)(0xD754 + at * 6));
    CHECK(frame->draw_list == (uint16_t)(0xDC18 + at * 4));
    bool construction = SimStructureVisuals_IsConstruction(frame->state);
    CHECK(((bridge->flags & kSimBackgroundVoxel_UnderConstruction) != 0) ==
          construction);

    SimBackgroundVoxelModel model;
    SimBackgroundVoxelModel_Build(
        bridge, kSimBackgroundVoxelDetail_Balanced, &model);
    CHECK(!model.overflow && model.face_count > 0);
    if (construction) {
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Wood) > 0);
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Paving) == 0);
    } else {
      CHECK(MaterialFaces(&model, kSimVoxelMaterial_Paving) > 0);
    }
  }
}

/* A windmill's state comes from the frame its plot is drawing, not from the
 * record's `$40` flag - that bit is the "no wind" story state, and reading it
 * as construction put a scaffold over a standing mill for the whole event. */
static void CheckWindmillFrames(void) {
  static uint8_t wram[kWramBytes];
  /* The six class-6 top-left frames: three scaffold steps, then the three
   * blade positions of the finished mill. */
  static const uint8_t kFrames[6] = {0x04, 0x06, 0x14, 0x24, 0x26, 0x16};
  static const int kConstruction[6] = {1, 1, 1, 0, 0, 0};
  static const uint8_t kPhase[6] = {0, 1, 2, 0, 1, 2};

  for (int stopped = 0; stopped < 2; stopped++)
    for (int frame = 0; frame < 6; frame++) {
      memset(wram, 0, sizeof(wram));
      for (int at = 0; at < 6; at++)
        SetStructureDefinition(wram, kFrames[at],
                               (uint16_t)(0x0100 + at * 4),
                               (uint16_t)(0x0101 + at * 4),
                               (uint16_t)(0x0102 + at * 4),
                               (uint16_t)(0x0103 + at * 4));
      uint8_t *record = wram + kRecords;
      record[0] = 10;
      record[1] = 11;
      record[2] = (uint8_t)(stopped ? 0xC3 : 0x83);
      SetCanvasCell(wram, 10, 11,
                    (uint16_t)(0x0100 + frame * 4),
                    (uint16_t)(0x0101 + frame * 4),
                    (uint16_t)(0x0102 + frame * 4),
                    (uint16_t)(0x0103 + frame * 4));

      SimBackgroundVoxelScene scene;
      SimBackgroundVoxels_Classify(1, wram, true, &scene);
      const SimBackgroundVoxelObject *mill =
          FindKind(&scene, kSimBackgroundVoxel_Windmill);
      CHECK(mill != NULL);
      if (!mill) continue;
      CHECK(((mill->flags & kSimBackgroundVoxel_UnderConstruction) != 0) ==
            (kConstruction[frame] != 0));
      /* `$C3` is the parked "no wind" record, so a built mill holds phase 0
       * however far round its plot happens to be drawn. A scaffold keeps its
       * own build step - construction is not wind-driven. */
      uint8_t expected = (stopped && !kConstruction[frame])
          ? 0 : kPhase[frame];
      CHECK(mill->animation_phase == expected);
    }

  /* One stamped record parks every mill in the town, including one the event
   * never reached because it was built afterwards. The ROM leaves that mill
   * spinning; the enhanced view holds it with the rest. */
  memset(wram, 0, sizeof(wram));
  SetStructureDefinition(wram, 0x24, 0x0100, 0x0101, 0x0102, 0x0103);
  SetStructureDefinition(wram, 0x26, 0x0104, 0x0105, 0x0106, 0x0107);
  uint8_t *stamped = wram + kRecords;
  stamped[0] = 10; stamped[1] = 11; stamped[2] = 0xC3;
  uint8_t *unstamped = stamped + 4;
  unstamped[0] = 4; unstamped[1] = 5; unstamped[2] = 0x83;
  SetCanvasCell(wram, 10, 11, 0x0100, 0x0101, 0x0102, 0x0103);
  SetCanvasCell(wram, 4, 5, 0x0104, 0x0105, 0x0106, 0x0107);
  SimBackgroundVoxelScene stopped_scene;
  SimBackgroundVoxels_Classify(1, wram, true, &stopped_scene);
  int mills = 0;
  for (uint16_t i = 0; i < stopped_scene.object_count; i++) {
    const SimBackgroundVoxelObject *o = &stopped_scene.objects[i];
    if (o->kind != kSimBackgroundVoxel_Windmill) continue;
    mills++;
    CHECK(!(o->flags & kSimBackgroundVoxel_UnderConstruction));
    CHECK(o->animation_phase == 0);
  }
  CHECK(mills == 2);
  /* With the enhancement off, each mill just follows its own plot: the ROM
   * parked the stamped one by drawing a static frame, and left the other
   * turning. That is the authentic behaviour the setting restores. */
  SimBackgroundVoxels_Classify(1, wram, false, &stopped_scene);
  int authentic_parked = 0, authentic_turning = 0;
  for (uint16_t i = 0; i < stopped_scene.object_count; i++) {
    const SimBackgroundVoxelObject *o = &stopped_scene.objects[i];
    if (o->kind != kSimBackgroundVoxel_Windmill) continue;
    CHECK(!(o->flags & kSimBackgroundVoxel_UnderConstruction));
    if (o->animation_phase == 0) authentic_parked++;
    if (o->animation_phase == 1) authentic_turning++;
  }
  CHECK(authentic_parked == 1);
  CHECK(authentic_turning == 1);

  /* Clearing the event releases both: the unstamped mill goes back to the
   * blade position its plot is drawing. */
  stamped[2] = 0x83;
  SimBackgroundVoxels_Classify(1, wram, true, &stopped_scene);
  int released = 0;
  for (uint16_t i = 0; i < stopped_scene.object_count; i++) {
    const SimBackgroundVoxelObject *o = &stopped_scene.objects[i];
    if (o->kind == kSimBackgroundVoxel_Windmill && o->animation_phase == 1)
      released++;
  }
  CHECK(released == 1);

  /* No recognisable frame on the plot fails closed: preserve authentic art and
   * expose the missing ownership entry instead of guessing "finished". */
  memset(wram, 0, sizeof(wram));
  uint8_t *record = wram + kRecords;
  record[0] = 10;
  record[1] = 11;
  record[2] = 0xC3;
  SimBackgroundVoxelScene scene;
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  const SimBackgroundVoxelObject *mill =
      FindKind(&scene, kSimBackgroundVoxel_Windmill);
  CHECK(mill == NULL);
  CHECK(scene.unmatched_visual_count == 1);
  CHECK(scene.unmatched_visuals[0].family == kSimStructureVisual_Windmill);
  CHECK(scene.unmatched_visuals[0].record_slot == 0);

  /* Same rule for the factory tier: $34 is its scaffold and $36 the finished
   * building. Its record never carries `$40` on any ROM path at all. */
  memset(wram, 0, sizeof(wram));
  SetStructureDefinition(wram, 0x34, 0x0200, 0x0201, 0x0202, 0x0203);
  SetStructureDefinition(wram, 0x36, 0x0204, 0x0205, 0x0206, 0x0207);
  record = wram + kRecords;
  record[0] = 20;
  record[1] = 20;
  record[2] = 0x84;
  SetCanvasCell(wram, 20, 20, 0x0200, 0x0201, 0x0202, 0x0203);
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  const SimBackgroundVoxelObject *factory =
      FindKind(&scene, kSimBackgroundVoxel_Factory);
  CHECK(factory && (factory->flags & kSimBackgroundVoxel_UnderConstruction));
  SetCanvasCell(wram, 20, 20, 0x0204, 0x0205, 0x0206, 0x0207);
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  factory = FindKind(&scene, kSimBackgroundVoxel_Factory);
  CHECK(factory && !(factory->flags & kSimBackgroundVoxel_UnderConstruction));
}

static void CheckStoneBridgeClassificationAndInpaint(void) {
  static uint8_t wram[kWramBytes];
  static uint16_t vram[kVramWords];
  static uint16_t cgram[256];
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  memset(wram, 0, sizeof(wram));
  memset(vram, 0, sizeof(vram));
  memset(cgram, 0, sizeof(cgram));
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++)
      pixels[(size_t)y * kSimTownCanvasPixels + x] = 0xFF647814;

  /* A two-marker east-west crossing through a four-cell channel. */
  wram[CellIndex(9, 10)] = 0x25;
  wram[CellIndex(10, 10)] = 0xE2;
  wram[CellIndex(11, 10)] = 0xE2;
  wram[CellIndex(12, 10)] = 0x25;
  /* One north-south crossing, proving the native ids are not transposed. */
  wram[CellIndex(20, 5)] = 0x25;
  wram[CellIndex(20, 6)] = 0xE1;
  wram[CellIndex(20, 7)] = 0x25;
  SetStructureDefinition(wram, 0x44, 0x0300, 0x0301, 0x0302, 0x0303);
  SetStructureDefinition(wram, 0x45, 0x0304, 0x0305, 0x0306, 0x0307);
  SetCanvasCell(wram, 10, 10, 0x0300, 0x0301, 0x0302, 0x0303);
  SetCanvasCell(wram, 11, 10, 0x0300, 0x0301, 0x0302, 0x0303);
  SetCanvasCell(wram, 20, 6, 0x0304, 0x0305, 0x0306, 0x0307);

  SimBackgroundVoxelScene scene;
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  int bridge_count = 0;
  const SimBackgroundVoxelObject *east_west = NULL;
  const SimBackgroundVoxelObject *north_south = NULL;
  for (uint16_t i = 0; i < scene.object_count; i++) {
    const SimBackgroundVoxelObject *bridge = &scene.objects[i];
    if (bridge->kind != kSimBackgroundVoxel_Bridge) continue;
    bridge_count++;
    if (bridge->bridge_axis == kSimBackgroundBridgeAxis_EastWest)
      east_west = bridge;
    if (bridge->bridge_axis == kSimBackgroundBridgeAxis_NorthSouth)
      north_south = bridge;
  }
  CHECK(bridge_count == 2);
  CHECK(scene.unmatched_visual_count == 0);
  CHECK(east_west && east_west->source_cells_w == 2 &&
        east_west->source_cells_h == 1);
  CHECK(east_west && east_west->bridge_bank_a_x == 8 &&
        east_west->bridge_bank_b_x == 13);
  CHECK(north_south && north_south->source_cells_w == 1 &&
        north_south->source_cells_h == 1);
  CHECK(north_south && north_south->bridge_bank_a_y == 4 &&
        north_south->bridge_bank_b_y == 8);

  /* Pale native rail pixels extend outside the nominal rows 4-13 deck band.
   * The whole bridge metatile must therefore be replaced by the original $3A
   * east-west river tile; sampling row 2 of the bridge itself reproduces the
   * detached rail. Give that raw terrain definition a unique blue palette so
   * this proves the exact metatile renderer won over nearby-cell copying. */
  SetSolidColourOneTile(vram, 1);
  SetSolidColourOneTile(vram, 2);
  SetTerrainDefinition(wram, 0x3A, 1, 1, 1, 1);
  const uint16_t north_south_entry = 2 | (1u << 10);
  SetTerrainDefinition(wram, 0x41,
                       north_south_entry, north_south_entry,
                       north_south_entry, north_south_entry);
  cgram[1] = 0x7C00;
  cgram[17] = 0x03E0;
  SimTownCanvas_Reset();
  SimTownCanvas_Render(1, wram, vram, cgram, 15, 0xFF000000);
  uint32_t original_river[16 * 16];
  uint32_t original_river_ns[16 * 16];
  CHECK(SimTownCanvas_RenderTerrainMetatile(
      wram, 0x3A, original_river));
  CHECK(SimTownCanvas_RenderTerrainMetatile(
      wram, 0x41, original_river_ns));
  FillCell(pixels, 9, 10, 0xFF204878);
  FillCell(pixels, 12, 10, 0xFF204878);
  for (int cell_x = 10; cell_x <= 11; cell_x++)
    for (int local_y = 0; local_y < 16; local_y++)
      for (int local_x = 0; local_x < 16; local_x++) {
        size_t at = (size_t)(10 * 16 + local_y) * kSimTownCanvasPixels +
            cell_x * 16 + local_x;
        pixels[at] = local_y == 2 ||
            (local_y >= 4 && local_y < 14)
            ? 0xFFB0A080 : 0xFF204878;
      }
  SimBackgroundVoxels_Reset();
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 91, 91, true);
  const uint32_t *ground = SimBackgroundVoxels_GroundPixels();
  const uint32_t *atlas = SimBackgroundVoxels_AtlasPixels();
  size_t deck = (size_t)(10 * 16 + 8) * kSimTownCanvasPixels + 10 * 16 + 8;
  size_t water = (size_t)(10 * 16 + 2) * kSimTownCanvasPixels + 10 * 16 + 8;
  size_t north_south_deck =
      (size_t)(6 * 16 + 8) * kSimTownCanvasPixels + 20 * 16 + 8;
  CHECK(ground[deck] == original_river[8 * 16 + 8]);
  CHECK(ground[water] == original_river[2 * 16 + 8]);
  CHECK(ground[north_south_deck] == original_river_ns[8 * 16 + 8]);
  CHECK(ground[north_south_deck] != ground[deck]);
  CHECK(ground[deck] != 0xFF204878);
  CHECK((atlas[deck] >> 24) == 0xFF);
  CHECK((atlas[water] >> 24) == 0xFF);
}

int main(void) {
  static uint8_t wram[kWramBytes];
  static uint16_t vram[kVramWords];
  static uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++)
      pixels[(size_t)y * kSimTownCanvasPixels + x] =
          (x % 16 == 8 && y % 16 == 8) ? 0xFF6A8018 : 0xFF647814;

  /* Crossing the quadrant boundary exercises the real paged cell map. */
  wram[CellIndex(15, 15)] = 0xC2;
  wram[CellIndex(16, 15)] = 0xC3;
  wram[CellIndex(15, 16)] = 0xCA;
  wram[CellIndex(16, 16)] = 0xCB;
  wram[CellIndex(6, 6)] = 0x78;

  uint8_t *house = wram + kRecords;
  house[0] = 4; house[1] = 5; house[2] = 0xC0;
  uint8_t *windmill = house + 4;
  windmill[0] = 10; windmill[1] = 11; windmill[2] = 0x83;
  uint8_t *factory = windmill + 4;
  factory[0] = 20; factory[1] = 20; factory[2] = 0x84;
  uint8_t *field = factory + 4;
  field[0] = 25; field[1] = 25; field[2] = 0x82;
  SetStructureDefinition(wram, 0x03, 0x0400, 0x0401, 0x0402, 0x0403);
  SetStructureDefinition(wram, 0x24, 0x0404, 0x0405, 0x0406, 0x0407);
  SetStructureDefinition(wram, 0x36, 0x0408, 0x0409, 0x040A, 0x040B);
  SetCanvasCell(wram, 4, 5, 0x0400, 0x0401, 0x0402, 0x0403);
  SetCanvasCell(wram, 10, 11, 0x0404, 0x0405, 0x0406, 0x0407);
  SetCanvasCell(wram, 20, 20, 0x0408, 0x0409, 0x040A, 0x040B);

  wram[CellIndex(1, 1)] = kTileForest;
  wram[CellIndex(2, 1)] = kTileForest;
  /* A forest fringe cell is accepted only because it is connected to complete
   * canopy cells; the detached one at (20,2) must stay terrain. */
  wram[CellIndex(3, 1)] = kTileForestEdge;
  wram[CellIndex(20, 2)] = kTileForestEdge;
  wram[CellIndex(8, 8)] = kTileForest;
  /* Two clearable bushes, one of them adjacent to the wood. Neither may be
   * absorbed into a forest component. */
  wram[CellIndex(8, 7)] = kTileShrub;
  wram[CellIndex(24, 4)] = kTileShrub;
  /* Marsh reads as green as a canopy but is terrain, not forest. */
  for (int y = 20; y < 24; y++)
    for (int x = 2; x < 6; x++) {
      wram[CellIndex(x, y)] = kTileMarsh;
      FillCell(pixels, x, y, 0xFF087020);
    }
  FillCell(pixels, 1, 1, 0xFF087020);
  FillCell(pixels, 2, 1, 0xFF087020);
  FillCell(pixels, 8, 8, 0xFF087020);
  FillCell(pixels, 6, 6, 0xFF8B5218);
  /* A terrain-coloured pixel is still part of the complete authentic tile;
   * the shallow voxel sides sample its edge rather than guessing a cutout. */
  pixels[(size_t)(6 * 16) * kSimTownCanvasPixels + 6 * 16] = 0xFF647814;
  /* Authentic dark trunk/outline pixels must disappear with the whole tree
   * cell, not survive because only canopy green was treated as foreground. */
  pixels[(size_t)(1 * 16 + 14) * kSimTownCanvasPixels + 1 * 16 + 8] =
      0xFF352010;
  /* Every classified source cell is reference-only, including pixels that
   * resemble terrain rather than the building silhouette. */
  pixels[(size_t)(5 * 16 + 8) * kSimTownCanvasPixels + 4 * 16 + 8] =
      0xFFC06020;

  /* The mountain's top-left 8x8 source remains transparent while its
   * bottom-right source is opaque. This exercises exact BG palette-index
   * alpha rather than rendered-colour similarity. */
  SetSolidColourOneTile(vram, 1);
  SetCanvasTile(wram, 13, 13, 1);

  SimBackgroundVoxelScene scene;
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  CHECK(scene.town == 1);
  CHECK(!scene.overflow);
  CHECK(scene.unmatched_visual_count == 0);
  /* Four structures, four tree cells and two bushes. */
  CHECK(scene.object_count == 10);
  CHECK(scene.tree_cell_count == 4);
  CHECK(scene.tree_group_count == 2);
  CHECK(scene.brush_cell_count == 2);
  CHECK(scene.mountains.cell_count == 1);

  const SimBackgroundVoxelObject *object =
      FindKind(&scene, kSimBackgroundVoxel_House);
  CHECK(object && object->footprint_cells_w == 1 &&
        object->footprint_cells_d == 1);
  CHECK(object &&
        (object->flags & kSimBackgroundVoxel_AlternateFacing));
  CHECK(object &&
        !(object->flags & kSimBackgroundVoxel_UnderConstruction));
  CHECK(object && object->town == 1 && object->development_level == 0);

  object = FindKind(&scene, kSimBackgroundVoxel_Cathedral);
  CHECK(object && object->cell_x == 15 && object->cell_y == 15);
  CHECK(object && object->source_cells_h == 2 &&
        object->footprint_cells_d == 2);

  object = FindKind(&scene, kSimBackgroundVoxel_Windmill);
  CHECK(object && object->source_cells_w == 2 &&
        object->footprint_cells_d == 1);

  object = FindKind(&scene, kSimBackgroundVoxel_Factory);
  CHECK(object && object->footprint_cells_w == 2 &&
        object->footprint_cells_d == 2);

  int isolated = 0, joined = 0;
  for (uint16_t i = 0; i < scene.object_count; i++)
    if (scene.objects[i].kind == kSimBackgroundVoxel_Tree) {
      if (scene.objects[i].flags & kSimBackgroundVoxel_IsolatedTree)
        isolated++;
      else {
        joined++;
        CHECK(scene.objects[i].tree_edges != 0);
      }
    }
  CHECK(isolated == 1);
  CHECK(joined == 3);

  /* The bush beside the wood keeps its own kind and cell rather than joining
   * the forest component next to it. */
  int shrubs = 0, shrub_beside_wood = 0;
  for (uint16_t i = 0; i < scene.object_count; i++) {
    const SimBackgroundVoxelObject *bush = &scene.objects[i];
    if (bush->kind != kSimBackgroundVoxel_Shrub) continue;
    shrubs++;
    CHECK(bush->group == 0);
    CHECK(bush->flags & kSimBackgroundVoxel_IsolatedTree);
    if (bush->cell_x == 8 && bush->cell_y == 7) shrub_beside_wood++;
  }
  CHECK(shrubs == 2);
  CHECK(shrub_beside_wood == 1);

  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1, 1, true);
  const uint32_t *atlas = SimBackgroundVoxels_AtlasPixels();
  const uint32_t *ground = SimBackgroundVoxels_GroundPixels();
  size_t house_corner = (size_t)(5 * 16) * kSimTownCanvasPixels + 4 * 16;
  size_t house_center =
      (size_t)(5 * 16 + 8) * kSimTownCanvasPixels + 4 * 16 + 8;
  CHECK((atlas[house_corner] >> 24) == 0xFF);
  CHECK((atlas[house_center] >> 24) == 0xFF);
  CHECK(ground[house_center] == 0xFF6A8018);
  size_t tree_center = (size_t)(1 * 16 + 8) * kSimTownCanvasPixels +
      1 * 16 + 8;
  size_t tree_trunk = (size_t)(1 * 16 + 14) * kSimTownCanvasPixels +
      1 * 16 + 8;
  CHECK((atlas[tree_center] >> 24) == 0xFF);
  CHECK(ground[tree_center] == 0xFF6A8018);
  CHECK(ground[tree_trunk] == 0xFF647814);
  size_t mountain_corner =
      (size_t)(6 * 16) * kSimTownCanvasPixels + 6 * 16;
  size_t mountain_center =
      (size_t)(6 * 16 + 8) * kSimTownCanvasPixels + 6 * 16 + 8;
  CHECK(ground[mountain_corner] == 0xFF647814);
  CHECK(ground[mountain_center] == 0xFF6A8018);
  CHECK((atlas[mountain_corner] >> 24) == 0);
  CHECK((atlas[mountain_center] >> 24) == 0xFF);
  uint32_t first_serial = SimBackgroundVoxels_Serial();
  SimBackgroundVoxels_Reset();
  CHECK(SimBackgroundVoxels_Serial() == 0);
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1, 1, true);
  CHECK(SimBackgroundVoxels_Serial() != 0);
  CHECK(SimBackgroundVoxels_Serial() != first_serial);

  /* Marahna draws two different trees and the difference is a gameplay one:
   * the palms are clearable brush like Fillmore's bushes, while the broad
   * mangrove family is the permanent forest. Rendering the town's foliage as
   * one kind erased that distinction. */
  memset(wram, 0, sizeof(wram));
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++)
      pixels[(size_t)y * kSimTownCanvasPixels + x] = 0xFF647814;
  wram[TownCellIndex(4, 2, 2)] = kTileBroadleaf;
  wram[TownCellIndex(4, 3, 2)] = kTileBroadleaf;
  wram[TownCellIndex(4, 8, 8)] = kTileBroadleaf;
  /* Two palms, one of them touching the mangrove block. */
  wram[TownCellIndex(4, 4, 2)] = kTilePalm;
  wram[TownCellIndex(4, 20, 20)] = kTilePalm;
  SimBackgroundVoxels_Classify(5, wram, true, &scene);
  CHECK(scene.tree_cell_count == 3);
  CHECK(scene.tree_group_count == 2);
  CHECK(scene.brush_cell_count == 2);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_Tree) == NULL);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_BroadTree) != NULL);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_Palm) != NULL);
  for (uint16_t i = 0; i < scene.object_count; i++) {
    uint8_t kind = scene.objects[i].kind;
    CHECK(kind == kSimBackgroundVoxel_BroadTree ||
          kind == kSimBackgroundVoxel_Palm);
    /* A palm never joins a forest component, however it is surrounded. */
    if (kind == kSimBackgroundVoxel_Palm)
      CHECK(scene.objects[i].group == 0 &&
            (scene.objects[i].flags & kSimBackgroundVoxel_IsolatedTree));
  }

  /* Clearing a bush commits the cleared cell-map value as soon as the miracle
   * resolves and only repaints the BG1 tilemap when the animation ends. For
   * those frames the cell map says grass while the bush art is still drawn;
   * classifying from the cell map alone dropped the model and let the flat
   * authentic sprite pop back mid-strike. The displayed metatile has to win. */
  memset(wram, 0, sizeof(wram));
  SetTerrainDefinition(wram, kTileShrub, 0x0DC8, 0x0DC9, 0x0DD8, 0x0DD9);
  SetTerrainDefinition(wram, kTileGrass, 0x0DE1, 0x0DE1, 0x0DE1, 0x0DE1);
  /* Both cells already read as cleared grass. Only the first still shows the
   * bush; the second has been repainted and must stay cleared. */
  wram[CellIndex(3, 3)] = kTileGrass;
  wram[CellIndex(9, 9)] = kTileGrass;
  SetCanvasCell(wram, 3, 3, 0x0DC8, 0x0DC9, 0x0DD8, 0x0DD9);
  SetCanvasCell(wram, 9, 9, 0x0DE1, 0x0DE1, 0x0DE1, 0x0DE1);
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  CHECK(scene.brush_cell_count == 1);
  object = FindKind(&scene, kSimBackgroundVoxel_Shrub);
  CHECK(object && object->cell_x == 3 && object->cell_y == 3);
  /* Tile priority and the definition's traversal bit differ between a
   * definition and a live entry, and must not defeat the match. */
  SetCanvasCell(wram, 3, 3, 0x2DC8, 0x0DC9, 0x0DD8, 0x0DD9);
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  CHECK(scene.brush_cell_count == 1);
  /* Once the art is repainted the object goes, without waiting for anything
   * else to change. */
  SetCanvasCell(wram, 3, 3, 0x0DE1, 0x0DE1, 0x0DE1, 0x0DE1);
  SimBackgroundVoxels_Classify(1, wram, true, &scene);
  CHECK(scene.brush_cell_count == 0);

  /* Kasandora carries both permanent families at once, so the model cannot be
   * chosen from the town. */
  memset(wram, 0, sizeof(wram));
  wram[TownCellIndex(2, 4, 4)] = kTileForest;
  wram[TownCellIndex(2, 10, 10)] = kTileBroadleaf;
  SimBackgroundVoxels_Classify(3, wram, true, &scene);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_Tree) != NULL);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_BroadTree) != NULL);

  /* A fringe cell takes the family of the block that recruited it, so a wood
   * never changes species at its own edge. */
  memset(wram, 0, sizeof(wram));
  wram[TownCellIndex(4, 6, 6)] = kTileBroadleaf;
  wram[TownCellIndex(4, 7, 6)] = kTileForestEdge;
  SimBackgroundVoxels_Classify(5, wram, true, &scene);
  CHECK(scene.tree_cell_count == 2);
  for (uint16_t i = 0; i < scene.object_count; i++)
    CHECK(scene.objects[i].kind == kSimBackgroundVoxel_BroadTree);

  /* Marahna's sanctuary is the $C0 variant of the same 2x2 plot every other
   * town draws as a cathedral. Recognising only the $C2 family left Marahna
   * with no sanctuary object at all. */
  memset(wram, 0, sizeof(wram));
  wram[TownCellIndex(4, 17, 17)] = 0xC0;
  wram[TownCellIndex(4, 18, 17)] = 0xC1;
  wram[TownCellIndex(4, 17, 18)] = 0xC8;
  wram[TownCellIndex(4, 18, 18)] = 0xC9;
  SimBackgroundVoxels_Classify(5, wram, true, &scene);
  object = FindKind(&scene, kSimBackgroundVoxel_MarahnaTemple);
  CHECK(object && object->cell_x == 17 && object->cell_y == 17);
  CHECK(object && object->footprint_cells_w == 2 &&
        object->footprint_cells_d == 2);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_Cathedral) == NULL);

  /* The eraser is selected from the current town rather than hardcoded to
   * grass: a snowy town must repeat its complete snow tile under replacements. */
  memset(wram, 0, sizeof(wram));
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++)
      pixels[(size_t)y * kSimTownCanvasPixels + x] =
          (x % 16 == 8 && y % 16 == 8) ? 0xFFFFFFFF : 0xFFF0F2E8;
  uint8_t *snow_house = wram + kRecords + 5 * kRecordsPerTown;
  snow_house[0] = 4;
  snow_house[1] = 5;
  snow_house[2] = 0x80;
  SetStructureDefinition(wram, 0x02, 0x0500, 0x0501, 0x0502, 0x0503);
  SetCanvasCell(wram, 4, 5, 0x0500, 0x0501, 0x0502, 0x0503);
  wram[TownCellIndex(5, 1, 1)] = kTileForest;
  FillCell(pixels, 1, 1, 0xFF087020);
  /* Northwall's green cathedral plot surrounds the masked source closely
   * enough to win the generic neighbour vote. It must not become the town's
   * eraser tile when ordinary snow is available elsewhere. */
  for (int y = 13; y <= 18; y++)
    for (int x = 13; x <= 18; x++)
      FillCell(pixels, x, y, 0xFF708030);
  wram[TownCellIndex(5, 14, 14)] = 0xC2;
  wram[TownCellIndex(5, 15, 14)] = 0xC3;
  wram[TownCellIndex(5, 14, 15)] = 0xCA;
  wram[TownCellIndex(5, 15, 15)] = 0xCB;
  SimBackgroundVoxels_Build(6, wram, pixels, vram, 2, 2, true);
  ground = SimBackgroundVoxels_GroundPixels();
  CHECK(ground[tree_center] == 0xFFFFFFFF);
  CHECK(ground[house_center] == 0xFFFFFFFF);
  size_t cathedral_center =
      (size_t)(14 * 16 + 8) * kSimTownCanvasPixels + 14 * 16 + 8;
  CHECK(ground[cathedral_center] == 0xFFFFFFFF);

  /* A snow-coloured Northwall mountain follows the palette-independent rock
   * silhouette. The prior RGB mask erased white rock, while source alpha alone
   * retained the opaque rectangular snow background around the peak. */
  memset(wram, 0, sizeof(wram));
  memset(vram, 0, sizeof(vram));
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++)
      pixels[(size_t)y * kSimTownCanvasPixels + x] = 0xFFF0F2E8;
  wram[TownCellIndex(5, 6, 6)] = 0x81;
  FillCell(pixels, 6, 6, 0xFFFFFFFF);
  SetSolidColourOneTile(vram, 1);
  SetCanvasTile(wram, 13, 13, 1);
  SimBackgroundVoxels_Build(6, wram, pixels, vram, 3, 3, true);
  atlas = SimBackgroundVoxels_AtlasPixels();
  size_t north_mountain_opaque =
      (size_t)(6 * 16 + 3) * kSimTownCanvasPixels + 6 * 16 + 12;
  CHECK((atlas[mountain_corner] >> 24) == 0);
  CHECK((atlas[north_mountain_opaque] >> 24) == 0xFF);

  CheckWindmillFrames();
  CheckStructureVisualCatalog();
  CheckHouseFramesEndToEnd();
  CheckAitosSnapshotHouseFrame();
  CheckUnknownFramesFailClosed();
  CheckBridgeFramesEndToEnd();
  CheckMountainOcclusionReach();
  CheckMarahnaEarthquakeCanvasRebuild();
  CheckMarahnaGroundSourceRejectsWaterDuringFade();
  CheckMarahnaGroundSourceRejectsCliffDuringFade();
  CheckIndependentSceneAndPixelPublications();
  CheckStoneBridgeClassificationAndInpaint();

  if (failures) {
    fprintf(stderr, "%d sim background voxel checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel checks passed");
  return 0;
}
