#include "sim_background_voxels.h"

#include "sim_background_mountain_silhouette.h"
#include "sim_background_voxel_landmarks.h"
#include "sim_background_mountain_relief.h"
#include "sim_background_voxel_region.h"
#include "sim_town_terrain.h"

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
  kMetatileDefinitionBytes =
      kTerrainMetatileCount * kTerrainDefinitionBytes,
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
  /* Classification consumes position and flags only. Record +3 is the live
   * action byte and can tick without changing the presented scene. */
  kStructureSceneBytesPerRecord = 3,
  kStructureSceneInputBytes =
      kStructureRecordCount * kStructureSceneBytesPerRecord,
  kStructureActive = 0x80,
  kStructureConstructionVariant = 0x40,
  kStructureClassMask = 0x0F,
  kStructureDevelopmentMask = 0x30,
  kStructureDevelopmentShift = 4,
  kStructureClassHouse = 0,
  kStructureClassWindmill = 3,
  kStructureClassFactory = 4,
  kBridgeTileNorthSouth = 0xE1,
  kBridgeTileEastWest = 0xE2,
  /* Stock terrain beneath every audited bridge site. The live structure stamp
   * replaces these with $E2/$E1 respectively; enhanced presentation restores
   * the original river metatile before placing the voxel bridge. */
  kBridgeRiverEastWest = 0x3A,
  kBridgeRiverNorthSouth = 0x41,
  kCellCount = kSimBackgroundTownCells * kSimBackgroundTownCells,
  kCanvasPixelCount = kSimTownCanvasPixels * kSimTownCanvasPixels,
};

typedef enum EnhancedReplacementKind {
  kEnhancedReplacement_None,
  kEnhancedReplacement_Ground,
  kEnhancedReplacement_BridgeEastWest,
  kEnhancedReplacement_BridgeNorthSouth,
} EnhancedReplacementKind;

enum {
  kAtlasAlpha_Transparent = 0,
  /* Semantic silhouette data does not cover every possible ROM metatile.
   * Resolve those pixels from the live character source during refresh so a
   * CHR animation remains pixel-only rather than invalidating the mask plan. */
  kAtlasAlpha_Source = 1,
  kAtlasAlpha_Opaque = 0xFF,
};

static EnhancedReplacementKind BridgeReplacementKind(
    SimBackgroundBridgeAxis axis) {
  switch (axis) {
    case kSimBackgroundBridgeAxis_EastWest:
      return kEnhancedReplacement_BridgeEastWest;
    case kSimBackgroundBridgeAxis_NorthSouth:
      return kEnhancedReplacement_BridgeNorthSouth;
    case kSimBackgroundBridgeAxis_None:
    case kSimBackgroundBridgeAxis_Count:
      return kEnhancedReplacement_Ground;
  }
  return kEnhancedReplacement_Ground;
}

static SimBackgroundBridgeAxis ReplacementBridgeAxis(
    EnhancedReplacementKind kind) {
  switch (kind) {
    case kEnhancedReplacement_BridgeEastWest:
      return kSimBackgroundBridgeAxis_EastWest;
    case kEnhancedReplacement_BridgeNorthSouth:
      return kSimBackgroundBridgeAxis_NorthSouth;
    case kEnhancedReplacement_None:
    case kEnhancedReplacement_Ground:
      return kSimBackgroundBridgeAxis_None;
  }
  return kSimBackgroundBridgeAxis_None;
}

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
  uint32_t scene_serial;
  uint32_t ground_serial;
  uint32_t atlas_serial;
  uint32_t canvas_serial;
  uint32_t canvas_layout_serial;
  bool wind_stops_all;
  bool have_scene_inputs;
  SimBackgroundVoxelScene scene;
  uint32_t atlas[kCanvasPixelCount];
  uint32_t ground[kCanvasPixelCount];
  /* One-based atlas cells for clean raw terrain-metatile sources. */
  uint16_t mountain_source_cell[256];
  /* Bottom cell row of the mountain mass each cell belongs to, one-based so
   * zero means "not mountain". This is the `baseline` the relief shear is
   * stated against, so an actor standing on a mountain can be placed on the
   * same surface the geometry draws. */
  uint8_t mountain_baseline_row[kCellCount];
  /* Authored structural height indexed by source cell. Runtime sprite
   * grounding samples this several times per actor, so resolve immutable
   * object footprints once when the voxel scene is rebuilt. */
  float structure_height[kCellCount];
  uint8_t cell_map[kTownCellMapBytes];
  uint8_t structure_records[kStructureSceneInputBytes];
  uint8_t terrain_definitions[kMetatileDefinitionBytes];
  uint8_t structure_definitions[kMetatileDefinitionBytes];
  bool have_general_ground;
  uint8_t general_ground_cell_x, general_ground_cell_y;
  /* Half-open dirty span for every output pixel row. */
  int ground_dirty_x0[kSimTownCanvasPixels];
  int ground_dirty_x1[kSimTownCanvasPixels];
} g_background;
/* Scene rebuild scratch. Every classified source rectangle, including static
 * terrain, is marked before a replacement tile is selected, so authentic art
 * is never considered as the town's general ground. */
