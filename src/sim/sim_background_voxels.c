#include "sim_background_voxels.h"

#include "sim_background_mountain_silhouette.h"
#include "sim_background_voxel_landmarks.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kTownCellMapsWram = 0x12000,       /* flat $7F:2000 */
  kTownCellMapBytes = 0x400,
  kTownTilemapWram = 0x10000,        /* flat $7F:0000 */
  kTerrainDefinitionsWram = 0x2100,  /* flat $7E:2100 */
  /* Structure art is a second metatile atlas with the same four-word entry
   * shape, copied into the live tilemap by $03:9C43 exactly as $03:9B5A
   * copies terrain. Structure frames are not in the terrain atlas, which is
   * why DisplayedCellMetatile has to fall back to the recorded cell value. */
  kStructureDefinitionsWram = 0x3100,  /* flat $7E:3100 */
  kTerrainDefinitionBytes = 8,
  kTerrainMetatileCount = 256,
  /* Each of the tilemap's four pages is a 32x32 run of 8x8 entries. */
  kTilemapPageTiles = 32,
  kTilemapPageWords = kTilemapPageTiles * kTilemapPageTiles,
  kTilesPerCellSide = kSimBackgroundCellPixels / 8,
  /* Bit 9 is terrain traversal metadata cleared by $03:9B5A before a
   * definition becomes a live entry, and bit 13 is tile priority, which
   * changes painter order but not which pixels a cell shows. Neither may
   * take part in matching a live cell against its definition. */
  kMetatileCompareMask = 0xDDFF,
  kStructureRecordsWram = 0x16BE7,  /* flat $7F:6BE7 */
  /* Per-record visual step-machine slots, $7F:77E7 + slot*8. Diagnostics only:
   * classification reads the drawn frame, never this. */
  kStepSlotsWram = 0x177E7,
  kStepSlotBytes = 8,
  kStructureRecordsPerTownBytes = 0x200,
  kStructureRecordBytes = 4,
  kStructureRecordCount = 128,
  kStructureActive = 0x80,
  kStructureConstructionVariant = 0x40,
  kStructureClassMask = 0x0F,
  kStructureDevelopmentMask = 0x30,
  kStructureDevelopmentShift = 4,
  kStructureClassHouse = 0,
  kStructureClassWindmill = 3,
  kStructureClassFactory = 4,
  kCellCount = kSimBackgroundTownCells * kSimBackgroundTownCells,
  kCanvasPixelCount = kSimTownCanvasPixels * kSimTownCanvasPixels,
};

/* The 2x2 sanctuary block is a contiguous metatile run in every town. Five
 * towns share the $C2 family; Marahna's $C0 family is the same plot drawn as
 * a tropical temple, so it is a distinct model rather than a missing one. */
typedef struct SanctuarySignature {
  uint8_t top_left;
  uint8_t kind;
} SanctuarySignature;

static const SanctuarySignature kSanctuaries[] = {
  {0xC2, kSimBackgroundVoxel_Cathedral},
  {0xC0, kSimBackgroundVoxel_MarahnaTemple},
};

static struct {
  uint32_t serial;
  uint32_t canvas_serial;
  bool wind_stops_all;
  SimBackgroundVoxelScene scene;
  uint32_t atlas[kCanvasPixelCount];
  uint32_t ground[kCanvasPixelCount];
  /* One-based atlas cells for clean raw terrain-metatile sources. */
  uint16_t mountain_source_cell[256];
} g_background;
/* Scene rebuild scratch. Every classified source rectangle, including static
 * terrain, is marked before a replacement tile is selected, so authentic art
 * is never considered as the town's general ground. */
static uint8_t g_object_mask[kCanvasPixelCount];
/* Reset clears publish state when leaving a town, but a later town must never
 * reuse a serial whose GPU texture may still exist. */
static uint32_t g_next_serial;

static uint32_t NextSerial(void) {
  g_next_serial++;
  if (!g_next_serial) g_next_serial = 1;
  return g_next_serial;
}

static size_t CellIndex(int x, int y) {
  return (size_t)y * kSimBackgroundTownCells + (size_t)x;
}

/* The cell maps use four 16x16 pages rather than row-major 32x32 storage. */
static size_t TownCellMapIndex(uint8_t town, int x, int y) {
  int quadrant = (y >= 16 ? 2 : 0) + (x >= 16 ? 1 : 0);
  return kTownCellMapsWram + (size_t)(town - 1) * kTownCellMapBytes +
      (size_t)quadrant * 0x100 + (size_t)(y & 15) * 16 + (x & 15);
}

static uint8_t CellMapValue(uint8_t town, const uint8_t *wram, int x, int y) {
  return wram[TownCellMapIndex(town, x, y)];
}

static uint16_t Read16(const uint8_t *wram, size_t address) {
  return (uint16_t)(wram[address] | (uint16_t)wram[address + 1] << 8);
}

/* The town tilemap is quadrant-paged, 64x64 8x8 entries. */
static uint16_t TownTilemapWord(const uint8_t *wram, int tile_x, int tile_y) {
  int quadrant = (tile_y >= kTilemapPageTiles ? 2 : 0) +
      (tile_x >= kTilemapPageTiles ? 1 : 0);
  size_t word = (size_t)quadrant * kTilemapPageWords +
      (size_t)(tile_y & (kTilemapPageTiles - 1)) * kTilemapPageTiles +
      (size_t)(tile_x & (kTilemapPageTiles - 1));
  return Read16(wram, kTownTilemapWram + word * 2) & kMetatileCompareMask;
}

