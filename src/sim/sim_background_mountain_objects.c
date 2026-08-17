#include "sim_background_mountain_objects.h"

#include <stddef.h>
#include <string.h>

typedef struct MountainStamp {
  uint8_t width, height;
  uint8_t rows[kSimBackgroundMountainObjectMaxRows];
  uint8_t tiles[kSimBackgroundMountainObjectMaxRows]
               [kSimBackgroundMountainObjectMaxColumns];
} MountainStamp;

/* These are assembled from Fillmore's underlying $78-$9F terrain-metatile
 * definitions, not cropped from either visible range. The latter contains
 * fused overlap variants which import a neighbouring mountain's shoulder
 * when treated as an independent object. */
static const MountainStamp kCommonSmall = {
  .width = 4,
  .height = 4,
  .rows = {0x06, 0x0F, 0x0F, 0x0F},
  .tiles = {
    {0x00, 0x81, 0x82, 0x00},
    {0x88, 0x89, 0x8A, 0x8B},
    {0x90, 0x91, 0x92, 0x93},
    {0x98, 0x99, 0x9A, 0x9B},
  },
};

static const MountainStamp kFillmoreLarge = {
  .width = 6,
  .height = 6,
  .rows = {0x0C, 0x1E, 0x3F, 0x3F, 0x3F, 0x3F},
  .tiles = {
    {0x00, 0x00, 0x81, 0x82, 0x00, 0x00},
    {0x00, 0x88, 0x89, 0x8A, 0x8B, 0x00},
    {0x88, 0x8C, 0x91, 0x92, 0x8F, 0x8B},
    {0x90, 0x94, 0x95, 0x96, 0x97, 0x9F},
    {0x90, 0x91, 0x9D, 0x9E, 0x92, 0x93},
    {0x98, 0x99, 0x9A, 0x99, 0x9A, 0x9B},
  },
};

/* Aitos uses the same semantic ridge parts, but its intact large northern
 * peak has the authored $9C left wall in row three. The volcano is a second
 * large stamp whose only difference is the red $70/$71 crown. */
static const MountainStamp kAitosLarge = {
  .width = 6,
  .height = 6,
  .rows = {0x0C, 0x1E, 0x3F, 0x3F, 0x3F, 0x3F},
  .tiles = {
    {0x00, 0x00, 0x81, 0x82, 0x00, 0x00},
    {0x00, 0x88, 0x89, 0x8A, 0x8B, 0x00},
    {0x88, 0x8C, 0x91, 0x92, 0x8F, 0x8B},
    {0x9C, 0x94, 0x95, 0x96, 0x97, 0x9F},
    {0x90, 0x91, 0x9D, 0x9E, 0x92, 0x93},
    {0x98, 0x99, 0x9A, 0x99, 0x9A, 0x9B},
  },
};

static const MountainStamp kAitosVolcano = {
  .width = 6,
  .height = 6,
  .rows = {0x0C, 0x1E, 0x3F, 0x3F, 0x3F, 0x3F},
  .tiles = {
    {0x00, 0x00, 0x70, 0x71, 0x00, 0x00},
    {0x00, 0x88, 0x89, 0x8A, 0x8B, 0x00},
    {0x88, 0x8C, 0x91, 0x92, 0x8F, 0x8B},
    {0x90, 0x94, 0x95, 0x96, 0x97, 0x9F},
    {0x90, 0x91, 0x9D, 0x9E, 0x92, 0x93},
    {0x98, 0x99, 0x9A, 0x99, 0x9A, 0x9B},
  },
};

typedef enum MountainStampKind {
  kMountainStamp_Small,
  kMountainStamp_AitosLarge,
  kMountainStamp_AitosVolcano,
} MountainStampKind;

typedef struct MountainPlacement {
  int8_t x, y;
  uint8_t stamp;
} MountainPlacement;

#define SMALL(x_, y_) {x_, y_, kMountainStamp_Small}

/* These placements are map composition data, not screen-space tuning. Each
 * table covers the town's canonical $7F:2000 mountain cells exactly. The
 * overlap metatiles are also composition evidence: they retain ridge/base
 * seams from objects whose other rows lie outside the map. Negative
 * coordinates reconstruct those clipped objects; an edge object remains a
 * half-peak only when the source composition actually ends at that edge. */