static uint8_t g_object_mask[kCanvasPixelCount];
/* Precomputed alpha ownership for the cutout atlas. Mountain silhouettes are
 * semantic; authored models retain their complete authentic source blocks. */
static uint8_t g_atlas_alpha[kCanvasPixelCount];
/* Reset clears publish state when leaving a town, but a later town must never
 * reuse a serial whose GPU texture may still exist. */
static uint32_t g_next_serial;
static SimBackgroundVoxelBuildStats g_build_stats;

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

static SimBackgroundBridgeAxis BridgeAxisForTile(uint8_t tile) {
  switch (tile) {
    case kBridgeTileEastWest:
      return kSimBackgroundBridgeAxis_EastWest;
    case kBridgeTileNorthSouth:
      return kSimBackgroundBridgeAxis_NorthSouth;
    default:
      return kSimBackgroundBridgeAxis_None;
  }
}

static bool IsBridgeTile(uint8_t tile) {
  return BridgeAxisForTile(tile) != kSimBackgroundBridgeAxis_None;
}

/* Same semantic water family used by the audited height rules. A live bridge
 * replaces one of these cells, so bank discovery starts at the bridge marker
 * and walks through water until it reaches actual solid ground. */
static bool IsWateryTerrain(uint8_t tile) {
  switch (tile) {
    case 0x10: case 0x11: case 0x18: case 0x19:
    case 0x20: case 0x21: case 0x25:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
    case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A:
    case 0x3B: case 0x3C: case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44:
    case 0xB0: case 0xB1: case 0xB2: case 0xB8: case 0xB9: case 0xBA:
    case 0xF7: case 0xFE:
      return true;
    default:
      return false;
  }
}

static bool FindBridgeBank(
    uint8_t town, const uint8_t *wram, int start_x, int start_y,
    int dx, int dy, int *bank_x, int *bank_y) {
  int x = start_x, y = start_y;
  for (;;) {
    x += dx;
    y += dy;
    if (x < 0 || x >= kSimBackgroundTownCells ||
        y < 0 || y >= kSimBackgroundTownCells)
      return false;
    uint8_t tile = CellMapValue(town, wram, x, y);
    if (IsBridgeTile(tile) || IsWateryTerrain(tile)) continue;
    *bank_x = x;
    *bank_y = y;
    return true;
  }
}

/* Fail-closed fallback when the raw terrain metatile renderer has no cached
 * town source (principally isolated unit tests). Production restores the exact
 * original $3A/$41 river tile below; this still avoids ever sampling the
 * bridge's own pale rail as supposed water. */