static void LiveCellEntries(const uint8_t *wram, int x, int y,
                            uint16_t out[4]) {
  int tile_x = x * kTilesPerCellSide, tile_y = y * kTilesPerCellSide;
  out[0] = TownTilemapWord(wram, tile_x, tile_y);
  out[1] = TownTilemapWord(wram, tile_x + 1, tile_y);
  out[2] = TownTilemapWord(wram, tile_x, tile_y + 1);
  out[3] = TownTilemapWord(wram, tile_x + 1, tile_y + 1);
}

static bool MetatileMatches(const uint8_t *wram, size_t definitions,
                            uint8_t metatile, const uint16_t live[4]) {
  size_t at = definitions + (size_t)metatile * kTerrainDefinitionBytes;
  uint16_t defined = 0;
  for (int entry = 0; entry < 4; entry++) {
    uint16_t word = Read16(wram, at + (size_t)entry * 2) & kMetatileCompareMask;
    if (word != live[entry]) return false;
    defined |= word;
  }
  /* An all-zero definition is an empty atlas slot, not a metatile. Without
   * this a town whose tilemap has not been built yet matches every candidate
   * at once, and the first one wins. */
  return defined != 0;
}

static bool TerrainMetatileMatches(const uint8_t *wram, uint8_t metatile,
                                   const uint16_t live[4]) {
  return MetatileMatches(wram, kTerrainDefinitionsWram, metatile, live);
}

/* One frame of an animated or progressing structure, identified by the
 * structure metatile its top-left cell draws.
 *
 * The record's `$40` flag deliberately takes no part in this. For a windmill
 * record (class 3) that bit is the "the wind has died" story state set by
 * $03:E2BB and cleared by the Wind miracle's action 6 at $03:A1F4 - it selects
 * visual variant 4 over variant 0, both of which draw a FINISHED mill. Reading
 * it as "under construction" put a scaffold over a standing windmill for the
 * whole event, which is the bug this replaces. Class 4 never has the bit set
 * by any ROM path at all: the allocator ($03:9D9F) only ever ORs `$80`.
 *
 * The frames themselves come from the class-6 and class-8 visual step programs
 * ($03:D4D2 construction / $03:D4E2 rebuild families, draw lists decoded by
 * $03:A591). Only the top-left metatile is needed to tell them apart. */
typedef struct StructureFrame {
  uint8_t metatile;
  uint8_t phase;
  bool construction;
} StructureFrame;

/* Windmill (record class 3, visual class 6). Construction plays $04/$06/$14
 * and lands on $16; the built mill then cycles $24 -> $26 -> $16, three blade
 * positions 30 degrees apart in the wheel's 90-degree period. The construction
 * program's last frame draws the same art as cycle position 2, so it is that
 * position and not a scaffold. */
static const StructureFrame kWindmillFrames[] = {
  {0x04, 0, true}, {0x06, 1, true}, {0x14, 2, true},
  {0x24, 0, false}, {0x26, 1, false}, {0x16, 2, false},
};

/* Factory tier (record class 4, visual class 8). Two frames, no cycle. */
static const StructureFrame kFactoryFrames[] = {
  {0x34, 0, true}, {0x36, 0, false},
};

/* Is this town in the "the wind has died" story event?
 *
 * `$03:E2BB` is the only site that parks a windmill, and it stamps `$40` on
 * whichever class-3 records exist at the moment the event fires. A mill the
 * town builds later is never stamped, so the ROM leaves it turning through an
 * event whose whole premise is that nothing turns - visible in
 * runs/20260817-194701 as record 7 parked on the static program while record
 * 20 ran the spin loop. The enhanced view takes the event at its word and
 * holds every mill in the town, which is a deliberate divergence from the flat
 * art for the one mill the ROM missed. The Wind miracle clears the bit on
 * every record it stamped ($03:A1F4), so this releases with the event. */
static bool TownWindStopped(const uint8_t *records) {
  for (int slot = 0; slot < kStructureRecordCount; slot++) {
    uint8_t flags = records[slot * kStructureRecordBytes + 2];
    if ((flags & kStructureActive) &&
        (flags & kStructureClassMask) == kStructureClassWindmill &&
        (flags & kStructureConstructionVariant))
      return true;
  }
  return false;
}

/* Resolves the frame a 2x2 structure plot is displaying. Leaves `object`
 * untouched when nothing matches - a half-drawn or not-yet-reconstructed plot
 * keeps the finished model rather than dropping back to a scaffold, because
 * un-building something the town has already finished is the louder error. */
static void ApplyStructureFrame(const uint8_t *wram,
                                const StructureFrame *frames, size_t count,
                                int cell_x, int cell_y,
                                SimBackgroundVoxelObject *object) {
  uint16_t live[4];
  LiveCellEntries(wram, cell_x, cell_y, live);
  for (size_t at = 0; at < count; at++) {
    if (!MetatileMatches(wram, kStructureDefinitionsWram,
                         frames[at].metatile, live))
      continue;
    object->animation_phase = frames[at].phase;
    if (frames[at].construction)
      object->flags |= kSimBackgroundVoxel_UnderConstruction;
    return;
  }
}

