#include "sim_town_terrain.h"

#include <math.h>
#include <stddef.h>

#include "sim_town_terrain_data.inc"

static const float kEdgeIntersectionTolerance = 0.000001f;
static const float kVisibleEdgeHeightEpsilonUnits = 0.004f;

typedef struct SimTownTerrainCellHeights {
  float nw, ne, se, sw;
} SimTownTerrainCellHeights;

static int ValidCellCorner(uint8_t town, int x, int y, int corner) {
  return town >= 1 && town <= kSimTownTerrainTownCount &&
      x >= 0 && x < kSimTownTerrainCells &&
      y >= 0 && y < kSimTownTerrainCells &&
      corner >= 0 && corner < kSimTownTerrainCornerCount;
}

static float ClampUnit(float value) {
  if (value < 0.0f) return 0.0f;
  return value > 1.0f ? 1.0f : value;
}

static float Q8ToUnits(int16_t height_q8) {
  return (float)height_q8 / (float)kSimTownTerrainQ8Scale;
}

static SimTownTerrainCellHeights CellHeights(
    uint8_t town, int cell_x, int cell_y) {
  const int16_t *corner =
      kSimTownTerrainCornerQ8[town - 1][cell_y][cell_x];
  return (SimTownTerrainCellHeights){
    .nw = Q8ToUnits(corner[kSimTownTerrainCornerNW]),
    .ne = Q8ToUnits(corner[kSimTownTerrainCornerNE]),
    .se = Q8ToUnits(corner[kSimTownTerrainCornerSE]),
    .sw = Q8ToUnits(corner[kSimTownTerrainCornerSW]),
  };
}

static float BilinearHeight(
    SimTownTerrainCellHeights height, float u, float v) {
  const float north = height.nw + (height.ne - height.nw) * u;
  const float south = height.sw + (height.se - height.sw) * u;
  return north + (south - north) * v;
}

static int ResolvePoint(
    uint8_t town, float pixel_x, float pixel_y,
    int *out_cell_x, int *out_cell_y, float *out_u, float *out_v) {
  if (town < 1 || town > kSimTownTerrainTownCount ||
      !isfinite(pixel_x) || !isfinite(pixel_y) ||
      !out_cell_x || !out_cell_y || !out_u || !out_v)
    return 0;
  const float extent =
      (float)(kSimTownTerrainCells * kSimTownTerrainCellPixels);
  if (pixel_x < 0.0f) pixel_x = 0.0f;
  if (pixel_y < 0.0f) pixel_y = 0.0f;
  if (pixel_x > extent) pixel_x = extent;
  if (pixel_y > extent) pixel_y = extent;
  const float cell_xf = pixel_x / (float)kSimTownTerrainCellPixels;
  const float cell_yf = pixel_y / (float)kSimTownTerrainCellPixels;
  /* Clamping made both values non-negative, so integer conversion is the
   * floor operation we need without calling libm twice for every actor,
   * foundation, decal, and mountain vertex. */
  int cell_x = (int)cell_xf;
  int cell_y = (int)cell_yf;
  if (cell_x >= kSimTownTerrainCells) cell_x = kSimTownTerrainCells - 1;
  if (cell_y >= kSimTownTerrainCells) cell_y = kSimTownTerrainCells - 1;
  *out_cell_x = cell_x;
  *out_cell_y = cell_y;
  *out_u = cell_xf - cell_x;
  *out_v = cell_yf - cell_y;
  return 1;
}

/* Height is the overwhelmingly common runtime query. Keep it independent of
 * SimTownTerrainSample so actor placement, foundations and bridge audits do
 * not pay for two slopes, a square root and three normalized components they
 * immediately discard. The full sampler below uses the same four source
 * values, preserving one interpolation contract. */
static int SampleCellHeightUnits(
    uint8_t town, int cell_x, int cell_y,
    float u, float v, float *out_height_units) {
  if (!out_height_units) return 0;
  *out_height_units = 0.0f;
  if (!ValidCellCorner(town, cell_x, cell_y, 0) ||
      !isfinite(u) || !isfinite(v))
    return 0;
  u = ClampUnit(u);
  v = ClampUnit(v);
  *out_height_units = BilinearHeight(CellHeights(town, cell_x, cell_y), u, v);
  return 1;
}

int16_t SimTownTerrain_CornerQ8(uint8_t town, int cell_x, int cell_y,
                                int corner) {
  if (!ValidCellCorner(town, cell_x, cell_y, corner)) return 0;
  return kSimTownTerrainCornerQ8[town - 1][cell_y][cell_x][corner];
}

float SimTownTerrain_CornerUnits(uint8_t town, int cell_x, int cell_y,
                                 int corner) {
  if (!ValidCellCorner(town, cell_x, cell_y, corner)) return 0.0f;
  return Q8ToUnits(
      kSimTownTerrainCornerQ8[town - 1][cell_y][cell_x][corner]);
}