static bool FindBridgeWaterSource(
    uint8_t town, const uint8_t *wram, int cell_x, int cell_y,
    SimBackgroundBridgeAxis axis, int *source_x, int *source_y) {
  if ((axis != kSimBackgroundBridgeAxis_EastWest &&
       axis != kSimBackgroundBridgeAxis_NorthSouth) ||
      !source_x || !source_y)
    return false;
  static const int east_west_order[4][2] = {
    {0, -1}, {0, 1}, {-1, 0}, {1, 0},
  };
  static const int north_south_order[4][2] = {
    {-1, 0}, {1, 0}, {0, -1}, {0, 1},
  };
  const int (*direction)[2] = axis == kSimBackgroundBridgeAxis_EastWest
      ? east_west_order : north_south_order;
  const size_t direction_count =
      sizeof(east_west_order) / sizeof(east_west_order[0]);
  for (int distance = 1; distance < kSimBackgroundTownCells; distance++) {
    for (size_t candidate = 0; candidate < direction_count; candidate++) {
      int x = cell_x + direction[candidate][0] * distance;
      int y = cell_y + direction[candidate][1] * distance;
      if (x < 0 || x >= kSimBackgroundTownCells ||
          y < 0 || y >= kSimBackgroundTownCells)
        continue;
      if (!IsWateryTerrain(CellMapValue(town, wram, x, y))) continue;
      *source_x = x;
      *source_y = y;
      return true;
    }
  }
  return false;
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
  if (!town || town > kSimBackgroundTownCount || !wram) return;
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
          .record_slot = kSimBackgroundVoxelNoRecordSlot,
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

  /* Bridges are dynamic structure metatiles stored directly in the live cell
   * map. Group adjacent markers along their travel axis and compile one span
   * between solid banks, avoiding repeated end caps inside a wide crossing. */
  bool bridge_visited[kCellCount] = {false};
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++) {
      size_t cell = CellIndex(x, y);
      uint8_t tile = CellMapValue(town, wram, x, y);
      if (!IsBridgeTile(tile) || bridge_visited[cell]) continue;
      const SimBackgroundBridgeAxis bridge_axis = BridgeAxisForTile(tile);
      int dx = bridge_axis == kSimBackgroundBridgeAxis_EastWest;
      int dy = bridge_axis == kSimBackgroundBridgeAxis_NorthSouth;
      int first_x = x, first_y = y, last_x = x, last_y = y;
      while (first_x - dx >= 0 && first_y - dy >= 0 &&
             CellMapValue(town, wram, first_x - dx, first_y - dy) == tile) {
        first_x -= dx;
        first_y -= dy;
      }
      while (last_x + dx < kSimBackgroundTownCells &&
             last_y + dy < kSimBackgroundTownCells &&
             CellMapValue(town, wram, last_x + dx, last_y + dy) == tile) {
        last_x += dx;
        last_y += dy;
      }
      for (int bx = first_x, by = first_y;; bx += dx, by += dy) {
        bridge_visited[CellIndex(bx, by)] = true;
        if (bx == last_x && by == last_y) break;
      }
      int bank_a_x, bank_a_y, bank_b_x, bank_b_y;
      if (!FindBridgeBank(town, wram, first_x, first_y,
                          -dx, -dy, &bank_a_x, &bank_a_y) ||
          !FindBridgeBank(town, wram, last_x, last_y,
                          dx, dy, &bank_b_x, &bank_b_y))
        continue;
      SimBackgroundVoxelObject bridge = {
        .town = town,
        .kind = kSimBackgroundVoxel_Bridge,
        .cell_x = (uint8_t)first_x,
        .cell_y = (uint8_t)first_y,
        .source_cells_w = (uint8_t)(last_x - first_x + 1),
        .source_cells_h = (uint8_t)(last_y - first_y + 1),
        .footprint_cells_w = (uint8_t)(last_x - first_x + 1),
        .footprint_cells_d = (uint8_t)(last_y - first_y + 1),
        .record_slot = kSimBackgroundVoxelNoRecordSlot,
        .bridge_axis = (uint8_t)bridge_axis,
        .bridge_bank_a_x = (uint8_t)bank_a_x,
        .bridge_bank_a_y = (uint8_t)bank_a_y,
        .bridge_bank_b_x = (uint8_t)bank_b_x,
        .bridge_bank_b_y = (uint8_t)bank_b_y,
      };
      MarkOccupied(occupied, first_x, first_y,
                   bridge.source_cells_w, bridge.source_cells_h);
      if (!AppendObject(out, bridge)) return;
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
              .record_slot = kSimBackgroundVoxelNoRecordSlot,
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
              .record_slot = kSimBackgroundVoxelNoRecordSlot,
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

static void BuildGeneralGroundSourceMask(
    const SimBackgroundVoxelScene *scene, const uint8_t *wram,
    bool out[kCellCount]) {
  /* Unlike rendered RGB, the semantic cell survives brightness fades and
   * palette animation. A source selected on the first black map frame must
   * still be reusable horizontal land when its visible pixels arrive on later
   * frames. Town-aware classifiers matter here: Marahna reuses common mountain
   * ids, while its plateau walls are authored face topology rather than a
   * globally unique tile range. */
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      const size_t cell = CellIndex(cell_x, cell_y);
      const uint8_t tile =
          CellMapValue(scene->town, wram, cell_x, cell_y);
      out[cell] =
          !IsBridgeTile(tile) && !IsWateryTerrain(tile) &&
          !SimBackgroundMountains_CellOccupied(
              &scene->mountains, cell_x, cell_y) &&
          !SimTownTerrain_IsFaceCell(scene->town, cell_x, cell_y);
    }
}

