#include "sim/sim_background_mountains.h"
#include "sim/sim_background_mountain_objects.h"

#include <stdio.h>
#include <string.h>

enum {
  kWramBytes = 0x20000,
  kCellMaps = 0x12000,
  kCellMapBytes = 0x400,
};

static int failures;
#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
  } \
} while (0)

static size_t CellIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kCellMaps + (size_t)(town - 1) * kCellMapBytes +
      quadrant * 0x100 + (y & 15) * 16 + (x & 15);
}

static void SetCell(uint8_t *wram, uint8_t town,
                    int x, int y, uint8_t tile) {
  wram[CellIndex(town, x, y)] = tile;
}

static void SetFillmoreLowerMountainComposition(uint8_t *wram) {
  static const struct {
    uint8_t x, y, tile;
  } cells[] = {
    {31, 21, 0x81},
    {30, 22, 0x88}, {31, 22, 0x89},
    {30, 23, 0x90}, {31, 23, 0x79},
    {21, 24, 0x81}, {22, 24, 0x82},
    {24, 24, 0x81}, {25, 24, 0x82},
    {29, 24, 0x81}, {30, 24, 0x78}, {31, 24, 0x89},
    {20, 25, 0x88}, {21, 25, 0x89}, {22, 25, 0x8A},
    {23, 25, 0x7D}, {24, 25, 0x89}, {25, 25, 0x8A},
    {26, 25, 0x8B}, {28, 25, 0x88}, {29, 25, 0x89},
    {30, 25, 0x8A}, {31, 25, 0x7F},
    {20, 26, 0x90}, {21, 26, 0x91}, {22, 26, 0x7C},
    {23, 26, 0x8C}, {24, 26, 0x91}, {25, 26, 0x92},
    {26, 26, 0x8F}, {27, 26, 0x8B}, {28, 26, 0x90},
    {29, 26, 0x91}, {30, 26, 0x92}, {31, 26, 0x87},
    {20, 27, 0x98}, {21, 27, 0x99}, {22, 27, 0x84},
    {23, 27, 0x94}, {24, 27, 0x95}, {25, 27, 0x96},
    {26, 27, 0x97}, {27, 27, 0x9F}, {28, 27, 0x98},
    {29, 27, 0x99}, {30, 27, 0x9A}, {31, 27, 0x9B},
    {22, 28, 0x90}, {23, 28, 0x91}, {24, 28, 0x9D},
    {25, 28, 0x9E}, {26, 28, 0x92}, {27, 28, 0x93},
    {28, 28, 0x8E}, {29, 28, 0x8E},
    {22, 29, 0x98}, {23, 29, 0x99}, {24, 29, 0x9A},
    {25, 29, 0x99}, {26, 29, 0x9A}, {27, 29, 0x9B},
  };
  for (size_t at = 0; at < sizeof(cells) / sizeof(cells[0]); at++)
    SetCell(wram, 1, cells[at].x, cells[at].y, cells[at].tile);
}

typedef struct AuditedMountainTown {
  uint8_t town;
  uint8_t object_count;
  uint32_t occupied_rows[kSimBackgroundMountainTownCells];
} AuditedMountainTown;

/* Canonical mountain occupancy from each town's raw 32x32 terrain map. These
 * bit rows deliberately omit the visual tile ids: the production builder
 * must prove that its independent complete stamps reproduce this silhouette
 * exactly, without importing a neighbouring shoulder or dropping an overlap. */