float SimTownTerrain_CellUnits(uint8_t town, int cell_x, int cell_y) {
  if (!ValidCellCorner(town, cell_x, cell_y, 0)) return 0.0f;
  const int16_t *corner =
      kSimTownTerrainCornerQ8[town - 1][cell_y][cell_x];
  int total = 0;
  for (int at = 0; at < kSimTownTerrainCornerCount; at++)
    total += corner[at];
  return (float)total /
      (float)(kSimTownTerrainCornerCount * kSimTownTerrainQ8Scale);
}

uint8_t SimTownTerrain_HardEdges(uint8_t town, int cell_x, int cell_y) {
  if (!ValidCellCorner(town, cell_x, cell_y, 0)) return 0;
  return kSimTownTerrainHardEdges[town - 1][cell_y][cell_x];
}

SimTownTerrainFaceKind SimTownTerrain_FaceKind(
    uint8_t town, int cell_x, int cell_y) {
  if (!ValidCellCorner(town, cell_x, cell_y, 0))
    return kSimTownTerrainFace_None;
  return (SimTownTerrainFaceKind)
      kSimTownTerrainFaceCell[town - 1][cell_y][cell_x];
}

int SimTownTerrain_IsFaceCell(uint8_t town, int cell_x, int cell_y) {
  return SimTownTerrain_FaceKind(town, cell_x, cell_y) !=
      kSimTownTerrainFace_None;
}

int SimTownTerrain_IsCaveCell(uint8_t town, int cell_x, int cell_y) {
  return SimTownTerrain_FaceKind(town, cell_x, cell_y) ==
      kSimTownTerrainFace_Cave;
}

float SimTownTerrain_MaximumUnits(uint8_t town) {
  if (town < 1 || town > kSimTownTerrainTownCount) return 0.0f;
  return Q8ToUnits(kSimTownTerrainMaximumQ8[town - 1]);
}

int SimTownTerrain_SampleCell(uint8_t town, int cell_x, int cell_y,
                              float u, float v,
                              SimTownTerrainSample *out) {
  if (out == NULL) return 0;
  *out = (SimTownTerrainSample){0};
  if (!ValidCellCorner(town, cell_x, cell_y, 0) ||
      !isfinite(u) || !isfinite(v))
    return 0;
  u = ClampUnit(u);
  v = ClampUnit(v);

  const SimTownTerrainCellHeights height =
      CellHeights(town, cell_x, cell_y);
  out->height_units = BilinearHeight(height, u, v);
  out->slope_x = (height.ne - height.nw) * (1.0f - v) +
      (height.se - height.sw) * v;
  out->slope_y = (height.sw - height.nw) * (1.0f - u) +
      (height.se - height.ne) * u;
  const float normal_length = sqrtf(
      out->slope_x * out->slope_x + out->slope_y * out->slope_y + 1.0f);
  out->normal_x = -out->slope_x / normal_length;
  out->normal_y = -out->slope_y / normal_length;
  out->normal_z = 1.0f / normal_length;
  out->hard_edges = kSimTownTerrainHardEdges[town - 1][cell_y][cell_x];
  return 1;
}

int SimTownTerrain_SamplePoint(uint8_t town, float pixel_x, float pixel_y,
                               SimTownTerrainSample *out) {
  if (out == NULL) return 0;
  *out = (SimTownTerrainSample){0};
  int cell_x, cell_y;
  float u, v;
  if (!ResolvePoint(
          town, pixel_x, pixel_y, &cell_x, &cell_y, &u, &v))
    return 0;
  return SimTownTerrain_SampleCell(town, cell_x, cell_y, u, v, out);
}

int SimTownTerrain_LevelPairUnits(
    uint8_t town,
    int cell_a_x, int cell_a_y, float a_u, float a_v,
    int cell_b_x, int cell_b_y, float b_u, float b_v,
    float *out_height_units) {
  if (!out_height_units) return 0;
  *out_height_units = 0.0f;
  float a, b;
  if (!SampleCellHeightUnits(
          town, cell_a_x, cell_a_y, a_u, a_v, &a) ||
      !SampleCellHeightUnits(
          town, cell_b_x, cell_b_y, b_u, b_v, &b))
    return 0;
  *out_height_units = fmaxf(a, b);
  return 1;
}