static const MountainPlacement kBloodpoolPlacements[] = {
  SMALL(2, -2), SMALL(10, -2), SMALL(18, -2),
  SMALL(0, -1), SMALL(20, -1), SMALL(24, -1), SMALL(28, -1),
  SMALL(-2, 0), SMALL(14, -3), SMALL(22, 0), SMALL(26, 0),
  SMALL(6, -3), SMALL(28, 1),
  SMALL(8, 26), SMALL(12, 28), SMALL(6, 29), SMALL(10, 29),
  SMALL(4, 30), SMALL(2, 31),
};

static const MountainPlacement kKasandoraPlacements[] = {
  /* The north edge's clean $98/$99/$9A/$9B runs pin a continuous background
   * row at x=0..24 in four-cell steps rather than the occupancy-only x=2/6
   * cover. Its fused $84/$79, $86, and $7C shoulders expose the staggered
   * rows immediately in front. */
  SMALL(0, -3), SMALL(4, -3), SMALL(8, -3), SMALL(12, -3),
  SMALL(16, -3), SMALL(20, -3), SMALL(24, -3),
  SMALL(-2, -2), SMALL(10, -2), SMALL(14, -2),
  SMALL(18, -2), SMALL(22, -2),
  SMALL(10, -1), SMALL(12, -1), SMALL(16, -1), SMALL(20, -1),
  SMALL(10, 0), SMALL(14, 0), SMALL(22, 0),
  SMALL(10, 2), SMALL(8, 3), SMALL(6, 4),
  SMALL(30, 18), SMALL(26, 20), SMALL(28, 21), SMALL(30, 22),
  SMALL(30, 26), SMALL(28, 27), SMALL(30, 28),
  SMALL(2, 28), SMALL(4, 29), SMALL(8, 29), SMALL(12, 29),
  SMALL(16, 29),
  /* The south edge's repeated $79/$86 row and $85/$86 row are the visible
   * apex halves of two complete off-map rows. An occupancy-only cover drops
   * all but their last objects because the remaining cells are overlapped. */
  SMALL(2, 30), SMALL(6, 30), SMALL(10, 30),
  SMALL(14, 30), SMALL(18, 30),
  SMALL(4, 31), SMALL(8, 31), SMALL(12, 31),
  SMALL(16, 31), SMALL(20, 31),
};

static const MountainPlacement kAitosPlacements[] = {
  SMALL(-2, 2), SMALL(2, 0), SMALL(4, -1), SMALL(8, -1),
  SMALL(10, 2), SMALL(12, -1), SMALL(18, -2), SMALL(18, 0),
  SMALL(20, -1), SMALL(24, -2), SMALL(28, -1), SMALL(30, 2),
  SMALL(30, 4), SMALL(30, 6),
  SMALL(-2, 10),
  SMALL(-2, 12),
  /* Only this stamp's left wall is in bounds; it preserves Aitos's authored
   * east-edge half mountain without inventing an off-map peak. */
  SMALL(31, 11),
  SMALL(12, 13), SMALL(28, 13), SMALL(28, 15),
  SMALL(-2, 16),
  SMALL(30, 16), SMALL(26, 20), SMALL(0, 21),
  SMALL(-2, 22),
  SMALL(26, 24), SMALL(0, 25), SMALL(24, 25), SMALL(26, 26),
  SMALL(-2, 28),
  SMALL(2, 28), SMALL(18, 28), SMALL(22, 28), SMALL(24, 29),
  SMALL(4, 29), SMALL(8, 29), SMALL(12, 29), SMALL(16, 29),
  SMALL(0, -1), SMALL(0, 1),
  {24, 2, kMountainStamp_AitosLarge},
  {6, 8, kMountainStamp_AitosVolcano},
};