static const AuditedMountainTown kAuditedMountainTowns[] = {
  {2, 19, {
    0xFFFFFFFFu, 0xFFFC3C3Fu, 0xFFF0000Fu, 0xFFC00003u,
    0xF0000000u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000600u, 0x00000F00u,
    0x00006F00u, 0x0000FF80u, 0x0000FFE0u, 0x0000FFF8u,
  }},
  {3, 44, {
    0x0FFFFFFFu, 0x03FFFC03u, 0x03FFFC00u, 0x03C3FE00u,
    0x00003F80u, 0x00003FC0u, 0x00000FC0u, 0x000003C0u,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80000000u, 0xC0000000u,
    0xD8000000u, 0xFC000000u, 0xFC000000u, 0xFC000000u,
    0xF0000000u, 0xC0000000u, 0x80000000u, 0xE0000000u,
    0xF0000018u, 0xF006667Cu, 0xF01FFFFCu, 0xC07FFFFCu,
  }},
  {4, 42, {
    0xFFFCFFFFu, 0xFFFCFFFFu, 0xFCFCFFFFu, 0xDE3C3C3Fu,
    0xFF003C0Fu, 0xFF003C03u, 0xFF000000u, 0xFF000000u,
    0xC0000300u, 0xC0000780u, 0x00000FC1u, 0x00000FC3u,
    0x80000FC3u, 0xE0006FC3u, 0xF000F003u, 0xF000F003u,
    0xF000F001u, 0xF0000003u, 0xF0000003u, 0xC0000003u,
    0x18000000u, 0x3C000006u, 0x3C00000Fu, 0x3C00000Fu,
    0x1800000Fu, 0x3E000007u, 0x3F00000Fu, 0x3F00000Fu,
    0x3F98001Fu, 0x3FFE667Fu, 0x0FFFFFFFu, 0x0FFFFFFFu,
  }},
  {6, 37, {
    0, 0, 0x00006000u, 0x0000F800u, 0x0000FE00u, 0x9800FF00u,
    0xFE00FF00u, 0xFF01FF00u, 0xFF07F000u, 0xFF0FF000u,
    0xF00FC000u, 0xF00F0006u, 0xF000000Fu, 0xF000000Fu,
    0xF000000Fu, 0xF0180003u, 0x003E0001u, 0x003F0007u,
    0x003F000Fu, 0x000F000Fu, 0x0000000Fu, 0x00000007u,
    0x0000000Fu, 0x0000000Fu, 0x0000001Fu, 0x0000003Fu,
    0x0000003Fu, 0x0060003Fu, 0x80F8180Fu, 0xC6FE7C63u,
    0xDFFFFDF8u, 0xFFFFFFFCu,
  }},
};

static void PopulateAuditedMountainField(
    const AuditedMountainTown *town, SimBackgroundMountainField *field) {
  memset(field, 0, sizeof(*field));
  field->town = town->town;
  for (int tile = 0x70; tile <= 0x9F; tile++)
    field->first_cell_by_tile[tile] = 1;
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      if (town->occupied_rows[y] & (1u << x)) {
        field->flags[y * kSimBackgroundMountainTownCells + x] =
            kSimBackgroundMountainCell_Occupied;
        field->cell_count++;
      }
}