static uint32_t GeneralGroundColour(
    const uint32_t *pixels, const bool source_cell[kCellCount]) {
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
      /* Bridge codes 2-5 have their own same-cell water replacement. Letting
       * their long river borders vote here could select water as the general
       * eraser beneath an unrelated house or forest. */
      if (g_object_mask[at] != kEnhancedReplacement_Ground) continue;
      static const int dx[] = {0, 1, 0, -1};
      static const int dy[] = {-1, 0, 1, 0};
      for (int edge = 0; edge < 4; edge++) {
        int nx = x + dx[edge], ny = y + dy[edge];
        if (nx < 0 || nx >= kSimTownCanvasPixels ||
            ny < 0 || ny >= kSimTownCanvasPixels)
          continue;
        size_t next = (size_t)ny * kSimTownCanvasPixels + (size_t)nx;
        if (!source_cell[CellIndex(
                nx / kSimBackgroundCellPixels,
                ny / kSimBackgroundCellPixels)])
          continue;
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
                               const bool source_cell[kCellCount],
                               int *ground_cell_x, int *ground_cell_y) {
  int best_score = 0, best_x = 0, best_y = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      if (CellIsMasked(cell_x, cell_y)) continue;
      if (!source_cell[CellIndex(cell_x, cell_y)]) continue;
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

static bool FindGeneralGroundCell(
    const uint32_t *pixels, uint8_t town,
    const bool source_cell[kCellCount],
    int *ground_cell_x, int *ground_cell_y) {
  /* Northwall contains deliberately green landmark plots. Their long border
   * can dominate the object-neighbour vote even though the general terrain is
   * snow, producing a conspicuous green rectangle under a replaced landmark.
   * Prefer a complete unmasked snow cell and retain the ordinary colour vote
   * as a fallback for fades or unusual captures with no detectable snow. */
  if (town == kSimBackgroundTownCount && FindSnowGroundCell(
          pixels, source_cell, ground_cell_x, ground_cell_y))
    return true;
  uint32_t ground_colour = GeneralGroundColour(pixels, source_cell);
  int best_score = -1, best_x = 0, best_y = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundTownCells; cell_x++) {
      if (CellIsMasked(cell_x, cell_y)) continue;
      if (!source_cell[CellIndex(cell_x, cell_y)]) continue;
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

/* Clean semantic parts used by every complete mountain stamp. Keeping this
 * list beside both plan and refresh code prevents the two stages drifting. */
static const uint8_t kMountainSourceTiles[] = {
  0x70, 0x71, 0x81, 0x82, 0x88, 0x89, 0x8A, 0x8B,
  0x8C, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
  0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D,
  0x9E, 0x9F,
};

static void BuildCleanMountainSourcePlan(void) {
  memset(g_background.mountain_source_cell, 0,
         sizeof(g_background.mountain_source_cell));
  bool used[kCellCount] = {false};
  for (size_t tile_at = 0;
       tile_at < sizeof(kMountainSourceTiles) /
                     sizeof(kMountainSourceTiles[0]); tile_at++) {
    uint8_t tile = kMountainSourceTiles[tile_at];
    bool test_opaque;
    if (!SimBackgroundMountainSilhouette_Lookup(
            tile, 0, 0, &test_opaque))
      continue;
    int cell_x, cell_y;
    if (!FindMountainScratchCell(used, &cell_x, &cell_y)) return;
    used[CellIndex(cell_x, cell_y)] = true;
    g_background.mountain_source_cell[tile] =
        (uint16_t)(CellIndex(cell_x, cell_y) + 1);
  }
}

static void MarkGroundDirtyPixel(int x, int y) {
  if (g_background.ground_dirty_x1[y] <=
      g_background.ground_dirty_x0[y]) {
    g_background.ground_dirty_x0[y] = x;
    g_background.ground_dirty_x1[y] = x + 1;
    return;
  }
  if (x < g_background.ground_dirty_x0[y])
    g_background.ground_dirty_x0[y] = x;
  if (x + 1 > g_background.ground_dirty_x1[y])
    g_background.ground_dirty_x1[y] = x + 1;
}

static void SetGroundPixel(size_t at, int x, int y, uint32_t value,
                           uint64_t *changed_pixels) {
  if (g_background.ground[at] == value) return;
  g_background.ground[at] = value;
  MarkGroundDirtyPixel(x, y);
  (*changed_pixels)++;
}

static void SetAtlasPixel(size_t at, uint32_t value,
                          uint64_t *changed_pixels) {
  if (g_background.atlas[at] == value) return;
  g_background.atlas[at] = value;
  (*changed_pixels)++;
}

static void RefreshCleanMountainSources(const uint8_t *wram,
                                        uint64_t *atlas_changed_pixels) {
  /* Render from raw definitions on every pixel publication. Palette fades and
   * character animation can recolour these cells without changing topology. */
  uint32_t metatile_pixels[kSimBackgroundCellPixels *
                           kSimBackgroundCellPixels];
  for (size_t tile_at = 0;
       tile_at < sizeof(kMountainSourceTiles) /
                     sizeof(kMountainSourceTiles[0]); tile_at++) {
    uint8_t tile = kMountainSourceTiles[tile_at];
    uint16_t source_cell = g_background.mountain_source_cell[tile];
    if (!source_cell || !SimTownCanvas_RenderTerrainMetatile(
            wram, tile, metatile_pixels))
      continue;
    int cell = source_cell - 1;
    int cell_x = cell % kSimBackgroundTownCells;
    int cell_y = cell / kSimBackgroundTownCells;
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
        SetAtlasPixel(destination,
                      opaque ? source | 0xFF000000u : 0,
                      atlas_changed_pixels);
      }
  }
}

/* One pass over the field: each component's lowest occupied row becomes the
 * baseline for every cell in it. Components are four-connected, so this is the
 * same bottom edge the renderer derives when it reconstructs a mountain mass. */
static void BuildMountainBaselines(const SimBackgroundVoxelScene *scene) {
  memset(g_background.mountain_baseline_row, 0,
         sizeof(g_background.mountain_baseline_row));
  uint8_t bottom_by_component[kCellCount + 1] = {0};
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++) {
      if (!SimBackgroundMountains_CellOccupied(&scene->mountains, x, y))
        continue;
      uint16_t component = scene->mountains.component[CellIndex(x, y)];
      if (component > kCellCount) continue;
      if ((uint8_t)(y + 1) > bottom_by_component[component])
        bottom_by_component[component] = (uint8_t)(y + 1);
    }
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++) {
      size_t cell = CellIndex(x, y);
      if (!SimBackgroundMountains_CellOccupied(&scene->mountains, x, y))
        continue;
      uint16_t component = scene->mountains.component[cell];
      if (component > kCellCount) continue;
      g_background.mountain_baseline_row[cell] =
          bottom_by_component[component];
    }
}