static const MountainPlacement kNorthwallPlacements[] = {
  SMALL(12, 2), SMALL(10, 3), SMALL(8, 4), SMALL(12, 4),
  SMALL(26, 5), SMALL(30, 5), SMALL(12, 6), SMALL(24, 6),
  SMALL(14, 7), SMALL(16, 8), SMALL(28, 8), SMALL(28, 10),
  SMALL(0, 11), SMALL(-2, 12), SMALL(28, 12),
  SMALL(18, 15), SMALL(16, 16), SMALL(-2, 16),
  SMALL(0, 17), SMALL(-2, 20), SMALL(0, 21),
  SMALL(0, 23), SMALL(2, 24), SMALL(0, 25),
  SMALL(-2, 26),
  SMALL(20, 27), SMALL(10, 28), SMALL(18, 28), SMALL(30, 28),
  SMALL(4, 29), SMALL(12, 29), SMALL(16, 29), SMALL(24, 29),
  SMALL(2, 30), SMALL(6, 30), SMALL(22, 30), SMALL(26, 30),
};

#undef SMALL

static size_t CellIndex(int x, int y) {
  return (size_t)y * kSimBackgroundMountainTownCells + (size_t)x;
}

static uint8_t TileAt(
    const SimBackgroundMountainField *field, int x, int y) {
  if (x < 0 || x >= kSimBackgroundMountainTownCells ||
      y < 0 || y >= kSimBackgroundMountainTownCells)
    return 0;
  return field->tile[CellIndex(x, y)];
}

static bool Append(
    SimBackgroundMountainObjectList *out, const MountainStamp *stamp,
    int cell_x, int cell_y) {
  if (out->count >= kSimBackgroundMountainMaxObjects) return false;
  SimBackgroundMountainObject *object = &out->objects[out->count++];
  *object = (SimBackgroundMountainObject){
    .cell_x = (int8_t)cell_x,
    .cell_y = (int8_t)cell_y,
    .width_cells = stamp->width,
    .height_cells = stamp->height,
    .flags = stamp == &kAitosVolcano
        ? kSimBackgroundMountainObject_Volcano : 0,
  };
  memcpy(object->row_occupied_mask, stamp->rows, sizeof(stamp->rows));
  memcpy(object->source_tile, stamp->tiles, sizeof(stamp->tiles));
  return true;
}

static bool StampIsValid(const MountainStamp *stamp) {
  for (int row = 0; row < stamp->height; row++)
    for (int column = 0; column < stamp->width; column++) {
      bool occupied = (stamp->rows[row] & (1u << column)) != 0;
      uint8_t tile = stamp->tiles[row][column];
      if (occupied != (tile != 0)) return false;
    }
  return true;
}

static bool FillmoreOracleAvailable(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps) {
  if (field->town != 1 || caps->tile_count != 64) return false;
  /* Verify both the canonical lower composition and every explicit source
   * metatile. The renderer can then resolve each source through the field's
   * tile index without assuming a rectangular example is clean. */
  return StampIsValid(&kCommonSmall) &&
      StampIsValid(&kFillmoreLarge) &&
      TileAt(field, 29, 24) == 0x81 &&
      TileAt(field, 24, 24) == 0x81 &&
      TileAt(field, 25, 24) == 0x82 &&
      TileAt(field, 24, 28) == 0x9D &&
      TileAt(field, 25, 28) == 0x9E;
}

static bool AppendFillmoreNorthRange(
    SimBackgroundMountainObjectList *out) {
  /* The canonical north edge is four staggered rows of the same small
   * mountain. Its overlap metatiles obscure several peaks, so scanning for
   * $81 cannot recover the composition. These static placements were
   * validated by OR-compositing the clean stamp against the complete
   * Fillmore terrain-map silhouette: they add no out-of-range pixels. */
  for (int center_x = 0;
       center_x <= kSimBackgroundMountainTownCells; center_x += 4)
    if (!Append(out, &kCommonSmall, center_x - 2, -2)) return false;
  for (int cell_x = 0;
       cell_x < kSimBackgroundMountainTownCells; cell_x += 4)
    if (!Append(out, &kCommonSmall, cell_x, -1)) return false;
  static const int8_t kMiddleRowX[] = {-2, 2, 14, 18, 26, 30};
  for (size_t at = 0;
       at < sizeof(kMiddleRowX) / sizeof(kMiddleRowX[0]); at++)
    if (!Append(out, &kCommonSmall, kMiddleRowX[at], 0)) return false;
  if (!Append(out, &kCommonSmall, 0, 1) ||
      !Append(out, &kCommonSmall, 16, 1))
    return false;
  return true;
}

