#include "sim_world_navigation_towns.h"

#include <stddef.h>
#include <string.h>

#include "sim_background_voxel_landmarks.h"
#include "sim_town_layout.h"

enum {
  kDevelopmentTiersWram = 0x16B18,
  kStructureRecordsWram = 0x16BE7,
  kStructureRecordsPerTownBytes = 0x200,
  kStructureRecordBytes = 4,
  kStructureRecordCount = 128,
  kStructureActive = 0x80,
  kStructureAlternateFacing = 0x40,
  kStructureClassMask = 0x0F,
  kStructureDevelopmentMask = 0x30,
  kStructureDevelopmentShift = 4,
  kStructureClassHouse = 0,
  kStructureClassBridge = 1,
  kStructureClassField = 2,
  kStructureClassWindmill = 3,
  kStructureClassFactory = 4,
  kTownCells = 32,
  kTownCellCount = kTownCells * kTownCells,
};

typedef enum NavigationFoliageClass {
  kNavigationFoliage_None,
  kNavigationFoliage_Shrub,
  kNavigationFoliage_Palm,
  kNavigationFoliage_Evergreen,
  kNavigationFoliage_Broadleaf,
  kNavigationFoliage_Edge,
} NavigationFoliageClass;

/* These are the same universal terrain-atlas families audited by the full
 * town voxel classifier. Navigation consumes the retained all-town cell maps,
 * so it cannot ask the active town's live tilemap which definition is drawn. */
static NavigationFoliageClass FoliageClassForCell(uint8_t tile) {
  switch (tile) {
    case 0x01:
      return kNavigationFoliage_Shrub;
    case 0x09:
      return kNavigationFoliage_Palm;
    case 0x02: case 0x03: case 0x04:
    case 0x0A: case 0x0B: case 0x0C:
    case 0x12: case 0x13: case 0x14:
    case 0x1A: case 0x1B: case 0x1C:
    case 0x23: case 0x24:
      return kNavigationFoliage_Evergreen;
    case 0x05: case 0x06: case 0x07:
    case 0x0D: case 0x0E: case 0x0F:
    case 0x15: case 0x16: case 0x17:
    case 0x1D: case 0x1E: case 0x1F:
    case 0x26: case 0x27:
      return kNavigationFoliage_Broadleaf;
    case 0x22:
      return kNavigationFoliage_Edge;
    default:
      return kNavigationFoliage_None;
  }
}

static size_t TownCellIndex(int x, int y) {
  return (size_t)y * kTownCells + (size_t)x;
}

static void MarkOccupied(
    bool occupied[kTownCellCount], int x, int y, int width, int depth) {
  for (int row = 0; row < depth; row++)
    for (int column = 0; column < width; column++) {
      const int cell_x = x + column;
      const int cell_y = y + row;
      if (cell_x >= 0 && cell_x < kTownCells &&
          cell_y >= 0 && cell_y < kTownCells)
        occupied[TownCellIndex(cell_x, cell_y)] = true;
    }
}

static bool TownHasRetainedData(const uint8_t *wram, uint8_t town) {
  const size_t at = kDevelopmentTiersWram + (size_t)(town - 1) * 2;
  /* Match the ground compositor's native development gate. Structure records
   * can survive a progress reset while the loader clears the corresponding
   * cell map. Those stale records must not build a ghost city on pristine
   * desert (or interpret stray pre-unlock markers as a populated town). */
  return (wram[at] | wram[at + 1]) != 0;
}

static bool Append(
    SimWorldNavigationTowns *out,
    SimWorldNavigationTownObject object) {
  if (out->object_count >= kSimWorldNavigationTownObjectCapacity) {
    out->overflow = true;
    return false;
  }
  out->objects[out->object_count++] = object;
  if (object.town >= 1 && object.town <= kSimTownCount)
    for (int y = object.cell_y;
         y < object.cell_y + object.source_cells_h && y < kTownCells; y++)
      for (int x = object.cell_x;
           x < object.cell_x + object.source_cells_w && x < kTownCells; x++)
        out->ground.object_rows[object.town - 1][y] |= UINT32_C(1) << x;
  return true;
}

