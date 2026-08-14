#include "sim_background_voxels.h"

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
} g_background;
/* Scene rebuild scratch. Every classified source rectangle is marked before a
 * replacement tile is selected, so authentic object art is never considered
 * as the town's general ground. */
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
  return (size_t)y * kSimBackgroundTownCells + x;
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
              pixels[(size_t)(y0 + y) * kSimTownCanvasPixels + x0 + x]))
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
              .kind = kSimBackgroundVoxel_Tree,
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
      if (g_object_mask[(size_t)(y0 + y) * kSimTownCanvasPixels + x0 + x])
        return true;
  return false;
}

static uint32_t GeneralGroundColour(const uint32_t *pixels) {
  enum { kMaxColours = 1024 };
  uint32_t colours[kMaxColours];
  uint32_t counts[kMaxColours];
  int colour_count = 0;
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++) {
      size_t at = (size_t)y * kSimTownCanvasPixels + x;
      if (!g_object_mask[at]) continue;
      static const int dx[] = {0, 1, 0, -1};
      static const int dy[] = {-1, 0, 1, 0};
      for (int edge = 0; edge < 4; edge++) {
        int nx = x + dx[edge], ny = y + dy[edge];
        if (nx < 0 || nx >= kSimTownCanvasPixels ||
            ny < 0 || ny >= kSimTownCanvasPixels)
          continue;
        size_t next = (size_t)ny * kSimTownCanvasPixels + nx;
        uint32_t colour = pixels[next];
        if (g_object_mask[next] || StrongTreePixel(colour)) continue;
        int index = 0;
        while (index < colour_count && colours[index] != colour) index++;
        if (index == colour_count) {
          if (colour_count >= kMaxColours) continue;
          colours[index] = colour;
          counts[index] = 0;
          colour_count++;
        }
        counts[index]++;
      }
    }
  int best = -1;
  for (int i = 0; i < colour_count; i++)
    if (best < 0 || counts[i] > counts[best]) best = i;
  return best >= 0 ? colours[best] : pixels[0];
}

static bool FindGeneralGroundCell(const uint32_t *pixels,
                                  int *ground_cell_x, int *ground_cell_y) {
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
          if (pixels[(size_t)(y0 + y) * kSimTownCanvasPixels + x0 + x] ==
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

static void ExtractVoxelObjects(const uint32_t *pixels,
                                const SimBackgroundVoxelScene *scene) {
  memset(g_object_mask, 0, sizeof(g_object_mask));
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    int x0 = object->cell_x * kSimBackgroundCellPixels;
    int y0 = object->cell_y * kSimBackgroundCellPixels;
    int width = object->source_cells_w * kSimBackgroundCellPixels;
    int height = object->source_cells_h * kSimBackgroundCellPixels;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++) {
        size_t at = (size_t)(y0 + y) * kSimTownCanvasPixels + x0 + x;
        g_object_mask[at] = 1;
        /* Retained for diagnostic/catalog consumers. The enhanced renderer
         * uses its authored model and never samples this authentic cutout. */
        g_background.atlas[at] = pixels[at] | 0xFF000000u;
      }
  }

  int ground_cell_x, ground_cell_y;
  if (!FindGeneralGroundCell(pixels, &ground_cell_x, &ground_cell_y)) return;
  int ground_x0 = ground_cell_x * kSimBackgroundCellPixels;
  int ground_y0 = ground_cell_y * kSimBackgroundCellPixels;
  /* The same complete biome tile erases every source cell. Grass towns keep
   * their grass texture, Northwall keeps snow, and no nearest-pixel flood can
   * create streaks around a large forest or cathedral footprint. */
  for (int y = 0; y < kSimTownCanvasPixels; y++)
    for (int x = 0; x < kSimTownCanvasPixels; x++) {
      size_t at = (size_t)y * kSimTownCanvasPixels + x;
      if (!g_object_mask[at]) continue;
      int source_x = ground_x0 + x % kSimBackgroundCellPixels;
      int source_y = ground_y0 + y % kSimBackgroundCellPixels;
      g_background.ground[at] =
          pixels[(size_t)source_y * kSimTownCanvasPixels + source_x];
    }
}

void SimBackgroundVoxels_Reset(void) { memset(&g_background, 0, sizeof(g_background)); }

void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               uint32_t canvas_serial) {
  if (!town || !wram || !canvas_pixels || !canvas_serial) return;
  if (g_background.scene.town == town &&
      g_background.canvas_serial == canvas_serial)
    return;
  uint32_t next_serial = NextSerial();
  memset(g_background.atlas, 0, sizeof(g_background.atlas));
  memcpy(g_background.ground, canvas_pixels, sizeof(g_background.ground));
  SimBackgroundVoxels_Classify(town, wram, canvas_pixels,
                               &g_background.scene);
  ExtractVoxelObjects(canvas_pixels, &g_background.scene);
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