int main(void) {
  static uint8_t wram[kWramBytes];
  SimBackgroundMountainField field;

  SetCell(wram, 1, 0, 0, 0x78);
  SetCell(wram, 1, 1, 0, 0x79);
  SetCell(wram, 1, 20, 20, 0x9F);
  SimBackgroundMountains_Classify(1, wram, &field);
  CHECK(field.town == 1);
  CHECK(field.cell_count == 3);
  CHECK(field.component_count == 2);
  CHECK(field.component[0] == field.component[1]);
  CHECK(field.component[20 * 32 + 20] != field.component[0]);
  int source_x = -1, source_y = -1;
  CHECK(SimBackgroundMountains_TileSource(
      &field, 0x79, &source_x, &source_y));
  CHECK(source_x == 1 && source_y == 0);
  CHECK(!SimBackgroundMountains_TileSource(
      &field, 0x77, &source_x, &source_y));

  /* A partial range without a validated intact stamp uses the generic sparse
   * restoration and never invents Fillmore-specific overlay fragments. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 12; x++) SetCell(wram, 1, x, 0, 0x86);
  SetCell(wram, 1, 10, 0, 0x8A);
  SetCell(wram, 1, 11, 0, 0x7F);
  SetCell(wram, 1, 12, 8, 0x8A);
  SetCell(wram, 1, 13, 8, 0x7D);
  SetCell(wram, 1, 14, 8, 0x89);
  SetCell(wram, 1, 15, 8, 0x82);
  SetCell(wram, 1, 16, 8, 0x81);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountainCaps caps;
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(caps.tile_count == 19);
  CHECK(caps.tiles[0].cell_x == 0 && caps.tiles[0].cell_y == -1 &&
        caps.tiles[0].source_tile == 0x8A);
  CHECK(caps.tiles[2].cell_x == 2 &&
        (caps.tiles[2].flags & kSimBackgroundMountainCapTile_MirrorX));
  CHECK(caps.tiles[14].cell_x == 10 && caps.tiles[14].cell_y == -1 &&
        caps.tiles[14].source_tile == 0x7D &&
        (caps.tiles[14].flags & kSimBackgroundMountainCapTile_MirrorX));
  CHECK(caps.tiles[17].cell_x == 11 && caps.tiles[17].cell_y == -2 &&
        caps.tiles[17].source_tile == 0x81 && caps.tiles[17].flags == 0);
  CHECK(caps.tiles[4].cell_x == 0 && caps.tiles[4].cell_y == -2 &&
        caps.tiles[4].source_tile == 0x82);
  CHECK(caps.tiles[5].cell_x == 3 && caps.tiles[5].cell_y == -2 &&
        caps.tiles[5].source_tile == 0x81);
  CHECK(caps.tiles[18].cell_x == 12 && caps.tiles[18].cell_y == -2 &&
        caps.tiles[18].source_tile == 0x82);
  CHECK(caps.tiles[18].source_cell_x == -1 &&
        caps.tiles[18].source_cell_y == -1);

  /* A range spanning the full map width keeps its authentic half-peaks at
   * both level boundaries instead of inventing cap halves at x=-1 or x=32. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 32; x++) SetCell(wram, 1, x, 0, 0x86);
  SetCell(wram, 1, 21, 24, 0x81);
  SetCell(wram, 1, 22, 24, 0x82);
  SetCell(wram, 1, 20, 25, 0x88);
  SetCell(wram, 1, 21, 25, 0x89);
  SetCell(wram, 1, 22, 25, 0x8A);
  SetCell(wram, 1, 23, 25, 0x7D);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(caps.tile_count == 64);
  CHECK(caps.tiles[0].cell_x == 0 && caps.tiles[0].cell_y == -2 &&
        caps.tiles[0].source_tile == 0x82 &&
        caps.tiles[0].source_cell_x == 22 &&
        caps.tiles[0].source_cell_y == 24);
  CHECK(caps.tiles[1].cell_x == 0 && caps.tiles[1].cell_y == -1 &&
        caps.tiles[1].source_tile == 0x8A &&
        caps.tiles[1].source_cell_x == 22 &&
        caps.tiles[1].source_cell_y == 25);
  CHECK(caps.tiles[2].cell_x == 1 && caps.tiles[2].cell_y == -1 &&
        caps.tiles[2].source_tile == 0x7D &&
        caps.tiles[2].source_cell_x == 23 &&
        caps.tiles[2].source_cell_y == 25);
  CHECK(caps.tiles[3].cell_x == 3 && caps.tiles[3].cell_y == -2 &&
        caps.tiles[3].source_tile == 0x81 &&
        caps.tiles[3].source_cell_x == 21 &&
        caps.tiles[3].source_cell_y == 24);
  CHECK(caps.tiles[4].cell_x == 4 && caps.tiles[4].cell_y == -2 &&
        caps.tiles[4].source_tile == 0x82);
  CHECK(caps.tiles[5].cell_x == 2 && caps.tiles[5].cell_y == -1 &&
        caps.tiles[5].source_tile == 0x88 &&
        caps.tiles[5].source_cell_x == 20 &&
        caps.tiles[5].source_cell_y == 25);
  CHECK(caps.tiles[8].cell_x == 5 && caps.tiles[8].cell_y == -1 &&
        caps.tiles[8].source_tile == 0x7D);
  CHECK(caps.tiles[45].cell_x == 31 && caps.tiles[45].cell_y == -2 &&
        caps.tiles[45].source_tile == 0x81 &&
        caps.tiles[45].source_cell_x == 21 &&
        caps.tiles[45].source_cell_y == 24);
  CHECK(caps.tiles[46].cell_x == 30 && caps.tiles[46].cell_y == -1 &&
        caps.tiles[46].source_tile == 0x88 &&
        caps.tiles[46].source_cell_x == 20 &&
        caps.tiles[46].source_cell_y == 25);
  CHECK(caps.tiles[47].cell_x == 31 && caps.tiles[47].cell_y == -1 &&
        caps.tiles[47].source_tile == 0x89);
  CHECK(caps.tiles[48].cell_x == 1 && caps.tiles[48].cell_y == -1 &&
        caps.tiles[48].source_tile == 0x81 &&
        caps.tiles[48].source_cell_x == 21 &&
        caps.tiles[48].source_cell_y == 24);
  CHECK(caps.tiles[49].cell_x == 2 && caps.tiles[49].cell_y == -1 &&
        caps.tiles[49].source_tile == 0x82);
  for (int tile = 0; tile < caps.tile_count; tile++) {
    CHECK(caps.tiles[tile].cell_x >= 0);
    CHECK(caps.tiles[tile].cell_x < 32);
    CHECK(caps.tiles[tile].flags == 0);
  }

  /* Fillmore's independent objects are assembled from explicit clean
   * metatile definitions. The overlap tiles in the canonical lower range
   * remain composition evidence, but can never leak into an object's outer
   * edge as a square shoulder. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 32; x++) SetCell(wram, 1, x, 0, 0x86);
  SetFillmoreLowerMountainComposition(wram);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  SimBackgroundMountainObjectList objects;
  CHECK(SimBackgroundMountainObjects_Build(&field, &caps, &objects));
  CHECK(objects.count == 29);
  CHECK(objects.objects[0].cell_x == -2 &&
        objects.objects[0].cell_y == -2);
  CHECK(objects.objects[8].cell_x == 30 &&
        objects.objects[8].cell_y == -2);
  CHECK(objects.objects[17].cell_x == -2 &&
        objects.objects[17].cell_y == 0);
  CHECK(objects.objects[23].cell_x == 0 &&
        objects.objects[23].cell_y == 1);
  CHECK(objects.objects[24].cell_x == 16 &&
        objects.objects[24].cell_y == 1);
  CHECK(objects.objects[27].width_cells == 6 &&
        objects.objects[27].height_cells == 6);
  CHECK(objects.objects[27].source_tile[2][0] == 0x88);
  CHECK(objects.objects[27].source_tile[3][0] == 0x90);
  CHECK(objects.objects[27].source_tile[2][0] != 0x7C);
  CHECK(objects.objects[27].source_tile[3][0] != 0x84);
  CHECK(objects.objects[28].cell_x == 28 &&
        objects.objects[28].cell_y == 24);

  /* The intact source cells are the oracle: changing one source metatile is
   * reflected by every generated copy without changing the composer. */
  SetCell(wram, 1, 20, 25, 0x90);
  SimBackgroundMountains_Classify(1, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(caps.tiles[5].source_tile == 0x90);
  CHECK(caps.tiles[46].source_tile == 0x90);

  /* Snowy Northwall ranges use the same complete-source-art restoration; the
   * renderer samples this town's snow-covered tiles and palette. */
  memset(wram, 0, sizeof(wram));
  for (int x = 0; x < 12; x++) SetCell(wram, 6, x, 0, 0x86);
  SetCell(wram, 6, 12, 8, 0x8A);
  SetCell(wram, 6, 13, 8, 0x7D);
  SetCell(wram, 6, 14, 8, 0x89);
  SetCell(wram, 6, 15, 8, 0x82);
  SetCell(wram, 6, 16, 8, 0x81);
  SimBackgroundMountains_Classify(6, wram, &field);
  SimBackgroundMountains_BuildNorthCaps(&field, &caps);
  CHECK(field.town == 6);
  CHECK(caps.tile_count == 19);
  CHECK(caps.tiles[14].source_tile == 0x7D);
  CHECK(caps.tiles[14].flags & kSimBackgroundMountainCapTile_MirrorX);
  CHECK(caps.tiles[17].source_tile == 0x81);
  CHECK(caps.tiles[18].source_tile == 0x82);

  /* Every newly audited town is accepted only when the union of independent
   * stamps exactly equals its canonical mountain cells. A single misplaced
   * or fused peak makes the build fail instead of quietly showing bad art. */
  for (size_t at = 0;
       at < sizeof(kAuditedMountainTowns) /
           sizeof(kAuditedMountainTowns[0]); at++) {
    PopulateAuditedMountainField(&kAuditedMountainTowns[at], &field);
    memset(&caps, 0, sizeof(caps));
    bool built =
        SimBackgroundMountainObjects_Build(&field, &caps, &objects);
    if (!built)
      fprintf(stderr, "audited mountain town %u failed exact cover\n",
              kAuditedMountainTowns[at].town);
    CHECK(built);
    CHECK(objects.count == kAuditedMountainTowns[at].object_count);
    int volcanoes = 0;
    for (int object = 0; object < objects.count; object++) {
      if (objects.objects[object].flags &
          kSimBackgroundMountainObject_Volcano) {
        volcanoes++;
        CHECK(kAuditedMountainTowns[at].town == 4);
        CHECK(objects.objects[object].cell_x == 6);
        CHECK(objects.objects[object].cell_y == 8);
        CHECK(objects.objects[object].source_tile[0][2] == 0x70);
        CHECK(objects.objects[object].source_tile[0][3] == 0x71);
      }
      for (int row = 0;
           row < objects.objects[object].height_cells; row++)
        for (int column = 0;
             column < objects.objects[object].width_cells; column++) {
          uint8_t tile = objects.objects[object].source_tile[row][column];
          CHECK(tile != 0x8D && tile != 0x8E);
        }
    }
    CHECK(volcanoes == (kAuditedMountainTowns[at].town == 4 ? 1 : 0));
  }

  /* Kasandora does not place a clean $8B right shoulder in its composed map.
   * The object oracle must retain $8B as the semantic source; the atlas builds
   * it directly from the raw terrain definition instead of substituting fused
   * $7D and importing a neighbour's partial peak. */
  const AuditedMountainTown *kasandora = &kAuditedMountainTowns[1];
  PopulateAuditedMountainField(kasandora, &field);
  field.first_cell_by_tile[0x8B] = 0;
  field.first_cell_by_tile[0x7D] = 1;
  memset(&caps, 0, sizeof(caps));
  CHECK(SimBackgroundMountainObjects_Build(&field, &caps, &objects));
  CHECK(objects.count == kasandora->object_count);
  CHECK(objects.objects[0].source_tile[1][3] == 0x8B);
  bool has_north_0 = false, has_north_4 = false, has_north_8 = false;
  bool has_north_12 = false, has_north_16 = false, has_north_20 = false;
  bool has_north_stagger = false, has_north_front = false;
  bool has_south_2 = false, has_south_6 = false, has_south_16 = false;
  bool has_old_shift_2 = false, has_old_shift_6 = false;
  for (int object = 0; object < objects.count; object++) {
    for (int row = 0; row < objects.objects[object].height_cells; row++)
      for (int column = 0;
           column < objects.objects[object].width_cells; column++)
        CHECK(objects.objects[object].source_tile[row][column] != 0x7D);
    int x = objects.objects[object].cell_x;
    int y = objects.objects[object].cell_y;
    has_north_0 |= x == 0 && y == -3;
    has_north_4 |= x == 4 && y == -3;
    has_north_8 |= x == 8 && y == -3;
    has_north_12 |= x == 12 && y == -3;
    has_north_16 |= x == 16 && y == -3;
    has_north_20 |= x == 20 && y == -3;
    has_north_stagger |= x == 10 && y == -2;
    has_north_front |= x == 10 && y == 0;
    has_south_2 |= x == 2 && y == 30;
    has_south_6 |= x == 6 && y == 30;
    has_south_16 |= x == 16 && y == 31;
    has_old_shift_2 |= x == 2 && y == -3;
    has_old_shift_6 |= x == 6 && y == -3;
  }
  CHECK(has_north_0 && has_north_4 && has_north_8);
  CHECK(has_north_12 && has_north_16 && has_north_20);
  CHECK(has_north_stagger && has_north_front);
  CHECK(has_south_2 && has_south_6 && has_south_16);
  CHECK(!has_old_shift_2 && !has_old_shift_6);

  /* Marahna's $8D is isolated swamp decoration, not common-range rock. */
  SetCell(wram, 5, 4, 4, 0x8D);
  SimBackgroundMountains_Classify(5, wram, &field);
  CHECK(field.cell_count == 0);
  CHECK(SimBackgroundMountains_TileFlags(5, 0x8D) == 0);
  CHECK(SimBackgroundMountains_TileFlags(1, 0x8D) == 0);
  CHECK(SimBackgroundMountains_TileFlags(1, 0x8E) == 0);
  CHECK(SimBackgroundMountains_TileFlags(6, 0x8D) == 0);
  CHECK(SimBackgroundMountains_TileFlags(6, 0x8E) == 0);

  SetCell(wram, 4, 5, 5, 0x70);
  SetCell(wram, 4, 6, 5, 0x71);
  SetCell(wram, 4, 7, 5, 0x78);
  SimBackgroundMountains_Classify(4, wram, &field);
  CHECK(field.cell_count == 3);
  CHECK(field.component_count == 1);
  CHECK((field.flags[5 * 32 + 5] &
         kSimBackgroundMountainCell_LavaCap) != 0);
  CHECK((field.flags[5 * 32 + 7] &
         kSimBackgroundMountainCell_LavaCap) == 0);

  SetCell(wram, 6, 31, 31, 0x80);
  SimBackgroundMountains_Classify(6, wram, &field);
  CHECK(SimBackgroundMountains_CellOccupied(&field, 31, 31));
  CHECK(!SimBackgroundMountains_CellOccupied(&field, 32, 31));

  memset(&field, 0xFF, sizeof(field));
  SimBackgroundMountains_Classify(0, wram, &field);
  CHECK(field.town == 0 && field.cell_count == 0);

  if (failures) return 1;
  puts("sim background mountain classification checks passed");
  return 0;
}