static void CaptureStructures(
    const uint8_t *wram, uint8_t town,
    SimWorldNavigationTowns *out, bool occupied[kTownCellCount]) {
  const uint8_t *records = wram + kStructureRecordsWram +
      (size_t)(town - 1) * kStructureRecordsPerTownBytes;
  for (int slot = 0; slot < kStructureRecordCount; slot++) {
    const uint8_t *record = records + slot * kStructureRecordBytes;
    const uint8_t flags = record[2];
    if (!(flags & kStructureActive)) continue;
    uint8_t kind;
    uint8_t cells = 1;
    switch (flags & kStructureClassMask) {
      case kStructureClassHouse:
        kind = kSimBackgroundVoxel_House;
        break;
      case kStructureClassBridge:
        /* The retained cell map supplies the complete bank-to-bank span. */
        continue;
      case kStructureClassField:
        kind = kSimWorldNavigationTown_Field;
        cells = 2;
        break;
      case kStructureClassWindmill:
        kind = kSimBackgroundVoxel_Windmill;
        cells = 2;
        break;
      case kStructureClassFactory:
        kind = kSimBackgroundVoxel_Factory;
        cells = 2;
        break;
      default:
        kind = kSimWorldNavigationTown_Support;
        cells = 2;
        break;
    }
    if (record[0] + cells > 32 || record[1] + cells > 32) continue;
    const SimWorldNavigationTownObject object = {
          .town = town,
          .kind = kind,
          .flags = kind == kSimBackgroundVoxel_House &&
              (flags & kStructureAlternateFacing)
              ? kSimBackgroundVoxel_AlternateFacing : 0,
          .development_level = (uint8_t)(
              (flags & kStructureDevelopmentMask) >>
              kStructureDevelopmentShift),
          .record_slot = (uint8_t)slot,
          .cell_x = record[0],
          .cell_y = record[1],
          .source_cells_w = cells,
          .source_cells_h = cells,
          .footprint_cells_w = cells,
          .footprint_cells_d =
              kind == kSimBackgroundVoxel_Windmill ? 1 : cells,
          .visual_state = kSimStructureVisualState_Finished,
        };
    if (!Append(out, object))
      return;
    MarkOccupied(occupied, object.cell_x, object.cell_y,
                 object.source_cells_w, object.source_cells_h);
  }
}

static bool SanctuaryAt(
    const uint8_t *wram, uint8_t town, int x, int y, uint8_t base) {
  return wram[SimTownLayout_CellMapIndex(town, x, y)] == base &&
      wram[SimTownLayout_CellMapIndex(town, x + 1, y)] ==
          (uint8_t)(base + 1) &&
      wram[SimTownLayout_CellMapIndex(town, x, y + 1)] ==
          (uint8_t)(base + 8) &&
      wram[SimTownLayout_CellMapIndex(town, x + 1, y + 1)] ==
          (uint8_t)(base + 9);
}

static void CaptureSanctuary(
    const uint8_t *wram, uint8_t town,
    SimWorldNavigationTowns *out, bool occupied[kTownCellCount]) {
  for (int y = 0; y < 31; y++) {
    for (int x = 0; x < 31; x++) {
      /* Match the retained sanctuary art, not a town-name assumption.
       * Marahna can contain the ordinary cathedral as well as the temple
       * variant, just as the full town classifier already recognizes. */
      const uint8_t base = wram[SimTownLayout_CellMapIndex(town, x, y)];
      if (base != 0xC0 && base != 0xC2) continue;
      if (!SanctuaryAt(wram, town, x, y, base)) continue;
      (void)Append(out, (SimWorldNavigationTownObject){
        .town = town,
        .kind = base == 0xC0
            ? kSimBackgroundVoxel_MarahnaTemple
            : kSimBackgroundVoxel_Cathedral,
        .record_slot = kSimBackgroundVoxelNoRecordSlot,
        .cell_x = (uint8_t)x,
        .cell_y = (uint8_t)y,
        .source_cells_w = 2,
        .source_cells_h = 2,
        .footprint_cells_w = 2,
        .footprint_cells_d = 2,
      });
      MarkOccupied(occupied, x, y, 2, 2);
      return;
    }
  }
}

