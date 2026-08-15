#include "sim_background_mountains.h"

#include <stddef.h>
#include <string.h>

enum {
  kTownCellMapsWram = 0x12000,  /* flat $7F:2000 */
  kTownCellMapBytes = 0x400,
};

static size_t CellIndex(int x, int y) {
  return (size_t)y * kSimBackgroundMountainTownCells + (size_t)x;
}

/* The cell maps use four 16x16 pages rather than row-major 32x32 storage. */
static size_t TownCellMapIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kTownCellMapsWram + (size_t)(town - 1) * kTownCellMapBytes +
      (size_t)quadrant * 0x100 + (size_t)(y & 15) * 16 + (x & 15);
}

uint8_t SimBackgroundMountains_TileFlags(uint8_t town, uint8_t tile) {
  if (town < 1 || town > 6) return 0;
  /* Marahna reuses ids from the common mountain range for isolated swamp art.
   * Its grey plateau walls are a separate terrain family and will receive an
   * edge-oriented mesh instead of being forced into this ridge heightfield. */
  if (town == 5) return 0;
  if (tile >= 0x78 && tile <= 0x9F)
    return kSimBackgroundMountainCell_Occupied;
  /* Aitos' two volcanic crown cells sit immediately before the shared ridge
   * range and carry the red cap visible in the source palette. */
  if (town == 4 && (tile == 0x70 || tile == 0x71))
    return kSimBackgroundMountainCell_Occupied |
        kSimBackgroundMountainCell_LavaCap;
  return 0;
}

bool SimBackgroundMountains_CellOccupied(
    const SimBackgroundMountainField *field, int x, int y) {
  if (!field || x < 0 || x >= kSimBackgroundMountainTownCells ||
      y < 0 || y >= kSimBackgroundMountainTownCells)
    return false;
  return (field->flags[CellIndex(x, y)] &
          kSimBackgroundMountainCell_Occupied) != 0;
}

void SimBackgroundMountains_Classify(
    uint8_t town, const uint8_t *wram, SimBackgroundMountainField *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (town < 1 || town > 6 || !wram) return;
  out->town = town;

  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      size_t cell = CellIndex(x, y);
      uint8_t tile = wram[TownCellMapIndex(town, x, y)];
      out->tile[cell] = tile;
      out->flags[cell] = SimBackgroundMountains_TileFlags(town, tile);
      if (out->flags[cell] & kSimBackgroundMountainCell_Occupied) {
        out->cell_count++;
        if (!out->first_cell_by_tile[tile])
          out->first_cell_by_tile[tile] = (uint16_t)(cell + 1);
      }
    }

  uint16_t queue[kSimBackgroundMountainCellCount];
  for (int start_y = 0; start_y < kSimBackgroundMountainTownCells; start_y++)
    for (int start_x = 0; start_x < kSimBackgroundMountainTownCells;
         start_x++) {
      size_t start = CellIndex(start_x, start_y);
      if (!(out->flags[start] & kSimBackgroundMountainCell_Occupied) ||
          out->component[start])
        continue;
      uint16_t component = ++out->component_count;
      int read = 0, write = 0;
      queue[write++] = (uint16_t)start;
      out->component[start] = component;
      while (read < write) {
        int cell = queue[read++];
        int x = cell % kSimBackgroundMountainTownCells;
        int y = cell / kSimBackgroundMountainTownCells;
        static const int dx[] = {0, 1, 0, -1};
        static const int dy[] = {-1, 0, 1, 0};
        for (int edge = 0; edge < 4; edge++) {
          int nx = x + dx[edge], ny = y + dy[edge];
          if (!SimBackgroundMountains_CellOccupied(out, nx, ny)) continue;
          size_t next = CellIndex(nx, ny);
          if (out->component[next]) continue;
          out->component[next] = component;
          queue[write++] = (uint16_t)next;
        }
      }
    }
}

bool SimBackgroundMountains_TileSource(
    const SimBackgroundMountainField *field, uint8_t tile,
    int *cell_x, int *cell_y) {
  if (!field || !cell_x || !cell_y || !field->first_cell_by_tile[tile])
    return false;
  int cell = field->first_cell_by_tile[tile] - 1;
  *cell_x = cell % kSimBackgroundMountainTownCells;
  *cell_y = cell / kSimBackgroundMountainTownCells;
  return true;
}