/* The metatile a cell is currently DISPLAYING, which is not always the one its
 * cell map records. Clearing a bush or a wood commits the cleared cell-map
 * value ($08 grass) as soon as the miracle resolves, and only repaints the BG1
 * tilemap when the animation ends; for those frames the semantic map says
 * "grass" while the original art is still on screen. Classifying from the
 * cell map alone dropped the object and let the flat authentic sprite show
 * through mid-strike, which is exactly the pop the enhanced view exists to
 * avoid. Resolving what is drawn keeps the model alive until the art itself
 * changes. */
static uint8_t DisplayedCellMetatile(uint8_t town, const uint8_t *wram,
                                     int x, int y) {
  uint8_t recorded = CellMapValue(town, wram, x, y);
  uint16_t live[4];
  LiveCellEntries(wram, x, y, live);
  if (TerrainMetatileMatches(wram, recorded, live)) return recorded;
  for (int metatile = 0; metatile < kTerrainMetatileCount; metatile++)
    if (TerrainMetatileMatches(wram, (uint8_t)metatile, live))
      return (uint8_t)metatile;
  /* Structure art and expansion marks are not in the terrain atlas at all, so
   * an unmatched cell keeps its recorded identity. */
  return recorded;
}

static bool AppendObject(SimBackgroundVoxelScene *scene,
                         SimBackgroundVoxelObject object) {
  if (scene->object_count >= kSimBackgroundMaxObjects) {
    scene->overflow = true;
    return false;
  }
  scene->objects[scene->object_count++] = object;
  return true;
}

static void MarkOccupied(bool occupied[kCellCount], int x, int y,
                         int width, int height) {
  for (int row = 0; row < height; row++)
    for (int column = 0; column < width; column++) {
      int cell_x = x + column, cell_y = y + row;
      if (cell_x >= 0 && cell_x < kSimBackgroundTownCells &&
          cell_y >= 0 && cell_y < kSimBackgroundTownCells)
        occupied[CellIndex(cell_x, cell_y)] = true;
    }
}

/* Used only to choose the ground eraser tile. Object classification is driven
 * by terrain metatile identity, because rendered chroma cannot tell Bloodpool's
 * mottled marsh from a canopy, nor Northwall's grey-white firs from snow. */
static bool StrongTreePixel(uint32_t argb) {
  unsigned red = (argb >> 16) & 0xFF;
  unsigned green = (argb >> 8) & 0xFF;
  unsigned blue = argb & 0xFF;
  return green >= 8 && green * 10 > red * 13 && green * 10 > blue * 12;
}

typedef enum FoliageClass {
  kFoliage_None,
  /* Clearable brush: one cell of decoration the player can remove, never part
   * of a forest component. The round bush and the palm are the same role in
   * different regions, so they are classified together and differ only in
   * which model they carry. */
  kFoliage_Bush,
  kFoliage_Palm,
  /* Permanent forest. Two families, both grouped by adjacency. */
  kFoliage_Evergreen,
  kFoliage_Broadleaf,
  /* Mostly ground with a canopy fringe. These are the outer cells of a forest
   * block and only become foliage when they touch one, and they take the
   * family of whichever block recruited them. */
  kFoliage_CanopyEdge,
} FoliageClass;

/* The terrain metatile atlas is an eight-wide grid whose entries mean the same
 * thing in all six towns; only the palette changes. Columns 2-4 of rows 0-3
 * (plus the $23/$24 caps) are the pointed evergreen family and columns 5-7 of
 * the same rows (plus $26/$27) are the broad round canopies - Marahna's
 * mangroves and Kasandora's oasis stands. Everything else here, including
 * Bloodpool's $3D-$4F marsh, is terrain. */
static FoliageClass FoliageClassForTile(uint8_t tile) {
  switch (tile) {
    case 0x01:
      return kFoliage_Bush;
    case 0x09:
      return kFoliage_Palm;
    case 0x02: case 0x03: case 0x04:
    case 0x0A: case 0x0B: case 0x0C:
    case 0x12: case 0x13: case 0x14:
    case 0x1A: case 0x1B: case 0x1C:
    case 0x23: case 0x24:
      return kFoliage_Evergreen;
    case 0x05: case 0x06: case 0x07:
    case 0x0D: case 0x0E: case 0x0F:
    case 0x15: case 0x16: case 0x17:
    case 0x1D: case 0x1E: case 0x1F:
    case 0x26: case 0x27:
      return kFoliage_Broadleaf;
    case 0x22:
      return kFoliage_CanopyEdge;
    default:
      return kFoliage_None;
  }
}

static int CellTreePixelCount(const uint32_t *pixels, int cell_x, int cell_y) {
  int count = 0;
  int x0 = cell_x * kSimBackgroundCellPixels;
  int y0 = cell_y * kSimBackgroundCellPixels;
  for (int y = 0; y < kSimBackgroundCellPixels; y++)
    for (int x = 0; x < kSimBackgroundCellPixels; x++)
      if (StrongTreePixel(
              pixels[(size_t)(y0 + y) * kSimTownCanvasPixels +
                     (size_t)(x0 + x)]))
        count++;
  return count;
}

