#include "sim/sim_background_voxels.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramBytes = 0x20000,
  kCellMaps = 0x12000,
  kCellMapBytes = 0x400,
  kRecords = 0x16BE7,
  kRecordsPerTown = 0x200,
  kVramWords = 0x8000,
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
  SimBackgroundVoxels_Classify(1, wram, &scene);
  CHECK(scene.town == 1);
  CHECK(!scene.overflow);
  /* Four structures, four tree cells and two bushes. */
  CHECK(scene.object_count == 10);
  CHECK(scene.tree_cell_count == 4);
  CHECK(scene.tree_group_count == 2);
  CHECK(scene.brush_cell_count == 2);
  CHECK(scene.mountains.cell_count == 1);

  const SimBackgroundVoxelObject *object =
      FindKind(&scene, kSimBackgroundVoxel_House);
  CHECK(object && object->height_pixels == 16);
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
        object->footprint_cells_d == 2 && object->height_pixels == 24);

  object = FindKind(&scene, kSimBackgroundVoxel_Windmill);
  CHECK(object && object->source_cells_w == 2 &&
        object->footprint_cells_d == 1 && object->height_pixels == 32);

  object = FindKind(&scene, kSimBackgroundVoxel_Factory);
  CHECK(object && object->footprint_cells_w == 2 &&
        object->footprint_cells_d == 2 && object->height_pixels == 8);

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
    CHECK(bush->group == 0 && bush->height_pixels == 12);
    CHECK(bush->flags & kSimBackgroundVoxel_IsolatedTree);
    if (bush->cell_x == 8 && bush->cell_y == 7) shrub_beside_wood++;
  }
  CHECK(shrubs == 2);
  CHECK(shrub_beside_wood == 1);

  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1);
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
  SimBackgroundVoxels_Build(1, wram, pixels, vram, 1);
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
  SimBackgroundVoxels_Classify(5, wram, &scene);
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
  SimBackgroundVoxels_Classify(1, wram, &scene);
  CHECK(scene.brush_cell_count == 1);
  object = FindKind(&scene, kSimBackgroundVoxel_Shrub);
  CHECK(object && object->cell_x == 3 && object->cell_y == 3);
  /* Tile priority and the definition's traversal bit differ between a
   * definition and a live entry, and must not defeat the match. */
  SetCanvasCell(wram, 3, 3, 0x2DC8, 0x0DC9, 0x0DD8, 0x0DD9);
  SimBackgroundVoxels_Classify(1, wram, &scene);
  CHECK(scene.brush_cell_count == 1);
  /* Once the art is repainted the object goes, without waiting for anything
   * else to change. */
  SetCanvasCell(wram, 3, 3, 0x0DE1, 0x0DE1, 0x0DE1, 0x0DE1);
  SimBackgroundVoxels_Classify(1, wram, &scene);
  CHECK(scene.brush_cell_count == 0);

  /* Kasandora carries both permanent families at once, so the model cannot be
   * chosen from the town. */
  memset(wram, 0, sizeof(wram));
  wram[TownCellIndex(2, 4, 4)] = kTileForest;
  wram[TownCellIndex(2, 10, 10)] = kTileBroadleaf;
  SimBackgroundVoxels_Classify(3, wram, &scene);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_Tree) != NULL);
  CHECK(FindKind(&scene, kSimBackgroundVoxel_BroadTree) != NULL);

  /* A fringe cell takes the family of the block that recruited it, so a wood
   * never changes species at its own edge. */
  memset(wram, 0, sizeof(wram));
  wram[TownCellIndex(4, 6, 6)] = kTileBroadleaf;
  wram[TownCellIndex(4, 7, 6)] = kTileForestEdge;
  SimBackgroundVoxels_Classify(5, wram, &scene);
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
  SimBackgroundVoxels_Classify(5, wram, &scene);
  object = FindKind(&scene, kSimBackgroundVoxel_MarahnaTemple);
  CHECK(object && object->cell_x == 17 && object->cell_y == 17);
  CHECK(object && object->footprint_cells_w == 2 &&
        object->footprint_cells_d == 2 && object->height_pixels == 24);
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
  SimBackgroundVoxels_Build(6, wram, pixels, vram, 2);
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
  SimBackgroundVoxels_Build(6, wram, pixels, vram, 3);
  atlas = SimBackgroundVoxels_AtlasPixels();
  size_t north_mountain_opaque =
      (size_t)(6 * 16 + 3) * kSimTownCanvasPixels + 6 * 16 + 12;
  CHECK((atlas[mountain_corner] >> 24) == 0);
  CHECK((atlas[north_mountain_opaque] >> 24) == 0xFF);

  if (failures) {
    fprintf(stderr, "%d sim background voxel checks failed\n", failures);
    return 1;
  }
  puts("sim background voxel checks passed");
  return 0;
}
