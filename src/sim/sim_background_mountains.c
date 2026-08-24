#include "sim_background_mountains.h"

#include "sim_town_layout.h"

#include <stddef.h>
#include <string.h>

static size_t CellIndex(int x, int y) {
  return (size_t)y * kSimBackgroundMountainTownCells + (size_t)x;
}

uint8_t SimBackgroundMountains_TileFlags(uint8_t town, uint8_t tile) {
  if (town < 1 || town > 6) return 0;
  /* Marahna reuses ids from the common mountain range for isolated swamp art.
   * Its grey plateau walls are a separate terrain family and will receive an
   * edge-oriented mesh instead of being forced into this ridge heightfield. */
  if (town == 5) return 0;
  /* $8D/$8E are deliberate holes between overlapping peaks: grass in the
   * temperate towns and plain snow in Northwall. Treating the numeric gap as
   * one continuous mountain family pulled those ground cells into the range
   * silhouette and produced square shoulders/horns. */
  if (tile >= 0x78 && tile <= 0x9F && tile != 0x8D && tile != 0x8E)
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
      uint8_t tile = wram[SimTownLayout_CellMapIndex(town, x, y)];
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
    .source_cell_x = -1,
    .source_cell_y = -1,
    .source_tile = source_tile,
    .flags = flags,
    .component = component,
  };
}

typedef struct SimBackgroundMountainNorthStamp {
  uint8_t town;
  uint8_t source_cell_x, source_cell_y;
  uint8_t width_cells;
  uint8_t repeat_cells;
  uint8_t center_offset;
  uint8_t row_occupied_mask[2];
} SimBackgroundMountainNorthStamp;

/* Fillmore's intact small mountain at cells (20,24)..(23,25) is the oracle
 * for the two rows clipped from its northern range. Further towns can supply
 * their own descriptor without sharing Fillmore's tile ids or palette. */
static const SimBackgroundMountainNorthStamp kNorthStamps[] = {
  {
    .town = 1,
    .source_cell_x = 20,
    .source_cell_y = 24,
    .width_cells = 4,
    .repeat_cells = 4,
    .center_offset = 2,
    .row_occupied_mask = {0x06, 0x0F},
  },
};

static const SimBackgroundMountainNorthStamp *NorthStampForTown(
    uint8_t town) {
  for (size_t at = 0; at < sizeof(kNorthStamps) / sizeof(kNorthStamps[0]); at++)
    if (kNorthStamps[at].town == town) return &kNorthStamps[at];
  return NULL;
}

static bool FullNorthRangeRestored(
    const bool restored_groups[kSimBackgroundMountainTownCells / 4]) {
  for (int group = 0;
       group < kSimBackgroundMountainTownCells / 4; group++)
    if (!restored_groups[group]) return false;
  return true;
}

static bool NorthStampAvailable(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainNorthStamp *stamp) {
  if (!field || !stamp || field->town != stamp->town ||
      !stamp->width_cells || stamp->width_cells > 8 ||
      !stamp->repeat_cells || stamp->center_offset >= stamp->width_cells ||
      stamp->source_cell_x + stamp->width_cells >
          kSimBackgroundMountainTownCells ||
      stamp->source_cell_y + 2 > kSimBackgroundMountainTownCells)
    return false;
  for (int row = 0; row < 2; row++)
    for (int offset = 0; offset < stamp->width_cells; offset++) {
      bool expected = (stamp->row_occupied_mask[row] & (1u << offset)) != 0;
      if (SimBackgroundMountains_CellOccupied(
              field, stamp->source_cell_x + offset,
              stamp->source_cell_y + row) != expected)
        return false;
    }
  return true;
}

static void AppendStampRow(
    SimBackgroundMountainCaps *caps,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainNorthStamp *stamp,
    int source_row, int destination_x, int destination_y,
    uint16_t component) {
  for (int offset = 0; offset < stamp->width_cells; offset++) {
    if (!(stamp->row_occupied_mask[source_row] & (1u << offset))) continue;
    int cell_x = destination_x + offset;
    if (cell_x < 0 || cell_x >= kSimBackgroundMountainTownCells) continue;
    int source_x = stamp->source_cell_x + offset;
    int source_y = stamp->source_cell_y + source_row;
    size_t source = CellIndex(source_x, source_y);
    if (caps->tile_count >= kSimBackgroundMountainMaxCapTiles) return;
    caps->tiles[caps->tile_count++] = (SimBackgroundMountainCapTile){
      .cell_x = (int8_t)cell_x,
      .cell_y = (int8_t)destination_y,
      .source_cell_x = (int8_t)source_x,
      .source_cell_y = (int8_t)source_y,
      .source_tile = field->tile[source],
      .component = component,
    };
  }
}

static void BuildNorthCapsFromStamp(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainNorthStamp *stamp,
    SimBackgroundMountainCaps *out) {
  /* The northern range interleaves the intact stamp in two rows: background
   * copies are one cell higher and foreground copies are shifted half a stamp
   * right. Only the rows clipped above the source map are emitted; authentic
   * row zero already contains the remaining overlaps. Boundary copies are
   * clipped to the map, naturally leaving a half-mountain at each outer edge. */

  for (int center_x = 0;
       center_x <= kSimBackgroundMountainTownCells;
       center_x += stamp->repeat_cells) {
    int component_x = center_x;
    if (component_x == kSimBackgroundMountainTownCells) component_x--;
    uint16_t component = field->component[component_x];
    int destination_x = center_x - stamp->center_offset;
    AppendStampRow(out, field, stamp, 0, destination_x, -2, component);
    AppendStampRow(out, field, stamp, 1, destination_x, -1, component);
  }

  for (int destination_x = 0;
       destination_x < kSimBackgroundMountainTownCells;
       destination_x += stamp->repeat_cells) {
    uint16_t component = field->component[destination_x];
    AppendStampRow(out, field, stamp, 0, destination_x, -1, component);
  }
}

void SimBackgroundMountains_BuildNorthCaps(
    const SimBackgroundMountainField *field, SimBackgroundMountainCaps *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!field || !field->cell_count) return;

  bool restored_groups[kSimBackgroundMountainTownCells / 4] = {false};
  for (int group_x = 0; group_x < kSimBackgroundMountainTownCells;
       group_x += 4) {
    uint16_t component = 0;
    bool complete = true;
    for (int offset = 0; offset < 4; offset++) {
      int cell_x = group_x + offset;
      if (!SimBackgroundMountains_CellOccupied(field, cell_x, 0)) {
        complete = false;
        break;
      }
      uint16_t candidate = field->component[cell_x];
      if (!component) component = candidate;
      else if (component != candidate) complete = false;
    }
    restored_groups[group_x / 4] = complete && component;
  }

  const SimBackgroundMountainNorthStamp *stamp =
      NorthStampForTown(field->town);
  if (FullNorthRangeRestored(restored_groups) &&
      NorthStampAvailable(field, stamp)) {
    BuildNorthCapsFromStamp(field, stamp, out);
    return;
  }

  if (!field->first_cell_by_tile[0x8A] ||
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
  for (int group_x = 0; group_x < kSimBackgroundMountainTownCells;
       group_x += 4) {
    if (!restored_groups[group_x / 4]) continue;
    uint16_t component = field->component[group_x];
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

}