void SimBackgroundVoxels_Classify(uint8_t town, const uint8_t *wram,
                                  bool wind_stops_all,
                                  SimBackgroundVoxelScene *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!town || town > 6 || !wram) return;
  out->town = town;

  bool occupied[kCellCount] = {false};
  SimBackgroundMountains_Classify(town, wram, &out->mountains);
  SimBackgroundMountains_BuildNorthCaps(
      &out->mountains, &out->mountain_caps);
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++)
      if (SimBackgroundMountains_CellOccupied(&out->mountains, x, y))
        occupied[CellIndex(x, y)] = true;
  const uint8_t *records = wram + kStructureRecordsWram +
      (size_t)(town - 1) * kStructureRecordsPerTownBytes;
  bool wind_stopped = wind_stops_all && TownWindStopped(records);
  for (int slot = 0; slot < kStructureRecordCount; slot++) {
    const uint8_t *record = records + slot * kStructureRecordBytes;
    uint8_t flags = record[2];
    if (!(flags & kStructureActive)) continue;
    int x = record[0], y = record[1];
    int structure_class = flags & kStructureClassMask;
    int cells = structure_class == kStructureClassHouse ? 1 : 2;
    /* Every non-house structure record marks a 2x2 source area. Mark all of
     * them occupied even when this phase intentionally ignores that class, so
     * a green field can never be reclassified as forest. */
    MarkOccupied(occupied, x, y, cells, cells);
    if (x < 0 || y < 0 || x + cells > kSimBackgroundTownCells ||
        y + cells > kSimBackgroundTownCells)
      continue;

    SimBackgroundVoxelObject object = {
      .town = town,
      .development_level = (uint8_t)(
          (flags & kStructureDevelopmentMask) >>
          kStructureDevelopmentShift),
      .cell_x = (uint8_t)x,
      .cell_y = (uint8_t)y,
      .record_slot = (uint8_t)slot,
    };
    if (structure_class == kStructureClassHouse) {
      /* In completed towns the authentic BG draws both $A0 and $E0 records as
       * finished houses: $E0 selects the side-facing art, not a timber frame.
       * Preserve that visual distinction without mistaking half the town for
       * active construction. */
      if (flags & kStructureConstructionVariant)
        object.flags |= kSimBackgroundVoxel_AlternateFacing;
      object.kind = kSimBackgroundVoxel_House;
      object.source_cells_w = object.source_cells_h = 1;
      object.footprint_cells_w = object.footprint_cells_d = 1;
    } else if (structure_class == kStructureClassWindmill) {
      object.kind = kSimBackgroundVoxel_Windmill;
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = 2;
      object.footprint_cells_d = 1;
      ApplyStructureFrame(wram, kWindmillFrames,
                          sizeof(kWindmillFrames) / sizeof(kWindmillFrames[0]),
                          x, y, &object);
      /* Parked on the same blade position the ROM's own stopped program draws,
       * so a stamped mill and an unstamped one hold together. Construction is
       * not wind-driven and keeps its own progress. */
      if (wind_stopped &&
          !(object.flags & kSimBackgroundVoxel_UnderConstruction))
        object.animation_phase = 0;
    } else if (structure_class == kStructureClassFactory) {
      object.kind = kSimBackgroundVoxel_Factory;
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = object.footprint_cells_d = 2;
      ApplyStructureFrame(wram, kFactoryFrames,
                          sizeof(kFactoryFrames) / sizeof(kFactoryFrames[0]),
                          x, y, &object);
    } else {
      continue;
    }
    if (!AppendObject(out, object)) return;
  }

  /* One stable four-cell signature identifies the town sanctuary. Its top row
   * is elevation art, not a second ground-depth row. */
  bool sanctuary_found = false;
  for (int y = 0; y < kSimBackgroundTownCells - 1 && !sanctuary_found; y++)
    for (int x = 0; x < kSimBackgroundTownCells - 1 && !sanctuary_found; x++)
      for (size_t at = 0;
           at < sizeof(kSanctuaries) / sizeof(kSanctuaries[0]); at++) {
        uint8_t base = kSanctuaries[at].top_left;
        if (CellMapValue(town, wram, x, y) != base ||
            CellMapValue(town, wram, x + 1, y) != (uint8_t)(base + 1) ||
            CellMapValue(town, wram, x, y + 1) != (uint8_t)(base + 8) ||
            CellMapValue(town, wram, x + 1, y + 1) != (uint8_t)(base + 9))
          continue;
        AppendObject(out, (SimBackgroundVoxelObject){
          .town = town,
          .kind = kSanctuaries[at].kind,
          .cell_x = (uint8_t)x,
          .cell_y = (uint8_t)y,
          .source_cells_w = 2,
          .source_cells_h = 2,
          .footprint_cells_w = 2,
          /* All four cells are protected land: the game never places another
           * structure behind the sanctuary. The upper source row is therefore
           * both real depth and the perspective-compressed second tier. */
          .footprint_cells_d = 2,
          .record_slot = 0xFF,
        });
        MarkOccupied(occupied, x, y, 2, 2);
        sanctuary_found = true;
        break;
      }

  /* Story landmarks own reserved cell-map plots rather than structure records.
   * Install and reserve their complete source art before forest extraction so
   * a large tree cannot be fragmented into ordinary one-cell evergreens. */
  SimBackgroundVoxelObject landmarks[3];
  size_t landmark_count = SimBackgroundVoxelLandmarks_Classify(
      town, wram, landmarks,
      sizeof(landmarks) / sizeof(landmarks[0]));
  for (size_t at = 0; at < landmark_count; at++) {
    SimBackgroundVoxelObject *landmark = &landmarks[at];
    MarkOccupied(occupied, landmark->cell_x, landmark->cell_y,
                 landmark->source_cells_w, landmark->source_cells_h);
    if (!AppendObject(out, *landmark)) return;
  }

  bool tree[kCellCount] = {false};
  bool weak_tree[kCellCount] = {false};
  /* Which permanent family owns each canopy cell. A fringe cell has none until
   * a block recruits it. */
  uint8_t canopy_kind[kCellCount] = {0};
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++) {
      size_t cell = CellIndex(x, y);
      if (occupied[cell]) continue;
      FoliageClass foliage =
          FoliageClassForTile(DisplayedCellMetatile(town, wram, x, y));
      if (foliage == kFoliage_Bush || foliage == kFoliage_Palm) {
        /* Clearable brush entries are their own single-cell objects. Keeping
         * them out of the forest flood fill stops one bush beside a wood from
         * being absorbed into it and losing its own model. */
        if (!AppendObject(out, (SimBackgroundVoxelObject){
              .town = town,
              .kind = foliage == kFoliage_Bush
                  ? kSimBackgroundVoxel_Shrub : kSimBackgroundVoxel_Palm,
              .flags = kSimBackgroundVoxel_IsolatedTree,
              .cell_x = (uint8_t)x,
              .cell_y = (uint8_t)y,
              .source_cells_w = 1,
              .source_cells_h = 1,
              .footprint_cells_w = 1,
              .footprint_cells_d = 1,
              .record_slot = 0xFF,
            }))
          return;
        out->brush_cell_count++;
        continue;
      }
      if (foliage == kFoliage_Evergreen || foliage == kFoliage_Broadleaf) {
        tree[cell] = weak_tree[cell] = true;
        canopy_kind[cell] = foliage == kFoliage_Evergreen
            ? kSimBackgroundVoxel_Tree : kSimBackgroundVoxel_BroadTree;
      } else if (foliage == kFoliage_CanopyEdge) {
        weak_tree[cell] = true;
      }
    }

  /* Grow only from complete canopy cells. This captures the fringe cells that
   * are mostly ground without letting an isolated fringe tile - or a marsh or
   * snow patch that merely reads green - become a tree object. A recruited
   * fringe cell inherits the family that reached it, so a wood never changes
   * species at its own edge. */
  uint16_t weak_queue[kCellCount];
  int weak_read = 0, weak_write = 0;
  for (int cell = 0; cell < kCellCount; cell++)
    if (tree[cell]) weak_queue[weak_write++] = (uint16_t)cell;
  while (weak_read < weak_write) {
    int cell = weak_queue[weak_read++];
    int x = cell % kSimBackgroundTownCells;
    int y = cell / kSimBackgroundTownCells;
    static const int dx[] = {0, 1, 0, -1};
    static const int dy[] = {-1, 0, 1, 0};
    for (int edge = 0; edge < 4; edge++) {
      int nx = x + dx[edge], ny = y + dy[edge];
      if (nx < 0 || nx >= kSimBackgroundTownCells ||
          ny < 0 || ny >= kSimBackgroundTownCells)
        continue;
      size_t next = CellIndex(nx, ny);
      if (!tree[next] && weak_tree[next]) {
        tree[next] = true;
        canopy_kind[next] = canopy_kind[cell];
        weak_queue[weak_write++] = (uint16_t)next;
      }
    }
  }

  bool visited[kCellCount] = {false};
  uint16_t queue[kCellCount];
  for (int start_y = 0; start_y < kSimBackgroundTownCells; start_y++)
    for (int start_x = 0; start_x < kSimBackgroundTownCells; start_x++) {
      size_t start = CellIndex(start_x, start_y);
      if (!tree[start] || visited[start]) continue;
      uint16_t group = ++out->tree_group_count;
      int read = 0, write = 0;
      queue[write++] = (uint16_t)start;
      visited[start] = true;
      while (read < write) {
        int cell = queue[read++];
        int x = cell % kSimBackgroundTownCells;
        int y = cell / kSimBackgroundTownCells;
        static const int dx[] = {0, 1, 0, -1};
        static const int dy[] = {-1, 0, 1, 0};
        for (int edge = 0; edge < 4; edge++) {
          int nx = x + dx[edge], ny = y + dy[edge];
          if (nx < 0 || nx >= kSimBackgroundTownCells ||
              ny < 0 || ny >= kSimBackgroundTownCells)
            continue;
          size_t next = CellIndex(nx, ny);
          if (tree[next] && !visited[next]) {
            visited[next] = true;
            queue[write++] = (uint16_t)next;
          }
        }
      }
      for (int item = 0; item < write; item++) {
        int cell = queue[item];
        int x = cell % kSimBackgroundTownCells;
        int y = cell / kSimBackgroundTownCells;
        uint8_t edges = 0;
        if (y > 0 && tree[CellIndex(x, y - 1)])
          edges |= kSimBackgroundTreeEdge_North;
        if (x + 1 < kSimBackgroundTownCells &&
            tree[CellIndex(x + 1, y)])
          edges |= kSimBackgroundTreeEdge_East;
        if (y + 1 < kSimBackgroundTownCells &&
            tree[CellIndex(x, y + 1)])
          edges |= kSimBackgroundTreeEdge_South;
        if (x > 0 && tree[CellIndex(x - 1, y)])
          edges |= kSimBackgroundTreeEdge_West;
        if (!AppendObject(out, (SimBackgroundVoxelObject){
              .group = group,
              .town = town,
              /* The permanent family is the cell's own, not the town's.
               * Marahna is mostly broad mangrove but its palms are clearable
               * brush, and Kasandora carries both families at once. */
              .kind = canopy_kind[cell],
              .flags = write == 1 ? kSimBackgroundVoxel_IsolatedTree : 0,
              .cell_x = (uint8_t)x,
              .cell_y = (uint8_t)y,
              .source_cells_w = 1,
              .source_cells_h = 1,
              .footprint_cells_w = 1,
              .footprint_cells_d = 1,
              .tree_edges = edges,
              .record_slot = 0xFF,
            }))
          return;
        out->tree_cell_count++;
      }
    }
}