static void CaptureLandmarks(
    const uint8_t *wram, uint8_t town,
    SimWorldNavigationTowns *out, bool occupied[kTownCellCount]) {
  SimBackgroundVoxelObject landmarks[3];
  const size_t count = SimBackgroundVoxelLandmarks_Classify(
      town, wram, landmarks, sizeof(landmarks) / sizeof(landmarks[0]));
  for (size_t i = 0; i < count; i++) {
    const SimBackgroundVoxelObject *source = &landmarks[i];
    if (!Append(out, *source))
      return;
    MarkOccupied(occupied, source->cell_x, source->cell_y,
                 source->footprint_cells_w, source->footprint_cells_d);
  }
}

static bool BridgeWater(uint8_t tile) {
  switch (tile) {
    case 0x10: case 0x11: case 0x18: case 0x19:
    case 0x20: case 0x21: case 0x25:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
    case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A:
    case 0x3B: case 0x3C: case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0xB0: case 0xB1: case 0xB2:
    case 0xB8: case 0xB9: case 0xBA: case 0xF7: case 0xFE:
    case 0xE1: case 0xE2:
      return true;
    default: return false;
  }
}

static bool BridgeBank(const uint8_t *wram, uint8_t town,
                       int x, int y, int dx, int dy,
                       uint8_t *out_x, uint8_t *out_y) {
  for (;;) {
    x += dx;
    y += dy;
    if (x < 0 || y < 0 || x >= kTownCells || y >= kTownCells)
      return false;
    if (BridgeWater(wram[SimTownLayout_CellMapIndex(town, x, y)]))
      continue;
    *out_x = (uint8_t)x;
    *out_y = (uint8_t)y;
    return true;
  }
}

static void CaptureBridges(const uint8_t *wram, uint8_t town,
                           SimWorldNavigationTowns *out,
                           bool occupied[kTownCellCount]) {
  bool visited[kTownCellCount] = {false};
  for (int y = 0; y < kTownCells; y++)
    for (int x = 0; x < kTownCells; x++) {
      const uint8_t tile = wram[SimTownLayout_CellMapIndex(town, x, y)];
      if (visited[TownCellIndex(x, y)] ||
          (tile != 0xE1 && tile != 0xE2)) continue;
      const int dx = tile == 0xE2, dy = tile == 0xE1;
      int last_x = x, last_y = y;
      while (last_x + dx < kTownCells && last_y + dy < kTownCells &&
             wram[SimTownLayout_CellMapIndex(
                 town, last_x + dx, last_y + dy)] == tile) {
        last_x += dx;
        last_y += dy;
      }
      for (int bx = x, by = y;; bx += dx, by += dy) {
        visited[TownCellIndex(bx, by)] = true;
        occupied[TownCellIndex(bx, by)] = true;
        if (bx == last_x && by == last_y) break;
      }
      SimWorldNavigationTownObject bridge = {
        .town = town, .kind = kSimBackgroundVoxel_Bridge,
        .cell_x = (uint8_t)x, .cell_y = (uint8_t)y,
        .source_cells_w = (uint8_t)(last_x - x + 1),
        .source_cells_h = (uint8_t)(last_y - y + 1),
        .footprint_cells_w = (uint8_t)(last_x - x + 1),
        .footprint_cells_d = (uint8_t)(last_y - y + 1),
        .record_slot = kSimBackgroundVoxelNoRecordSlot,
        .bridge_axis = dx ? kSimBackgroundBridgeAxis_EastWest
                          : kSimBackgroundBridgeAxis_NorthSouth,
        .visual_state = kSimStructureVisualState_Finished,
      };
      if (!BridgeBank(wram, town, x, y, -dx, -dy,
                      &bridge.bridge_bank_a_x, &bridge.bridge_bank_a_y) ||
          !BridgeBank(wram, town, last_x, last_y, dx, dy,
                      &bridge.bridge_bank_b_x, &bridge.bridge_bank_b_y))
        continue;
      if (!Append(out, bridge)) return;
    }
}

