#include "sim_background_voxels.h"

#include "sim_background_mountain_silhouette.h"

#include <stddef.h>
#include <string.h>

enum {
  kTownCellMapsWram = 0x12000,       /* flat $7F:2000 */
  kTownCellMapBytes = 0x400,
  kStructureRecordsWram = 0x16BE7,  /* flat $7F:6BE7 */
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
  kCathedralTopLeft = 0xC2,
  kCathedralTopRight = 0xC3,
  kCathedralBottomLeft = 0xCA,
  kCathedralBottomRight = 0xCB,
  kCellCount = kSimBackgroundTownCells * kSimBackgroundTownCells,
  kCanvasPixelCount = kSimTownCanvasPixels * kSimTownCanvasPixels,
};

static struct {
  uint32_t serial;
  uint32_t canvas_serial;
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

static bool StrongTreePixel(uint32_t argb) {
  unsigned red = (argb >> 16) & 0xFF;
  unsigned green = (argb >> 8) & 0xFF;
  unsigned blue = argb & 0xFF;
  /* Across all six captured town palettes, canopy greens are separated from
   * olive grass by chroma rather than by a town-specific colour index. The
   * low floor keeps classification stable during ordinary brightness fades. */
  return green >= 8 && green * 10 > red * 13 && green * 10 > blue * 12;
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
                                  const uint32_t *canvas_pixels,
                                  SimBackgroundVoxelScene *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!town || town > 6 || !wram || !canvas_pixels) return;
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
      object.height_pixels = 16;
    } else if (structure_class == kStructureClassWindmill) {
      if (flags & kStructureConstructionVariant)
        object.flags |= kSimBackgroundVoxel_UnderConstruction;
      object.kind = kSimBackgroundVoxel_Windmill;
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = 2;
      object.footprint_cells_d = 1;
      object.height_pixels = 32;
    } else if (structure_class == kStructureClassFactory) {
      if (flags & kStructureConstructionVariant)
        object.flags |= kSimBackgroundVoxel_UnderConstruction;
      object.kind = kSimBackgroundVoxel_Factory;
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = object.footprint_cells_d = 2;
      object.height_pixels = 8;
    } else {
      continue;
    }
    if (!AppendObject(out, object)) return;
  }

  /* One stable four-cell signature identifies the cathedral in every town.
   * Its top row is elevation art, not a second ground-depth row. */
  bool cathedral_found = false;
  for (int y = 0; y < kSimBackgroundTownCells - 1 && !cathedral_found; y++)
    for (int x = 0; x < kSimBackgroundTownCells - 1; x++)
      if (CellMapValue(town, wram, x, y) == kCathedralTopLeft &&
          CellMapValue(town, wram, x + 1, y) == kCathedralTopRight &&
          CellMapValue(town, wram, x, y + 1) == kCathedralBottomLeft &&
          CellMapValue(town, wram, x + 1, y + 1) ==
              kCathedralBottomRight) {
        AppendObject(out, (SimBackgroundVoxelObject){
          .town = town,
          .kind = kSimBackgroundVoxel_Cathedral,
          .cell_x = (uint8_t)x,
          .cell_y = (uint8_t)y,
          .source_cells_w = 2,
          .source_cells_h = 2,
          .footprint_cells_w = 2,
          /* All four cells are protected land: the game never places another
           * structure behind the cathedral. The upper source row is therefore
           * both real depth and the perspective-compressed second tier. */
          .footprint_cells_d = 2,
          .height_pixels = 24,
          .record_slot = 0xFF,
        });
        MarkOccupied(occupied, x, y, 2, 2);
        cathedral_found = true;
        break;
      }

  bool tree[kCellCount] = {false};
  bool weak_tree[kCellCount] = {false};
  for (int y = 0; y < kSimBackgroundTownCells; y++)
    for (int x = 0; x < kSimBackgroundTownCells; x++) {
      size_t cell = CellIndex(x, y);
      if (occupied[cell]) continue;
      int green = CellTreePixelCount(canvas_pixels, x, y);
      /* Main canopies occupy at least 30% of a cell. Edge/shadow variants can
       * fall to 10%, just above the measured 7% ordinary-terrain maximum. */
      tree[cell] = green * 10 >= kSimBackgroundCellPixels *
          kSimBackgroundCellPixels * 3;
      weak_tree[cell] = green * 10 >= kSimBackgroundCellPixels *
          kSimBackgroundCellPixels;
    }

  /* Grow only from high-confidence canopies. This captures dark perimeter and
   * trunk-heavy continuation cells without allowing an isolated patch of
   * ordinary green decoration to become a tree object. */
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
              /* Marahna's permanent forest art is a broad tropical palm
               * family. It shares extraction/adjacency with other trees but
               * must not inherit the pointed evergreen model. */
              .kind = town == 5 ? kSimBackgroundVoxel_Palm
                                : kSimBackgroundVoxel_Tree,
              .flags = write == 1 ? kSimBackgroundVoxel_IsolatedTree : 0,
              .cell_x = (uint8_t)x,
              .cell_y = (uint8_t)y,
              .source_cells_w = 1,
              .source_cells_h = 1,
              .footprint_cells_w = 1,
              .footprint_cells_d = 1,
              .height_pixels = 15,
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

void SimBackgroundVoxels_Reset(void) { memset(&g_background, 0, sizeof(g_background)); }

void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               const uint16_t *vram,
                               uint32_t canvas_serial) {
  if (!town || !wram || !canvas_pixels || !vram || !canvas_serial) return;
  if (g_background.scene.town == town &&
      g_background.canvas_serial == canvas_serial)
    return;
  uint32_t next_serial = NextSerial();
  memset(g_background.atlas, 0, sizeof(g_background.atlas));
  memset(g_background.mountain_source_cell, 0,
         sizeof(g_background.mountain_source_cell));
  memcpy(g_background.ground, canvas_pixels, sizeof(g_background.ground));
  SimBackgroundVoxels_Classify(town, wram, canvas_pixels,
                               &g_background.scene);
  ExtractEnhancedReplacements(wram, vram, canvas_pixels, &g_background.scene);
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