static bool ObjectHasStructuralHeight(const SimBackgroundVoxelObject *object) {
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
    case kSimBackgroundVoxel_Cathedral:
    case kSimBackgroundVoxel_Windmill:
    case kSimBackgroundVoxel_Factory:
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
    case kSimBackgroundVoxel_Pyramid:
      return true;
    default:
      return false;
  }
}

static void BuildStructureHeights(const SimBackgroundVoxelScene *scene) {
  memset(g_background.structure_height, 0,
         sizeof(g_background.structure_height));
  /* Preserve the classifier's order as the tie-breaker. This exactly matches
   * the former first-object result if transitional footprints overlap. */
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (!ObjectHasStructuralHeight(object)) continue;
    const float height = SimBackgroundVoxelRegion_AuthoredHeight(object);
    int x0 = object->cell_x;
    int y0 = object->cell_y;
    int x1 = x0 + object->source_cells_w;
    int y1 = y0 + object->source_cells_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kSimBackgroundTownCells) x1 = kSimBackgroundTownCells;
    if (y1 > kSimBackgroundTownCells) y1 = kSimBackgroundTownCells;
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++) {
        float *cell_height =
            &g_background.structure_height[CellIndex(x, y)];
        if (*cell_height <= 0.0f) *cell_height = height;
      }
  }
}

static void BuildEnhancedReplacementPlan(
    const uint8_t *wram, const uint32_t *pixels,
    const SimBackgroundVoxelScene *scene) {
  memset(g_object_mask, 0, sizeof(g_object_mask));
  memset(g_atlas_alpha, 0, sizeof(g_atlas_alpha));
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
          bool opaque = false;
          if (!SimBackgroundMountainSilhouette_Lookup(
                  tile, x, y, &opaque)) {
            g_atlas_alpha[at] = kAtlasAlpha_Source;
          } else {
            g_atlas_alpha[at] = opaque
                ? kAtlasAlpha_Opaque : kAtlasAlpha_Transparent;
          }
          g_object_mask[at] = kEnhancedReplacement_Ground;
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
        EnhancedReplacementKind replacement_kind =
            kEnhancedReplacement_Ground;
        if (object->kind == kSimBackgroundVoxel_Bridge) {
          /* Mask the entire authored metatile, not only its nominal roadway.
           * Pale rail pixels live outside that band and otherwise survive as
           * a second, detached bridge in the projected ground texture. */
          replacement_kind = BridgeReplacementKind(
              (SimBackgroundBridgeAxis)object->bridge_axis);
        }
        size_t at = (size_t)(y0 + y) * kSimTownCanvasPixels +
            (size_t)(x0 + x);
        g_object_mask[at] = (uint8_t)replacement_kind;
        /* Retained for diagnostic/catalog consumers. The enhanced renderer
         * uses its authored model and never samples this authentic cutout. */
        g_atlas_alpha[at] = kAtlasAlpha_Opaque;
      }
  }

  bool general_ground_source[kCellCount];
  BuildGeneralGroundSourceMask(scene, wram, general_ground_source);
  int ground_cell_x, ground_cell_y;
  g_background.have_general_ground = FindGeneralGroundCell(
      pixels, scene->town, general_ground_source,
      &ground_cell_x, &ground_cell_y);
  if (g_background.have_general_ground) {
    g_background.general_ground_cell_x = (uint8_t)ground_cell_x;
    g_background.general_ground_cell_y = (uint8_t)ground_cell_y;
  }
  BuildCleanMountainSourcePlan();
}