static void CaptureFoliage(
    const uint8_t *wram, uint8_t town,
    SimWorldNavigationTowns *out, const bool occupied[kTownCellCount]) {
  uint8_t foliage[kTownCellCount] = {0};
  bool visited[kTownCellCount] = {false};
  const int dx[4] = {0, 1, 0, -1};
  const int dy[4] = {-1, 0, 1, 0};
  uint16_t queue[kTownCellCount];
  int count = 0;
  for (int y = 0; y < kTownCells; y++)
    for (int x = 0; x < kTownCells; x++) {
      const size_t cell = TownCellIndex(x, y);
      if (occupied[cell]) continue;
      foliage[cell] = (uint8_t)FoliageClassForCell(
          wram[SimTownLayout_CellMapIndex(town, x, y)]);
      if (foliage[cell] == kNavigationFoliage_Evergreen ||
          foliage[cell] == kNavigationFoliage_Broadleaf)
        queue[count++] = (uint16_t)cell;
    }
  /* Recruit the same fringe cells as the town forest classifier. */
  for (int read = 0; read < count; read++) {
    const int cell = queue[read];
    for (int edge = 0; edge < 4; edge++) {
      const int nx = cell % kTownCells + dx[edge];
      const int ny = cell / kTownCells + dy[edge];
      if (nx < 0 || ny < 0 || nx >= kTownCells || ny >= kTownCells)
        continue;
      const size_t next = TownCellIndex(nx, ny);
      if (foliage[next] != kNavigationFoliage_Edge) continue;
      foliage[next] = foliage[cell];
      queue[count++] = (uint16_t)next;
    }
  }
  uint16_t group = 0;
  for (int start = 0; start < kTownCellCount; start++) {
    if (visited[start] || !foliage[start] ||
        foliage[start] == kNavigationFoliage_Edge) continue;
    const bool forest = foliage[start] == kNavigationFoliage_Evergreen ||
        foliage[start] == kNavigationFoliage_Broadleaf;
    count = 1;
    queue[0] = (uint16_t)start;
    visited[start] = true;
    if (forest) {
      group++;
      for (int read = 0; read < count; read++) {
        const int cell = queue[read];
        for (int edge = 0; edge < 4; edge++) {
          const int nx = cell % kTownCells + dx[edge];
          const int ny = cell / kTownCells + dy[edge];
          if (nx < 0 || ny < 0 || nx >= kTownCells || ny >= kTownCells)
            continue;
          const size_t next = TownCellIndex(nx, ny);
          if (visited[next] ||
              (foliage[next] != kNavigationFoliage_Evergreen &&
               foliage[next] != kNavigationFoliage_Broadleaf)) continue;
          visited[next] = true;
          queue[count++] = (uint16_t)next;
        }
      }
    }
    for (int item = 0; item < count; item++) {
      const int cell = queue[item];
      const int x = cell % kTownCells, y = cell / kTownCells;
      uint8_t edges = 0;
      if (forest)
        for (int edge = 0; edge < 4; edge++) {
          const int nx = x + dx[edge], ny = y + dy[edge];
          if (nx < 0 || ny < 0 || nx >= kTownCells || ny >= kTownCells)
            continue;
          const uint8_t adjacent = foliage[TownCellIndex(nx, ny)];
          if (adjacent == kNavigationFoliage_Evergreen ||
              adjacent == kNavigationFoliage_Broadleaf)
            edges |= (uint8_t)(1u << edge);
        }
      const uint8_t kind = foliage[cell] == kNavigationFoliage_Shrub
          ? kSimBackgroundVoxel_Shrub
          : foliage[cell] == kNavigationFoliage_Palm
          ? kSimBackgroundVoxel_Palm
          : foliage[cell] == kNavigationFoliage_Broadleaf
          ? kSimBackgroundVoxel_BroadTree : kSimBackgroundVoxel_Tree;
      if (!Append(out, (SimWorldNavigationTownObject){
            .group = forest ? group : 0,
            .town = town, .kind = kind,
            .flags = count == 1 ? kSimBackgroundVoxel_IsolatedTree : 0,
            .record_slot = kSimBackgroundVoxelNoRecordSlot,
            .cell_x = (uint8_t)x, .cell_y = (uint8_t)y,
            .source_cells_w = 1, .source_cells_h = 1,
            .footprint_cells_w = 1, .footprint_cells_d = 1,
            .tree_edges = edges,
          })) return;
    }
  }
}