static bool CellIsMasked(int cell_x, int cell_y) {
  int x0 = cell_x * kSimBackgroundCellPixels;
  int y0 = cell_y * kSimBackgroundCellPixels;
  for (int y = 0; y < kSimBackgroundCellPixels; y++)
    for (int x = 0; x < kSimBackgroundCellPixels; x++)
      if (g_object_mask[(size_t)(y0 + y) * kSimTownCanvasPixels +
                        (size_t)(x0 + x)])
        return true;
  return false;
}

static uint32_t GeneralGroundColour(const uint32_t *pixels) {
  enum { kMaxColours = 1024, kColourTableSize = 2048 };
  uint32_t colours[kColourTableSize] = {0};
  uint32_t counts[kColourTableSize] = {0};
  uint16_t order[kColourTableSize] = {0};
  bool used[kColourTableSize] = {false};
  _Static_assert(
      (kColourTableSize & (kColourTableSize - 1)) == 0,
      "ground-colour hash table size must be a power of two");
  int colour_count = 0;
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++) {
      size_t at = (size_t)y * kSimTownCanvasPixels + (size_t)x;
      if (!g_object_mask[at]) continue;
      static const int dx[] = {0, 1, 0, -1};
      static const int dy[] = {-1, 0, 1, 0};
      for (int edge = 0; edge < 4; edge++) {
        int nx = x + dx[edge], ny = y + dy[edge];
        if (nx < 0 || nx >= kSimTownCanvasPixels ||
            ny < 0 || ny >= kSimTownCanvasPixels)
          continue;
        size_t next = (size_t)ny * kSimTownCanvasPixels + (size_t)nx;
        uint32_t colour = pixels[next];
        if (g_object_mask[next] || StrongTreePixel(colour)) continue;
        uint32_t mixed = colour * 0x9E3779B1u;
        int index = (int)(mixed & (kColourTableSize - 1));
        while (used[index] && colours[index] != colour)
          index = (index + 1) & (kColourTableSize - 1);
        if (!used[index]) {
          if (colour_count >= kMaxColours) continue;
          used[index] = true;
          colours[index] = colour;
          counts[index] = 0;
          order[index] = (uint16_t)colour_count;
          colour_count++;
        }
        counts[index]++;
      }
  }
  int best = -1;
  for (int i = 0; i < kColourTableSize; i++)
    if (used[i] &&
        (best < 0 || counts[i] > counts[best] ||
         (counts[i] == counts[best] && order[i] < order[best])))
      best = i;
  return best >= 0 ? colours[best] : pixels[0];
}