static void RefreshEnhancedPixels(
    const uint8_t *wram, const uint16_t *vram, const uint32_t *pixels,
    const SimBackgroundVoxelScene *scene) {
  uint32_t bridge_river[kSimBackgroundBridgeAxis_Count]
      [kSimBackgroundCellPixels * kSimBackgroundCellPixels];
  bool have_bridge_river[kSimBackgroundBridgeAxis_Count] = {
    [kSimBackgroundBridgeAxis_EastWest] =
        SimTownCanvas_RenderTerrainMetatile(
            wram, kBridgeRiverEastWest,
            bridge_river[kSimBackgroundBridgeAxis_EastWest]),
    [kSimBackgroundBridgeAxis_NorthSouth] =
        SimTownCanvas_RenderTerrainMetatile(
            wram, kBridgeRiverNorthSouth,
            bridge_river[kSimBackgroundBridgeAxis_NorthSouth]),
  };
  uint64_t ground_changed_pixels = 0;
  uint64_t atlas_changed_pixels = 0;
  int ground_x0 = (int)g_background.general_ground_cell_x *
      kSimBackgroundCellPixels;
  int ground_y0 = (int)g_background.general_ground_cell_y *
      kSimBackgroundCellPixels;

  /* The same complete biome tile erases every source cell. Grass towns keep
   * their grass texture, Northwall keeps snow, and no nearest-pixel flood can
   * create streaks around a large forest, cathedral, or lifted mountain. */
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++) {
      size_t at = (size_t)y * kSimTownCanvasPixels + (size_t)x;
      uint32_t replacement = pixels[at];
      int source_x = x, source_y = y;
      SimBackgroundBridgeAxis bridge_axis = ReplacementBridgeAxis(
          (EnhancedReplacementKind)g_object_mask[at]);
      if (g_object_mask[at] && g_background.have_general_ground) {
        source_x = ground_x0 + x % kSimBackgroundCellPixels;
        source_y = ground_y0 + y % kSimBackgroundCellPixels;
      }
      if (g_object_mask[at] &&
          bridge_axis != kSimBackgroundBridgeAxis_None) {
        int water_cell_x, water_cell_y;
        if (!have_bridge_river[bridge_axis] && FindBridgeWaterSource(
                scene->town, wram,
                x / kSimBackgroundCellPixels,
                y / kSimBackgroundCellPixels,
                bridge_axis, &water_cell_x, &water_cell_y)) {
          source_x = water_cell_x * kSimBackgroundCellPixels +
              x % kSimBackgroundCellPixels;
          source_y = water_cell_y * kSimBackgroundCellPixels +
              y % kSimBackgroundCellPixels;
        }
      }
      if (g_object_mask[at]) replacement =
          bridge_axis != kSimBackgroundBridgeAxis_None &&
              have_bridge_river[bridge_axis]
          ? bridge_river[bridge_axis][
                (y % kSimBackgroundCellPixels) *
                    kSimBackgroundCellPixels +
                x % kSimBackgroundCellPixels]
          : pixels[(size_t)source_y * kSimTownCanvasPixels +
                   (size_t)source_x];
      SetGroundPixel(at, x, y, replacement, &ground_changed_pixels);
      bool atlas_opaque = g_atlas_alpha[at] == kAtlasAlpha_Opaque ||
          (g_atlas_alpha[at] == kAtlasAlpha_Source &&
           SimTownCanvas_SourcePixelOpaque(wram, vram, x, y));
      SetAtlasPixel(at, atlas_opaque ? pixels[at] | 0xFF000000u : 0,
                    &atlas_changed_pixels);
    }
  RefreshCleanMountainSources(wram, &atlas_changed_pixels);

  g_build_stats.pixel_refreshes++;
  g_build_stats.ground_pixels_changed += ground_changed_pixels;
  g_build_stats.atlas_pixels_changed += atlas_changed_pixels;
  if (ground_changed_pixels) g_background.ground_serial = NextSerial();
  if (atlas_changed_pixels) g_background.atlas_serial = NextSerial();
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

static const uint8_t *TownCellMapSource(uint8_t town, const uint8_t *wram) {
  return wram + kTownCellMapsWram +
      (size_t)(town - 1) * kTownCellMapBytes;
}

static const uint8_t *TownStructureRecordSource(
    uint8_t town, const uint8_t *wram) {
  return wram + kStructureRecordsWram +
      (size_t)(town - 1) * kStructureRecordsPerTownBytes;
}

static bool StructureSceneInputsChanged(
    uint8_t town, const uint8_t *wram) {
  const uint8_t *records = TownStructureRecordSource(town, wram);
  for (int slot = 0; slot < kStructureRecordCount; slot++)
    if (memcmp(g_background.structure_records +
                   (size_t)slot * kStructureSceneBytesPerRecord,
               records + (size_t)slot * kStructureRecordBytes,
               kStructureSceneBytesPerRecord) != 0)
      return true;
  return false;
}

static void SaveStructureSceneInputs(uint8_t town, const uint8_t *wram) {
  const uint8_t *records = TownStructureRecordSource(town, wram);
  for (int slot = 0; slot < kStructureRecordCount; slot++)
    memcpy(g_background.structure_records +
               (size_t)slot * kStructureSceneBytesPerRecord,
           records + (size_t)slot * kStructureRecordBytes,
           kStructureSceneBytesPerRecord);
}