int SimTownTerrain_MaximumUnitsInRect(
    uint8_t town, float pixel_x0, float pixel_y0,
    float pixel_x1, float pixel_y1, float *out_height_units) {
  if (!out_height_units) return 0;
  *out_height_units = 0.0f;
  if (town < 1 || town > kSimTownTerrainTownCount ||
      !isfinite(pixel_x0) || !isfinite(pixel_y0) ||
      !isfinite(pixel_x1) || !isfinite(pixel_y1) ||
      pixel_x1 < pixel_x0 || pixel_y1 < pixel_y0)
    return 0;
  const float extent =
      (float)(kSimTownTerrainCells * kSimTownTerrainCellPixels);
  if (pixel_x0 < 0.0f) pixel_x0 = 0.0f;
  if (pixel_y0 < 0.0f) pixel_y0 = 0.0f;
  if (pixel_x1 > extent) pixel_x1 = extent;
  if (pixel_y1 > extent) pixel_y1 = extent;
  if (pixel_x0 > extent || pixel_y0 > extent ||
      pixel_x1 < 0.0f || pixel_y1 < 0.0f)
    return 0;

  int first_x = (int)(pixel_x0 / (float)kSimTownTerrainCellPixels);
  int first_y = (int)(pixel_y0 / (float)kSimTownTerrainCellPixels);
  int last_x = (int)(pixel_x1 / (float)kSimTownTerrainCellPixels);
  int last_y = (int)(pixel_y1 / (float)kSimTownTerrainCellPixels);
  if (first_x >= kSimTownTerrainCells) first_x = kSimTownTerrainCells - 1;
  if (first_y >= kSimTownTerrainCells) first_y = kSimTownTerrainCells - 1;
  if (last_x >= kSimTownTerrainCells) last_x = kSimTownTerrainCells - 1;
  if (last_y >= kSimTownTerrainCells) last_y = kSimTownTerrainCells - 1;

  float maximum = -INFINITY;
  for (int cell_y = first_y; cell_y <= last_y; cell_y++)
    for (int cell_x = first_x; cell_x <= last_x; cell_x++) {
      const float cell_x0 = cell_x * (float)kSimTownTerrainCellPixels;
      const float cell_y0 = cell_y * (float)kSimTownTerrainCellPixels;
      float u0 = (fmaxf(pixel_x0, cell_x0) - cell_x0) /
          (float)kSimTownTerrainCellPixels;
      float v0 = (fmaxf(pixel_y0, cell_y0) - cell_y0) /
          (float)kSimTownTerrainCellPixels;
      float u1 = (fminf(
          pixel_x1, cell_x0 + kSimTownTerrainCellPixels) - cell_x0) /
          (float)kSimTownTerrainCellPixels;
      float v1 = (fminf(
          pixel_y1, cell_y0 + kSimTownTerrainCellPixels) - cell_y0) /
          (float)kSimTownTerrainCellPixels;
      /* A bilinear patch has no interior extrema, so its clipped rectangle's
       * four corners are the complete maximum candidate set. */
      const float u[2] = {u0, u1}, v[2] = {v0, v1};
      for (int vy = 0; vy < 2; vy++)
        for (int ux = 0; ux < 2; ux++) {
          float height;
          if (SampleCellHeightUnits(
                  town, cell_x, cell_y, u[ux], v[vy], &height) &&
              height > maximum)
            maximum = height;
        }
    }
  if (!isfinite(maximum)) return 0;
  *out_height_units = maximum;
  return 1;
}

int SimTownTerrain_ClipHigherEdge(
    float current_0, float current_1,
    float neighbour_0, float neighbour_1,
    float epsilon, float *out_t0, float *out_t1) {
  if (!out_t0 || !out_t1 ||
      !isfinite(current_0) || !isfinite(current_1) ||
      !isfinite(neighbour_0) || !isfinite(neighbour_1) ||
      !isfinite(epsilon))
    return 0;
  if (epsilon < 0.0f) epsilon = -epsilon;
  *out_t0 = 0.0f;
  *out_t1 = 0.0f;
  const float difference_0 = current_0 - neighbour_0;
  const float difference_1 = current_1 - neighbour_1;
  if (difference_0 <= epsilon && difference_1 <= epsilon) return 0;

  /* No sign reversal means the whole edge belongs to this side.  A zero
   * endpoint deliberately remains: it closes as a triangle without opening a
   * sub-pixel crack at the meeting vertex. */
  if (difference_0 >= 0.0f && difference_1 >= 0.0f) {
    *out_t1 = 1.0f;
    return 1;
  }

  const float denominator = difference_0 - difference_1;
  if (fabsf(denominator) < kEdgeIntersectionTolerance) return 0;
  float crossing = difference_0 / denominator;
  if (crossing < 0.0f) crossing = 0.0f;
  if (crossing > 1.0f) crossing = 1.0f;
  if (difference_0 > epsilon) {
    *out_t1 = crossing;
  } else {
    *out_t0 = crossing;
    *out_t1 = 1.0f;
  }
  return *out_t1 - *out_t0 > kEdgeIntersectionTolerance;
}

int SimTownTerrain_ClipVisibleHigherEdge(
    float current_0, float current_1,
    float neighbour_0, float neighbour_1,
    float *out_t0, float *out_t1) {
  return SimTownTerrain_ClipHigherEdge(
      current_0, current_1, neighbour_0, neighbour_1,
      kVisibleEdgeHeightEpsilonUnits, out_t0, out_t1);
}

float SimTownTerrain_HeightUnitsAt(uint8_t town, float pixel_x,
                                   float pixel_y) {
  int cell_x, cell_y;
  float u, v;
  if (!ResolvePoint(
          town, pixel_x, pixel_y, &cell_x, &cell_y, &u, &v))
    return 0.0f;
  float height;
  return SampleCellHeightUnits(
      town, cell_x, cell_y, u, v, &height) ? height : 0.0f;
}