void SimWorldNavigationTowns_Capture(
    const uint8_t *wram, SimWorldNavigationTowns *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!wram) return;
  for (uint8_t town = 1; town <= 6; town++) {
    if (!TownHasRetainedData(wram, town)) continue;
    out->enabled_town_mask |= (uint8_t)(1u << (town - 1));
    out->ground.enabled_town_mask = out->enabled_town_mask;
    out->ground.development_tier[town - 1] =
        wram[kDevelopmentTiersWram + (size_t)(town - 1) * 2];
    for (int y = 0; y < kTownCells; y++)
      for (int x = 0; x < kTownCells; x++)
        out->ground.terrain[town - 1][TownCellIndex(x, y)] =
            wram[SimTownLayout_CellMapIndex(town, x, y)];
  }
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(out->enabled_town_mask & (1u << (town - 1)))) continue;
    bool occupied[kTownCellCount] = {false};
    CaptureStructures(wram, town, out, occupied);
    CaptureSanctuary(wram, town, out, occupied);
    CaptureLandmarks(wram, town, out, occupied);
    CaptureBridges(wram, town, out, occupied);
    CaptureFoliage(wram, town, out, occupied);
    if (out->overflow) return;
  }
}

/* Keep source dependencies beside the classifier that consumes them. Neither
 * frame metadata nor the presenter needs to know these WRAM ranges. Including
 * all four structure bytes conservatively invalidates for action-only changes
 * as well; that avoids encoding a second interpretation of record ownership. */
static struct {
  uint8_t cells[kSimTownCount * kSimTownCellMapBytes];
  uint8_t structures[kSimTownCount * kStructureRecordsPerTownBytes];
  uint8_t development[kSimTownCount * 2];
  SimWorldNavigationTowns scene;
  bool valid;
} s_capture_cache;
_Static_assert(sizeof(s_capture_cache) <= 192 * 1024,
    "bounded navigation capture cache");

void SimWorldNavigationTowns_ResetCache(void) {
  s_capture_cache.valid = false;
}

void SimWorldNavigationTowns_CaptureCached(
    const uint8_t *wram, SimWorldNavigationTowns *out) {
  if (!out) return;
  if (!wram) {
    SimWorldNavigationTowns_ResetCache();
    SimWorldNavigationTowns_Capture(NULL, out);
    return;
  }
  if (!s_capture_cache.valid ||
      memcmp(s_capture_cache.cells, wram + kSimTownCellMapsWram,
          sizeof(s_capture_cache.cells)) ||
      memcmp(s_capture_cache.structures, wram + kStructureRecordsWram,
          sizeof(s_capture_cache.structures)) ||
      memcmp(s_capture_cache.development, wram + kDevelopmentTiersWram,
          sizeof(s_capture_cache.development))) {
    SimWorldNavigationTowns_Capture(wram, &s_capture_cache.scene);
    memcpy(s_capture_cache.cells, wram + kSimTownCellMapsWram,
        sizeof(s_capture_cache.cells));
    memcpy(s_capture_cache.structures, wram + kStructureRecordsWram,
        sizeof(s_capture_cache.structures));
    memcpy(s_capture_cache.development, wram + kDevelopmentTiersWram,
        sizeof(s_capture_cache.development));
    s_capture_cache.valid = true;
  }
  *out = s_capture_cache.scene;
}