static void AppendCapTile(
    SimBackgroundMountainCaps *caps, int cell_x, int cell_y,
    uint8_t source_tile, uint8_t flags, uint16_t component) {
  if (caps->tile_count >= kSimBackgroundMountainMaxCapTiles) return;
  caps->tiles[caps->tile_count++] = (SimBackgroundMountainCapTile){
    .cell_x = (int8_t)cell_x,
    .cell_y = (int8_t)cell_y,
    .source_tile = source_tile,
    .flags = flags,
    .component = component,
  };
}

void SimBackgroundMountains_BuildNorthCaps(
    const SimBackgroundMountainField *field, SimBackgroundMountainCaps *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!field || !field->cell_count ||
      !field->first_cell_by_tile[0x8A] ||
      !field->first_cell_by_tile[0x7D] ||
      !field->first_cell_by_tile[0x89] ||
      !field->first_cell_by_tile[0x81] ||
      !field->first_cell_by_tile[0x82])
    return;

  /* Restore both clipped rows above a northern range. The mirrored $7D is
   * deliberately the second row's right face: it keeps the authentic dark
   * slope instead of substituting a flat, unshaded mountain tile.
   *
   *       $82  --  --  $81
   *       $8A $7D mirrored-$7D $89
   *
   * Adjacent groups pair $81/$82 into complete top-row peaks. The first and
   * last map-edge groups retain one authentic half each because the missing
   * half lies outside the level. Only complete a peak beside a genuine gap
   * within the map; never add a third row or replace an original map cell. */
  static const uint8_t row_tiles[4] = {0x8A, 0x7D, 0x7D, 0x89};
  bool restored_groups[kSimBackgroundMountainTownCells / 4] = {false};
  for (int group_x = 0; group_x < kSimBackgroundMountainTownCells;
       group_x += 4) {
    uint16_t component = 0;
    bool complete = true;
    for (int offset = 0; offset < 4; offset++) {
      int cell_x = group_x + offset;
      if (cell_x >= kSimBackgroundMountainTownCells ||
          !SimBackgroundMountains_CellOccupied(field, cell_x, 0)) {
        complete = false;
        break;
      }
      uint16_t candidate = field->component[cell_x];
      if (!component) component = candidate;
      else if (component != candidate) complete = false;
    }
    if (!complete || !component) continue;
    restored_groups[group_x / 4] = true;
    for (int offset = 0; offset < 4; offset++) {
      int cell_x = group_x + offset;
      uint8_t flags =
          offset == 2 ? kSimBackgroundMountainCapTile_MirrorX : 0;
      AppendCapTile(
          out, cell_x, -1, row_tiles[offset], flags, component);
    }
    AppendCapTile(out, group_x, -2, 0x82, 0, component);
    AppendCapTile(out, group_x + 3, -2, 0x81, 0, component);
  }

  for (int group = 0;
       group < kSimBackgroundMountainTownCells / 4; group++) {
    if (!restored_groups[group]) continue;
    int group_x = group * 4;
    uint16_t component = field->component[group_x];
    if (group > 0 && !restored_groups[group - 1])
      AppendCapTile(out, group_x - 1, -2, 0x81, 0, component);
    if (group + 1 < kSimBackgroundMountainTownCells / 4 &&
        !restored_groups[group + 1])
      AppendCapTile(out, group_x + 4, -2, 0x82, 0, component);
  }

  /* Filmora repeats the same nested overlap inside every reconstructed
   * four-cell cap. Preserve the ordinary cap silhouettes, then layer the two
   * demonstrated source fragments on each restored stamp: shaded $82 on the
   * foreground-right and normally lit $89 inside the background-left
   * triangle. This deliberately leaves the original map row and the valid
   * source examples untouched. */
  if (field->town == 1) {
    for (int group = 0;
         group < kSimBackgroundMountainTownCells / 4; group++) {
      if (!restored_groups[group]) continue;
      int group_x = group * 4;
      uint16_t component = field->component[group_x];
      AppendCapTile(out, group_x + 2, -1, 0x82, 0, component);
      AppendCapTile(
          out, group_x + 3, -2, 0x89,
          kSimBackgroundMountainCapTile_TriangleLowerRight, component);
    }
  }

}