static bool SnowLikePixel(uint32_t colour) {
  unsigned red = (colour >> 16) & 0xFF;
  unsigned green = (colour >> 8) & 0xFF;
  unsigned blue = colour & 0xFF;
  unsigned minimum = red < green ? red : green;
  if (blue < minimum) minimum = blue;
  unsigned maximum = red > green ? red : green;
  if (blue > maximum) maximum = blue;
  return minimum >= 144 && maximum - minimum <= 80;
}

static bool FindSnowGroundCell(const uint32_t *pixels,
                               int *ground_cell_x, int *ground_cell_y) {
  int best_score = 0, best_x = 0, best_y = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      if (CellIsMasked(cell_x, cell_y)) continue;
      int score = 0;
      int x0 = cell_x * kSimBackgroundCellPixels;
      int y0 = cell_y * kSimBackgroundCellPixels;
      for (int y = 0; y < kSimBackgroundCellPixels; y++)
        for (int x = 0; x < kSimBackgroundCellPixels; x++)
          if (SnowLikePixel(
                  pixels[(size_t)(y0 + y) * kSimTownCanvasPixels +
                         (size_t)(x0 + x)]))
            score++;
      if (score > best_score) {
        best_score = score;
        best_x = cell_x;
        best_y = cell_y;
      }
    }
  if (!best_score) return false;
  *ground_cell_x = best_x;
  *ground_cell_y = best_y;
  return true;
}