static bool SceneInputsChanged(uint8_t town, const uint8_t *wram,
                               uint32_t canvas_layout_serial,
                               bool wind_stops_all) {
  return !g_background.have_scene_inputs ||
      g_background.scene.town != town ||
      g_background.canvas_layout_serial != canvas_layout_serial ||
      g_background.wind_stops_all != wind_stops_all ||
      memcmp(g_background.cell_map, TownCellMapSource(town, wram),
             sizeof(g_background.cell_map)) != 0 ||
      StructureSceneInputsChanged(town, wram) ||
      memcmp(g_background.terrain_definitions,
             wram + kTerrainDefinitionsWram,
             sizeof(g_background.terrain_definitions)) != 0 ||
      memcmp(g_background.structure_definitions,
             wram + kStructureDefinitionsWram,
             sizeof(g_background.structure_definitions)) != 0;
}

static void SaveSceneInputs(uint8_t town, const uint8_t *wram,
                            uint32_t canvas_layout_serial,
                            bool wind_stops_all) {
  memcpy(g_background.cell_map, TownCellMapSource(town, wram),
         sizeof(g_background.cell_map));
  SaveStructureSceneInputs(town, wram);
  memcpy(g_background.terrain_definitions,
         wram + kTerrainDefinitionsWram,
         sizeof(g_background.terrain_definitions));
  memcpy(g_background.structure_definitions,
         wram + kStructureDefinitionsWram,
         sizeof(g_background.structure_definitions));
  g_background.canvas_layout_serial = canvas_layout_serial;
  g_background.wind_stops_all = wind_stops_all;
  g_background.have_scene_inputs = true;
}

void SimBackgroundVoxels_Reset(void) {
  memset(&g_background, 0, sizeof(g_background));
  memset(g_object_mask, 0, sizeof(g_object_mask));
  memset(g_atlas_alpha, 0, sizeof(g_atlas_alpha));
}

void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               const uint16_t *vram,
                               uint32_t canvas_serial,
                               uint32_t canvas_layout_serial,
                               bool wind_stops_all) {
  if (!town || town > kSimBackgroundTownCount || !wram || !canvas_pixels ||
      !vram || !canvas_serial || !canvas_layout_serial)
    return;
  g_build_stats.build_calls++;
  bool scene_changed = SceneInputsChanged(
      town, wram, canvas_layout_serial, wind_stops_all);
  bool pixels_changed = g_background.canvas_serial != canvas_serial;
  if (!scene_changed && !pixels_changed) {
    LogWindmills(town, wram, &g_background.scene);
    return;
  }

  if (scene_changed) {
    SimBackgroundVoxels_Classify(town, wram, wind_stops_all,
                                 &g_background.scene);
    BuildEnhancedReplacementPlan(wram, canvas_pixels, &g_background.scene);
    BuildMountainBaselines(&g_background.scene);
    BuildStructureHeights(&g_background.scene);
    SaveSceneInputs(town, wram, canvas_layout_serial, wind_stops_all);
    g_background.scene_serial = NextSerial();
    g_build_stats.scene_rebuilds++;
  }
  uint32_t prior_ground_serial = g_background.ground_serial;
  uint32_t prior_atlas_serial = g_background.atlas_serial;
  if (scene_changed || pixels_changed)
    RefreshEnhancedPixels(wram, vram, canvas_pixels, &g_background.scene);
  g_background.canvas_serial = canvas_serial;
  LogWindmills(town, wram, &g_background.scene);
  if (scene_changed || prior_ground_serial != g_background.ground_serial ||
      prior_atlas_serial != g_background.atlas_serial)
    g_background.serial = NextSerial();
}

uint32_t SimBackgroundVoxels_Serial(void) { return g_background.serial; }
uint32_t SimBackgroundVoxels_SceneSerial(void) {
  return g_background.scene_serial;
}
uint32_t SimBackgroundVoxels_GroundSerial(void) {
  return g_background.ground_serial;
}
uint32_t SimBackgroundVoxels_AtlasSerial(void) {
  return g_background.atlas_serial;
}
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void) {
  return &g_background.scene;
}
const uint32_t *SimBackgroundVoxels_AtlasPixels(void) {
  return g_background.atlas;
}
const uint32_t *SimBackgroundVoxels_GroundPixels(void) {
  return g_background.ground;
}

bool SimBackgroundVoxels_TakeGroundDirtyRect(
    int *x, int *y, int *width, int *height) {
  int first_row = 0;
  while (first_row < kSimTownCanvasPixels &&
         g_background.ground_dirty_x1[first_row] <=
             g_background.ground_dirty_x0[first_row])
    first_row++;
  if (first_row == kSimTownCanvasPixels) return false;

  int dirty_x0 = g_background.ground_dirty_x0[first_row];
  int dirty_x1 = g_background.ground_dirty_x1[first_row];
  int end_row = first_row;
  do {
    g_background.ground_dirty_x0[end_row] = kSimTownCanvasPixels;
    g_background.ground_dirty_x1[end_row] = 0;
    end_row++;
  } while (end_row < kSimTownCanvasPixels &&
           g_background.ground_dirty_x0[end_row] == dirty_x0 &&
           g_background.ground_dirty_x1[end_row] == dirty_x1);

  if (x) *x = dirty_x0;
  if (y) *y = first_row;
  if (width) *width = dirty_x1 - dirty_x0;
  if (height) *height = end_row - first_row;
  return true;
}