static bool IsLargeFillmorePeak(
    const SimBackgroundMountainField *field, int peak_x, int peak_y) {
  return TileAt(field, peak_x, peak_y + 4) == 0x9D &&
      TileAt(field, peak_x + 1, peak_y + 4) == 0x9E;
}

static bool AppendFillmoreInteriorPeaks(
    const SimBackgroundMountainField *field,
    SimBackgroundMountainObjectList *out) {
  for (int y = 1; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      if (TileAt(field, x, y) != 0x81) continue;
      if (IsLargeFillmorePeak(field, x, y)) {
        if (!Append(out, &kFillmoreLarge, x - 2, y)) return false;
      } else if (!Append(out, &kCommonSmall, x - 1, y)) {
        return false;
      }
    }
  return true;
}

static const MountainStamp *StampForKind(uint8_t kind) {
  switch ((MountainStampKind)kind) {
    case kMountainStamp_Small: return &kCommonSmall;
    case kMountainStamp_AitosLarge: return &kAitosLarge;
    case kMountainStamp_AitosVolcano: return &kAitosVolcano;
  }
  return NULL;
}

static bool ValidateObjectCoverage(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainObjectList *objects) {
  bool covered[kSimBackgroundMountainCellCount] = {false};
  for (int at = 0; at < objects->count; at++) {
    const SimBackgroundMountainObject *object = &objects->objects[at];
    for (int row = 0; row < object->height_cells; row++)
      for (int column = 0; column < object->width_cells; column++) {
        bool occupied =
            (object->row_occupied_mask[row] & (1u << column)) != 0;
        if (occupied != (object->source_tile[row][column] != 0)) return false;
        if (!occupied) continue;
        int x = object->cell_x + column;
        int y = object->cell_y + row;
        if (x < 0 || x >= kSimBackgroundMountainTownCells ||
            y < 0 || y >= kSimBackgroundMountainTownCells)
          continue;
        if (!SimBackgroundMountains_CellOccupied(field, x, y)) return false;
        covered[CellIndex(x, y)] = true;
      }
  }
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      if (SimBackgroundMountains_CellOccupied(field, x, y) !=
          covered[CellIndex(x, y)])
        return false;
  return true;
}

static bool BuildAuditedTown(
    const SimBackgroundMountainField *field,
    SimBackgroundMountainObjectList *out) {
  const MountainPlacement *placements = NULL;
  size_t count = 0;
  switch (field->town) {
    case 2:
      placements = kBloodpoolPlacements;
      count = sizeof(kBloodpoolPlacements) / sizeof(kBloodpoolPlacements[0]);
      break;
    case 3:
      placements = kKasandoraPlacements;
      count = sizeof(kKasandoraPlacements) /
          sizeof(kKasandoraPlacements[0]);
      break;
    case 4:
      placements = kAitosPlacements;
      count = sizeof(kAitosPlacements) / sizeof(kAitosPlacements[0]);
      break;
    case 6:
      placements = kNorthwallPlacements;
      count = sizeof(kNorthwallPlacements) /
          sizeof(kNorthwallPlacements[0]);
      break;
    default:
      return false;
  }
  if (count > kSimBackgroundMountainMaxObjects) return false;
  bool source_checked[3] = {false};
  for (size_t at = 0; at < count; at++) {
    const MountainStamp *stamp = StampForKind(placements[at].stamp);
    if (!stamp) return false;
    if (!source_checked[placements[at].stamp]) {
      if (!StampIsValid(stamp)) return false;
      source_checked[placements[at].stamp] = true;
    }
    if (!Append(out, stamp, placements[at].x, placements[at].y))
      return false;
  }
  return ValidateObjectCoverage(field, out);
}

bool SimBackgroundMountainObjects_Build(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps,
    SimBackgroundMountainObjectList *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!field || !caps) return false;
  bool valid = false;
  if (field->town == 1 && FillmoreOracleAvailable(field, caps)) {
    valid = AppendFillmoreNorthRange(out) &&
        AppendFillmoreInteriorPeaks(field, out);
  } else {
    valid = BuildAuditedTown(field, out);
  }
  if (!valid) memset(out, 0, sizeof(*out));
  return valid;
}