static bool FindGeneralGroundCell(const uint32_t *pixels, uint8_t town,
                                  int *ground_cell_x, int *ground_cell_y) {
  /* Northwall contains deliberately green landmark plots. Their long border
   * can dominate the object-neighbour vote even though the general terrain is
   * snow, producing a conspicuous green rectangle under a replaced landmark.
   * Prefer a complete unmasked snow cell and retain the ordinary colour vote
   * as a fallback for fades or unusual captures with no detectable snow. */
  if (town == 6 && FindSnowGroundCell(
          pixels, ground_cell_x, ground_cell_y))
    return true;
  uint32_t ground_colour = GeneralGroundColour(pixels);
  int best_score = -1, best_x = 0, best_y = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      if (CellIsMasked(cell_x, cell_y)) continue;
      int green = CellTreePixelCount(pixels, cell_x, cell_y);
      if (green * 10 >= kSimBackgroundCellPixels *
          kSimBackgroundCellPixels)
        continue;
      int score = 0;
      int x0 = cell_x * kSimBackgroundCellPixels;
      int y0 = cell_y * kSimBackgroundCellPixels;
      for (int y = 0; y < kSimBackgroundCellPixels; y++)
        for (int x = 0; x < kSimBackgroundCellPixels; x++)
          if (pixels[(size_t)(y0 + y) * kSimTownCanvasPixels +
                     (size_t)(x0 + x)] ==
              ground_colour)
            score++;
      if (score > best_score) {
        best_score = score;
        best_x = cell_x;
        best_y = cell_y;
      }
    }
  if (best_score < 0) return false;
  *ground_cell_x = best_x;
  *ground_cell_y = best_y;
  return true;
}

static bool FindMountainScratchCell(
    const bool used[kCellCount], int *cell_x, int *cell_y) {
  for (int y = kSimBackgroundTownCells - 1; y >= 0; y--)
    for (int x = kSimBackgroundTownCells - 1; x >= 0; x--) {
      size_t cell = CellIndex(x, y);
      if (used[cell] || CellIsMasked(x, y)) continue;
      *cell_x = x;
      *cell_y = y;
      return true;
    }
  return false;
}

static void BuildCleanMountainSources(const uint8_t *wram) {
  /* These are the clean semantic parts used by every complete mountain stamp.
   * Rendering them from the town's raw metatile definitions preserves native
   * region palettes (including Northwall snow) without sampling fused range
   * cells such as Kasandora's $7D overlap. */
  static const uint8_t kSourceTiles[] = {
    0x70, 0x71, 0x81, 0x82, 0x88, 0x89, 0x8A, 0x8B,
    0x8C, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D,
    0x9E, 0x9F,
  };
  bool used[kCellCount] = {false};
  uint32_t metatile_pixels[kSimBackgroundCellPixels *
                           kSimBackgroundCellPixels];
  for (size_t tile_at = 0;
       tile_at < sizeof(kSourceTiles) / sizeof(kSourceTiles[0]); tile_at++) {
    uint8_t tile = kSourceTiles[tile_at];
    bool test_opaque;
    if (!SimBackgroundMountainSilhouette_Lookup(
            tile, 0, 0, &test_opaque) ||
        !SimTownCanvas_RenderTerrainMetatile(
            wram, tile, metatile_pixels))
      continue;
    int cell_x, cell_y;
    if (!FindMountainScratchCell(used, &cell_x, &cell_y)) return;
    used[CellIndex(cell_x, cell_y)] = true;
    int x0 = cell_x * kSimBackgroundCellPixels;
    int y0 = cell_y * kSimBackgroundCellPixels;
    for (int y = 0; y < kSimBackgroundCellPixels; y++)
      for (int x = 0; x < kSimBackgroundCellPixels; x++) {
        bool opaque = false;
        SimBackgroundMountainSilhouette_Lookup(tile, x, y, &opaque);
        size_t destination =
            (size_t)(y0 + y) * kSimTownCanvasPixels + (size_t)(x0 + x);
        uint32_t source = metatile_pixels[
            y * kSimBackgroundCellPixels + x];
        g_background.atlas[destination] =
            opaque ? source | 0xFF000000u : 0;
      }
    g_background.mountain_source_cell[tile] =
        (uint16_t)(CellIndex(cell_x, cell_y) + 1);
  }
}

static void ExtractEnhancedReplacements(
    const uint8_t *wram, const uint16_t *vram,
    const uint32_t *pixels, const SimBackgroundVoxelScene *scene) {
  memset(g_object_mask, 0, sizeof(g_object_mask));
  /* Mountain cells keep the current town's authored colours but take their
   * alpha from a palette-independent semantic silhouette. This preserves
   * Northwall's white snow faces without lifting the opaque snow/grass pixels
   * baked into some metatiles. Unknown semantic tiles fall back to the SNES
   * source palette index, which is still safer than rendered RGB matching. */
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      if (!SimBackgroundMountains_CellOccupied(
              &scene->mountains, cell_x, cell_y))
        continue;
      uint8_t tile = scene->mountains.tile[
          CellIndex(cell_x, cell_y)];
      int x0 = cell_x * kSimBackgroundCellPixels;
      int y0 = cell_y * kSimBackgroundCellPixels;
      for (int y = 0; y < kSimBackgroundCellPixels; y++)
        for (int x = 0; x < kSimBackgroundCellPixels; x++) {
          size_t at = (size_t)(y0 + y) * kSimTownCanvasPixels +
              (size_t)(x0 + x);
          bool opaque;
          if (!SimBackgroundMountainSilhouette_Lookup(
                  tile, x, y, &opaque)) {
            opaque = SimTownCanvas_SourcePixelOpaque(
                wram, vram, x0 + x, y0 + y);
          }
          g_object_mask[at] = 1;
          g_background.atlas[at] = opaque
              ? pixels[at] | 0xFF000000u
              : 0;
        }
    }
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    int x0 = object->cell_x * kSimBackgroundCellPixels;
    int y0 = object->cell_y * kSimBackgroundCellPixels;
    int width = object->source_cells_w * kSimBackgroundCellPixels;
    int height = object->source_cells_h * kSimBackgroundCellPixels;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        size_t at = (size_t)(y0 + y) * kSimTownCanvasPixels +
            (size_t)(x0 + x);
        g_object_mask[at] = 1;
        /* Retained for diagnostic/catalog consumers. The enhanced renderer
         * uses its authored model and never samples this authentic cutout. */
        g_background.atlas[at] = pixels[at] | 0xFF000000u;
      }
  }

  BuildCleanMountainSources(wram);

  int ground_cell_x, ground_cell_y;
  if (!FindGeneralGroundCell(
          pixels, scene->town, &ground_cell_x, &ground_cell_y))
    return;
  int ground_x0 = ground_cell_x * kSimBackgroundCellPixels;
  int ground_y0 = ground_cell_y * kSimBackgroundCellPixels;
  /* The same complete biome tile erases every source cell. Grass towns keep
   * their grass texture, Northwall keeps snow, and no nearest-pixel flood can
   * create streaks around a large forest, cathedral, or lifted mountain. */
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++) {
      size_t at = (size_t)y * kSimTownCanvasPixels + (size_t)x;
      if (!g_object_mask[at]) continue;
      int source_x = ground_x0 + x % kSimBackgroundCellPixels;
      int source_y = ground_y0 + y % kSimBackgroundCellPixels;
      uint32_t replacement =
          pixels[(size_t)source_y * kSimTownCanvasPixels +
                 (size_t)source_x];
      g_background.ground[at] = replacement;
    }
}