SimBackgroundVoxelBuildStats SimBackgroundVoxels_BuildStats(void) {
  return g_build_stats;
}

void SimBackgroundVoxels_ResetBuildStats(void) {
  memset(&g_build_stats, 0, sizeof(g_build_stats));
}

bool SimBackgroundVoxels_CellIsMountain(int cell_x, int cell_y) {
  return SimBackgroundMountains_CellOccupied(
      &g_background.scene.mountains, cell_x, cell_y);
}

bool SimBackgroundVoxels_MountainInFrontOf(int cell_x, int cell_y) {
  /* Bounded by how far the art can actually reach, not by the map. A mountain
   * displaces its raised pixels south by `rise * (1 - face_depth_scale)`, so
   * the tallest mass in the game covers about four cells beyond its own base;
   * past that it cannot overlap anything. Scanning the whole column instead
   * put 598 of Aitos's 626 ground cells "behind a mountain" -- the south rim
   * alone claimed the entire town, which is no split at all. */
  /* A mass H cells tall displaces its highest pixels south by
   * H * (1 - face_depth_scale) cells -- about 0.38 * H. The towns' masses run
   * to roughly ten cells, so four covers them. Stated as a constant rather
   * than computed from face_depth_scale alone, because the other half of the
   * product is the mass height, which is not known at this point.
   * CheckMountainOcclusionReach in the voxels test pins the resulting split,
   * so widening or narrowing this shows up as a failure rather than as actors
   * quietly reappearing through peaks. */
  enum { kMountainReachCells = 4 };
  int limit = cell_y + kMountainReachCells;
  if (limit >= kSimBackgroundTownCells) limit = kSimBackgroundTownCells - 1;
  for (int y = cell_y + 1; y <= limit; y++)
    if (SimBackgroundMountains_CellOccupied(
            &g_background.scene.mountains, cell_x, y))
      return true;
  return false;
}

bool SimBackgroundVoxels_MountainSurface(int map_x, int map_y,
                                         float *out_map_y, float *out_height) {
  int cell_x = map_x / kSimBackgroundCellPixels;
  int cell_y = map_y / kSimBackgroundCellPixels;
  if (cell_x < 0 || cell_x >= kSimBackgroundTownCells ||
      cell_y < 0 || cell_y >= kSimBackgroundTownCells)
    return false;
  uint8_t baseline_row = g_background.mountain_baseline_row[
      CellIndex(cell_x, cell_y)];
  if (!baseline_row) return false;
  SimBackgroundMountainRelief relief;
  /* Both scales are detail-independent, so the surface an actor stands on does
   * not move when the player changes model detail. */
  SimBackgroundMountainRelief_Resolve(
      kSimBackgroundVoxelDetail_High, &relief);
  float baseline = (float)baseline_row * (float)kSimBackgroundCellPixels;
  float rise = baseline - (float)map_y;
  if (rise < 0.0f) rise = 0.0f;
  /* The same shear the geometry uses: the art is pulled toward its base by
   * face_depth_scale and raised by face_height_scale, so a point standing on
   * it moves south by the part of the rise the shear did not consume. */
  if (out_map_y) *out_map_y = (float)map_y + rise * (1.0f - relief.face_depth_scale);
  if (out_height) *out_height = rise * relief.face_height_scale;
  return true;
}

static float StructureHeightAtCell(int cell_x, int cell_y) {
  if (cell_x < 0 || cell_x >= kSimBackgroundTownCells ||
      cell_y < 0 || cell_y >= kSimBackgroundTownCells)
    return 0.0f;
  return g_background.structure_height[CellIndex(cell_x, cell_y)];
}

float SimBackgroundVoxels_StructureHeight(int map_x, int map_y) {
  /* Search downward rather than sampling one cell.
   *
   * Structure height is a step function at every cell boundary -- measured on
   * Aitos it is 0 at map_y 382 and 9.5 at 384 -- and the ROM gives these
   * bubbles a two-or-three pixel bounce. Sampling a single cell therefore
   * flips the lift between nothing and a whole storey on alternate frames
   * whenever the bounce straddles an edge, which reads as the bubble leaping.
   * Scanning down from the art's own bottom edge lands on the structure the
   * bubble is sitting on and keeps answering the same height through the
   * whole bounce, so the ROM's motion shows through unchanged. One cell of
   * reach is enough for a bubble resting on a roof and short enough that it
   * cannot borrow the height of some unrelated building further south. */
  int cell_x = map_x / kSimBackgroundCellPixels;
  for (int offset = 0; offset <= kSimBackgroundCellPixels; offset += 4) {
    float height = StructureHeightAtCell(
        cell_x, (map_y + offset) / kSimBackgroundCellPixels);
    if (height > 0.0f) return height;
  }
  return 0.0f;
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