/* AR_WINDMILL_DEBUG=1: one line per windmill whenever any of their state
 * changes. Prints the record, its visual step slot and the phase we resolved,
 * which is everything needed to tell "the ROM never restarted this mill" from
 * "we failed to follow it". Transition-only, so a still town is silent. */
static void LogWindmills(uint8_t town, const uint8_t *wram,
                         const SimBackgroundVoxelScene *scene) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_WINDMILL_DEBUG");
    enabled = (value && *value && *value != '0') ? 1 : 0;
  }
  if (!enabled) return;
  const uint8_t *records = wram + kStructureRecordsWram +
      (size_t)(town - 1) * kStructureRecordsPerTownBytes;
  static char previous[512];
  char line[512];
  size_t at = 0;
  for (uint16_t i = 0; i < scene->object_count && at + 96 < sizeof(line); i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (object->kind != kSimBackgroundVoxel_Windmill) continue;
    const uint8_t *record = records + object->record_slot * kStructureRecordBytes;
    const uint8_t *step = wram + kStepSlotsWram +
        (size_t)object->record_slot * kStepSlotBytes;
    at += (size_t)snprintf(line + at, sizeof(line) - at,
        "slot=%u flags=$%02X act=$%02X step=%u/%u cursor=$%04X entry=$%04X "
        "phase=%u constr=%u | ",
        object->record_slot, record[2], record[3], step[0], step[1],
        (unsigned)(step[2] | step[3] << 8), (unsigned)(step[6] | step[7] << 8),
        object->animation_phase,
        (object->flags & kSimBackgroundVoxel_UnderConstruction) ? 1u : 0u);
  }
  if (!at || !strcmp(line, previous)) return;
  memcpy(previous, line, at + 1);
  fprintf(stderr, "[windmill] town=%u %s\n", town, line);
}

void SimBackgroundVoxels_Reset(void) { memset(&g_background, 0, sizeof(g_background)); }

void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               const uint16_t *vram,
                               uint32_t canvas_serial,
                               bool wind_stops_all) {
  if (!town || !wram || !canvas_pixels || !vram || !canvas_serial) return;
  /* The policy takes part in the gate: toggled in a still town, nothing else
   * would ask for a rebuild and the mills would keep their old behaviour until
   * something happened to move the canvas. */
  if (g_background.scene.town == town &&
      g_background.canvas_serial == canvas_serial &&
      g_background.wind_stops_all == wind_stops_all)
    return;
  uint32_t next_serial = NextSerial();
  memset(g_background.atlas, 0, sizeof(g_background.atlas));
  memset(g_background.mountain_source_cell, 0,
         sizeof(g_background.mountain_source_cell));
  memcpy(g_background.ground, canvas_pixels, sizeof(g_background.ground));
  g_background.wind_stops_all = wind_stops_all;
  SimBackgroundVoxels_Classify(town, wram, wind_stops_all,
                               &g_background.scene);
  ExtractEnhancedReplacements(wram, vram, canvas_pixels, &g_background.scene);
  LogWindmills(town, wram, &g_background.scene);
  g_background.canvas_serial = canvas_serial;
  g_background.serial = next_serial;
}

uint32_t SimBackgroundVoxels_Serial(void) { return g_background.serial; }
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void) {
  return &g_background.scene;
}
const uint32_t *SimBackgroundVoxels_AtlasPixels(void) {
  return g_background.atlas;
}
const uint32_t *SimBackgroundVoxels_GroundPixels(void) {
  return g_background.ground;
}

bool SimBackgroundVoxels_MountainTileSource(
    uint8_t tile, int *cell_x, int *cell_y) {
  if (!cell_x || !cell_y || !g_background.mountain_source_cell[tile])
    return false;
  int cell = g_background.mountain_source_cell[tile] - 1;
  *cell_x = cell % kSimBackgroundTownCells;
  *cell_y = cell / kSimBackgroundTownCells;
  return true;
}
