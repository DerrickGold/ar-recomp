#include "sim_world_navigation_terrain.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "sim_town_terrain.h"
#include "sim_world_map.h"

static const float kTownBorderBlendTiles = 4.0f;
static const float kWorldPlainHeight = 4.0f;
/* Navigation registration, not changes to the town's own terrain data.
 * Fillmore already supplies the reference plain; Bloodpool's plain is 1,
 * and Kasandora/Aitos/Northwall start at 0. Marahna keeps its authored 6-unit
 * plateau. Raise low plains without flattening genuine local contours. */
static const float kTownDatumOffset[kSimTownCount] = {0, 3, 4, 4, 0, 4};
static const float kSlopeSampleStepTiles = 0.5f;
static const float kMountainBaseRise = 0.60f;
static const float kMountainRisePerTile = 0.65f;
static const float kMountainDistanceLimit = 5.0f;

enum {
  kWorldCells = kSimWorldMapTiles,
  kWorldAxis = kWorldCells + 1,
  kWorldCellCount = kWorldCells * kWorldCells,
  kWorldVertexCount = kWorldAxis * kWorldAxis,
  kSemanticSmoothPasses = 5,
};

typedef struct WorldVisualCell {
  float land;
  float vegetation;
  float coast_distance;
  float rock;
  float rock_distance;
} WorldVisualCell;

static WorldVisualCell s_visual[kWorldCellCount];
static float s_semantic_height[kWorldVertexCount];
static float s_mountain_height[kWorldVertexCount];
static float s_mountain_replacement[kWorldVertexCount];
static float s_mountain_transition[kWorldVertexCount];
static bool s_mountain_transition_enabled;
static float s_mountain_join_target[kWorldVertexCount];
static float s_mountain_join_weight[kWorldVertexCount];
static bool s_mountain_join_enabled;
static float s_mountain_limit_target[kWorldVertexCount];
static float s_mountain_limit_weight[kWorldVertexCount];
static bool s_mountain_limit_enabled;
static float s_coast_weight[kWorldVertexCount];
static uint32_t s_world_prior_serial;

static float Clamp(float value, float low, float high) {
  return value < low ? low : value > high ? high : value;
}

static float Smoothstep(float value) {
  value = Clamp(value, 0.0f, 1.0f);
  return value * value * (3.0f - 2.0f * value);
}

static int CellIndex(int x, int y) {
  return y * kWorldCells + x;
}

static int VertexIndex(int x, int y) {
  return y * kWorldAxis + x;
}

static float GridHeightAt(const float *grid, float tile_x, float tile_y) {
  if (!s_world_prior_serial) return 0.0f;
  tile_x = Clamp(tile_x, 0.0f, kWorldCells);
  tile_y = Clamp(tile_y, 0.0f, kWorldCells);
  const int x0 = (int)tile_x;
  const int y0 = (int)tile_y;
  const int x1 = x0 < kWorldCells ? x0 + 1 : x0;
  const int y1 = y0 < kWorldCells ? y0 + 1 : y0;
  const float u = tile_x - x0;
  const float v = tile_y - y0;
  const float north = grid[VertexIndex(x0, y0)] +
      (grid[VertexIndex(x1, y0)] - grid[VertexIndex(x0, y0)]) * u;
  const float south = grid[VertexIndex(x0, y1)] +
      (grid[VertexIndex(x1, y1)] - grid[VertexIndex(x0, y1)]) * u;
  return north + (south - north) * v;
}

static WorldVisualCell ClassifyWorldCell(
    const uint32_t *pixels, int pitch_pixels, int cell_x, int cell_y) {
  float blue = 0.0f, green = 0.0f;
  for (int y = 0; y < kSimWorldMapTilePixels; y++) {
    const uint32_t *row = pixels +
        (size_t)(cell_y * kSimWorldMapTilePixels + y) * pitch_pixels +
        cell_x * kSimWorldMapTilePixels;
    for (int x = 0; x < kSimWorldMapTilePixels; x++) {
      const uint32_t pixel = row[x];
      const float red = (float)((pixel >> 16) & 0xFF);
      const float value_green = (float)((pixel >> 8) & 0xFF);
      const float value_blue = (float)(pixel & 0xFF);
      if (value_blue > 48.0f && value_blue > red * 1.16f &&
          value_blue > value_green * 1.04f && value_blue - red > 18.0f)
        blue += 1.0f;
      if (value_green > 42.0f && value_green > red * 1.06f &&
          value_green > value_blue * 0.82f)
        green += 1.0f;
    }
  }
  const float samples =
      (float)(kSimWorldMapTilePixels * kSimWorldMapTilePixels);
  const float water = Smoothstep((blue / samples - 0.24f) / 0.58f);
  const float rock = SimWorldMap_MountainCoverage(cell_x, cell_y);
  return (WorldVisualCell){
    .land = 1.0f - water,
    .vegetation = green / samples,
    .coast_distance = water > 0.58f ? 0.0f : 10000.0f,
    .rock = rock,
    .rock_distance = rock >= 0.18f ? 10000.0f : 0.0f,
  };
}

static void SeedOceanDistance(void) {
  /* Flood only broad water connected to the map boundary. Narrow river tiles
   * do not propagate the ocean datum into inland lakes or carve a new canyon
   * through a raised town. A one-cell water fringe restores the shoreline. */
  bool broad[kWorldCellCount] = {false};
  bool ocean[kWorldCellCount] = {false};
  uint16_t queue[kWorldCellCount];
  int read = 0, write = 0;
  for (int y = 0; y < kWorldCells; y++)
    for (int x = 0; x < kWorldCells; x++) {
      if (s_visual[CellIndex(x, y)].land > 0.42f) continue;
      int water = 0;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= kWorldCells || ny >= kWorldCells ||
              s_visual[CellIndex(nx, ny)].land <= 0.42f)
            water++;
        }
      const int at = CellIndex(x, y);
      broad[at] = water >= 6;
      if (broad[at] &&
          (x == 0 || y == 0 || x == kWorldCells - 1 || y == kWorldCells - 1)) {
        ocean[at] = true;
        queue[write++] = (uint16_t)at;
      }
    }
  static const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  while (read < write) {
    const int at = queue[read++];
    const int x = at % kWorldCells, y = at / kWorldCells;
    for (int edge = 0; edge < 4; edge++) {
      const int nx = x + offsets[edge][0], ny = y + offsets[edge][1];
      if (nx < 0 || ny < 0 || nx >= kWorldCells || ny >= kWorldCells)
        continue;
      const int next = CellIndex(nx, ny);
      if (!broad[next] || ocean[next]) continue;
      ocean[next] = true;
      queue[write++] = (uint16_t)next;
    }
  }
  for (int y = 0; y < kWorldCells; y++)
    for (int x = 0; x < kWorldCells; x++) {
      const int at = CellIndex(x, y);
      bool sea = ocean[at];
      if (s_visual[at].land <= 0.42f)
        for (int edge = 0; edge < 4; edge++) {
          const int nx = x + offsets[edge][0], ny = y + offsets[edge][1];
          if (nx >= 0 && ny >= 0 && nx < kWorldCells && ny < kWorldCells &&
              ocean[CellIndex(nx, ny)])
            sea = true;
        }
      s_visual[at].coast_distance = sea ? 0.0f : 10000.0f;
    }
}

static void BuildCoastDistance(void) {
  const float diagonal = 1.41421356237f;
  for (int y = 0; y < kWorldCells; y++) {
    for (int x = 0; x < kWorldCells; x++) {
      WorldVisualCell *cell = &s_visual[CellIndex(x, y)];
      if (x > 0)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x - 1, y)].coast_distance + 1.0f);
      if (y > 0)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x, y - 1)].coast_distance + 1.0f);
      if (x > 0 && y > 0)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x - 1, y - 1)].coast_distance + diagonal);
      if (x + 1 < kWorldCells && y > 0)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x + 1, y - 1)].coast_distance + diagonal);
    }
  }
  for (int y = kWorldCells - 1; y >= 0; y--) {
    for (int x = kWorldCells - 1; x >= 0; x--) {
      WorldVisualCell *cell = &s_visual[CellIndex(x, y)];
      if (x + 1 < kWorldCells)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x + 1, y)].coast_distance + 1.0f);
      if (y + 1 < kWorldCells)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x, y + 1)].coast_distance + 1.0f);
      if (x + 1 < kWorldCells && y + 1 < kWorldCells)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x + 1, y + 1)].coast_distance + diagonal);
      if (x > 0 && y + 1 < kWorldCells)
        cell->coast_distance = fminf(
            cell->coast_distance,
            s_visual[CellIndex(x - 1, y + 1)].coast_distance + diagonal);
    }
  }
}

static float VisualCellHeight(int x, int y) {
  x = x < 0 ? 0 : x >= kWorldCells ? kWorldCells - 1 : x;
  y = y < 0 ? 0 : y >= kWorldCells ? kWorldCells - 1 : y;
  const WorldVisualCell *cell = &s_visual[CellIndex(x, y)];
  const float inland = Clamp(cell->coast_distance / 10.0f, 0.0f, 1.0f);
  /* Broad lowlands, not a luminance-variance mountain detector: snow plains,
   * roads and roof pixels have just as much contrast as exposed rock. */
  return kWorldPlainHeight + inland * 0.12f + cell->vegetation * 0.08f;
}

static void BuildMountainRelief(void) {
  /* Distance within the actual rock silhouette produces connected crests,
   * shoulders and saddles rather than one cone per source tile. Keep this
   * separate from town base elevation: town mountains are a separate render
   * layer and deliberately absent from SimTownTerrain's floor heightfield. */
  for (int pass = 0; pass < 2; pass++) {
    const int step = pass ? -1 : 1;
    for (int y = pass ? kWorldCells - 1 : 0;
         y >= 0 && y < kWorldCells; y += step)
      for (int x = pass ? kWorldCells - 1 : 0;
           x >= 0 && x < kWorldCells; x += step) {
        WorldVisualCell *cell = &s_visual[CellIndex(x, y)];
        const int offsets[4][2] = {
          {-step, 0}, {0, -step}, {-step, -step}, {step, -step},
        };
        for (int neighbour = 0; neighbour < 4; neighbour++) {
          const int nx = x + offsets[neighbour][0];
          const int ny = y + offsets[neighbour][1];
          if (nx < 0 || ny < 0 || nx >= kWorldCells || ny >= kWorldCells)
            continue;
          const float distance = s_visual[CellIndex(nx, ny)].rock_distance +
              (neighbour < 2 ? 1.0f : 1.41421356f);
          cell->rock_distance = fminf(cell->rock_distance, distance);
        }
      }
  }
  for (int y = 0; y <= kWorldCells; y++)
    for (int x = 0; x <= kWorldCells; x++) {
      float total = 0.0f;
      int count = 0;
      for (int dy = -1; dy <= 0; dy++)
        for (int dx = -1; dx <= 0; dx++) {
          const int cx = x + dx, cy = y + dy;
          if (cx < 0 || cy < 0 || cx >= kWorldCells || cy >= kWorldCells)
            continue;
          const WorldVisualCell *cell = &s_visual[CellIndex(cx, cy)];
          total += Smoothstep(cell->rock / 0.65f) *
              (kMountainBaseRise + fminf(cell->rock_distance, kMountainDistanceLimit) *
                  kMountainRisePerTile);
          count++;
        }
      s_mountain_height[VertexIndex(x, y)] = count ? total / count : 0.0f;
    }
}

float SimWorldNavigationTerrain_MaxMountainRise(void) {
  return kMountainBaseRise + kMountainDistanceLimit * kMountainRisePerTile;
}

bool SimWorldNavigationTerrain_RebuildWorldPrior(
    const uint32_t *pixels, int pitch_pixels, uint32_t serial) {
  if (!pixels || pitch_pixels < kSimWorldMapPixels || !serial) return false;
  if (serial == s_world_prior_serial) return true;
  for (int y = 0; y < kWorldCells; y++)
    for (int x = 0; x < kWorldCells; x++)
      s_visual[CellIndex(x, y)] = ClassifyWorldCell(
          pixels, pitch_pixels, x, y);
  SeedOceanDistance();
  BuildCoastDistance();
  BuildMountainRelief();

  float current[kWorldVertexCount];
  float next[kWorldVertexCount];
  for (int y = 0; y <= kWorldCells; y++) {
    for (int x = 0; x <= kWorldCells; x++) {
      float total = 0.0f;
      float coast = 0.0f;
      int count = 0;
      for (int dy = -1; dy <= 0; dy++)
        for (int dx = -1; dx <= 0; dx++) {
          const int cell_x = x + dx, cell_y = y + dy;
          if (cell_x < 0 || cell_y < 0 ||
              cell_x >= kWorldCells || cell_y >= kWorldCells)
            continue;
          total += VisualCellHeight(cell_x, cell_y);
          coast += Smoothstep(
              s_visual[CellIndex(cell_x, cell_y)].coast_distance / 4.0f);
          count++;
        }
      current[VertexIndex(x, y)] = count ? total / count : 0.0f;
      s_coast_weight[VertexIndex(x, y)] = count ? coast / count : 0.0f;
    }
  }
  /* Smooth lowland material noise independently of the mountain silhouette.
   * The coastal envelope is applied after this filter so ocean stays pinned. */
  for (int pass = 0; pass < kSemanticSmoothPasses; pass++) {
    for (int y = 0; y <= kWorldCells; y++) {
      for (int x = 0; x <= kWorldCells; x++) {
        float neighbours = 0.0f;
        int count = 0;
        if (x > 0) { neighbours += current[VertexIndex(x - 1, y)]; count++; }
        if (x < kWorldCells) {
          neighbours += current[VertexIndex(x + 1, y)]; count++;
        }
        if (y > 0) { neighbours += current[VertexIndex(x, y - 1)]; count++; }
        if (y < kWorldCells) {
          neighbours += current[VertexIndex(x, y + 1)]; count++;
        }
        const float source = current[VertexIndex(x, y)];
        next[VertexIndex(x, y)] = count
            ? source * 0.55f + neighbours / count * 0.45f
            : source;
      }
    }
    memcpy(current, next, sizeof(current));
  }
  memcpy(s_semantic_height, current, sizeof(s_semantic_height));
  s_world_prior_serial = serial;
  return true;
}

uint32_t SimWorldNavigationTerrain_WorldPriorSerial(void) {
  return s_world_prior_serial;
}

static bool TownHeightAt(uint8_t town, float tile_x, float tile_y,
                         float *height, float *weight) {
  int origin_x = 0, origin_y = 0;
  if (!SimWorldMap_OriginForTown(town, &origin_x, &origin_y)) return false;
  const float local_x = tile_x - (float)origin_x;
  const float local_y = tile_y - (float)origin_y;
  /* Outside the complete four-cell feather, this town's weight is exactly
   * zero. Avoid resolving/clamping its unrelated terrain cell on every
   * globe, cliff and mountain query.
   * Keep the original arithmetic for every nonzero contributor. */
  if (local_x <= -kTownBorderBlendTiles ||
      local_y <= -kTownBorderBlendTiles ||
      local_x >= kSimTownCells + kTownBorderBlendTiles ||
      local_y >= kSimTownCells + kTownBorderBlendTiles)
    return false;
  const float clamped_x = Clamp(local_x, 0.0f, kSimTownCells);
  const float clamped_y = Clamp(local_y, 0.0f, kSimTownCells);
  if (weight) {
    const float span = kTownBorderBlendTiles * 2.0f;
    *weight = Smoothstep((local_x + kTownBorderBlendTiles) / span) *
        Smoothstep((kSimTownCells - local_x + kTownBorderBlendTiles) / span) *
        Smoothstep((local_y + kTownBorderBlendTiles) / span) *
        Smoothstep((kSimTownCells - local_y + kTownBorderBlendTiles) / span);
  }
  if (height) {
    *height = SimTownTerrain_HeightUnitsAt(
        town, clamped_x * kSimTownCellPixels,
        clamped_y * kSimTownCellPixels) + kTownDatumOffset[town - 1];
  }
  return true;
}

static float FloorOwned(float tile_x, float tile_y, float *authored_weight,
                        uint8_t owner, int cell_x, int cell_y,
                        const float *owned_height) {
  tile_x = Clamp(tile_x, 0.0f, kSimWorldMapTiles);
  tile_y = Clamp(tile_y, 0.0f, kSimWorldMapTiles);

  /* Grade both sides of boundaries, including touching town windows. Exact
   * selection inside each rectangle leaves a vertical jump even if values
   * happen to be averaged at the single shared edge vertex. */
  float weighted_height = 0.0f;
  float weight_total = 0.0f;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    float height = 0.0f, weight = 0.0f;
    if (!TownHeightAt(
            town, tile_x, tile_y, &height, &weight))
      continue;
    if (town == owner) {
      int ox, oy;
      SimTownTerrainSample sample;
      if (owned_height) height = *owned_height + kTownDatumOffset[town - 1];
      else if (SimWorldMap_OriginForTown(town, &ox, &oy) &&
          SimTownTerrain_SampleCell(town, cell_x, cell_y,
              tile_x - ox - cell_x, tile_y - oy - cell_y, &sample))
        height = sample.height_units + kTownDatumOffset[town - 1];
    }
    weighted_height += height * weight;
    weight_total += weight;
  }
  const float influence = Clamp(weight_total, 0.0f, 1.0f);
  if (authored_weight) *authored_weight = influence;
  const float inferred = s_world_prior_serial
      ? GridHeightAt(s_semantic_height, tile_x, tile_y) : kWorldPlainHeight;
  const float town_height = weight_total > 0.0f
      ? weighted_height / weight_total : inferred;
  const float coast = s_world_prior_serial
      ? GridHeightAt(s_coast_weight, tile_x, tile_y) : 1.0f;
  /* The coast prior estimates otherwise unknown terrain. It must not
   * multiply the authored town contribution: doing so carves false valleys
   * through level coastal roads and bends lowland towns toward the sea.
   * Native data already owns coastal slopes, water floors and hard cliffs.
   * Keep that registered shape intact, grading to the inferred ocean only
   * as town ownership falls away outside its boundary. */
  return town_height * influence + inferred * (1.0f - influence) * coast;
}

static float HeightOwned(float tile_x, float tile_y, float *authored_weight,
                         uint8_t owner, int cell_x, int cell_y, float *floor_height) {
  const float floor = FloorOwned(tile_x, tile_y, authored_weight, owner, cell_x, cell_y, NULL);
  if (floor_height) *floor_height = floor;
  float mountain = GridHeightAt(s_mountain_height, tile_x, tile_y) *
          (1.0f - GridHeightAt(s_mountain_replacement, tile_x, tile_y)) *
          (s_mountain_transition_enabled
              ? GridHeightAt(s_mountain_transition, tile_x, tile_y) : 1.0f);
  if (s_mountain_join_enabled)
    mountain = mountain * (1 - GridHeightAt(s_mountain_join_weight, tile_x, tile_y)) +
        GridHeightAt(s_mountain_join_target, tile_x, tile_y);
  if (s_mountain_limit_enabled) {
    const float limited = mountain * (1 - GridHeightAt(s_mountain_limit_weight, tile_x, tile_y)) +
        GridHeightAt(s_mountain_limit_target, tile_x, tile_y);
    mountain = fminf(mountain, limited);
  }
  return floor + mountain;
}

static float HeightOnly(float tile_x, float tile_y, float *authored_weight) {
  return HeightOwned(tile_x, tile_y, authored_weight, 0, 0, 0, NULL);
}

float SimWorldNavigationTerrain_FloorHeightUnits(float tile_x, float tile_y) {
  if (!isfinite(tile_x) || !isfinite(tile_y)) return 0;
  /* Native mountain anchors do not consume inferred rise. Keep this explicit:
   * the full sampler requests both floor and total height from HeightOwned. */
  return FloorOwned(tile_x, tile_y, NULL, 0, 0, 0, NULL);
}

bool SimWorldNavigationTerrain_RegisterTownFloor(uint8_t town,
    float local_x, float local_y, float height, float *out) {
  int ox, oy;
  if (!out || !SimWorldMap_OriginForTown(town,&ox,&oy) ||
      !isfinite(local_x) || !isfinite(local_y) || !isfinite(height) ||
      local_x < 0 || local_x > kSimTownCells || local_y < 0 || local_y > kSimTownCells)
    return false;
  *out = FloorOwned(ox+local_x,oy+local_y,NULL,town,0,0,&height);
  return true;
}

bool SimWorldNavigationTerrain_TownCellCorners(
    uint8_t town, int cell_x, int cell_y, float height[4]) {
  int ox, oy;
  if (!height || !SimWorldMap_OriginForTown(town, &ox, &oy) ||
      cell_x < 0 || cell_y < 0 || cell_x >= kSimTownCells || cell_y >= kSimTownCells)
    return false;
  static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
  for (int p = 0; p < 4; p++)
    height[p] = HeightOwned(ox + cell_x + dx[p], oy + cell_y + dy[p],
        NULL, town, cell_x, cell_y, NULL);
  return true;
}

void SimWorldNavigationTerrain_SetMountainTransition(const float *ridge_scale) {
  s_mountain_transition_enabled = ridge_scale != NULL;
  if (!ridge_scale) return;
  for (int i = 0; i < kWorldVertexCount; i++)
    s_mountain_transition[i] = isfinite(ridge_scale[i])
        ? Clamp(ridge_scale[i], 0, 1) : 1;
}

static bool CopyMountainConstraint(const float *rise, const float *weight,
                                   float native_to_relief, float *targets, float *weights) {
  if (!rise || !weight || !isfinite(native_to_relief) || native_to_relief <= 0) return false;
  for (int i = 0; i < kWorldVertexCount; i++) {
    const float target = rise[i] * native_to_relief;
    const float w = isfinite(target) && target >= 0 && isfinite(weight[i])
        ? Clamp(weight[i], 0, 1) : 0;
    weights[i] = w;
    targets[i] = w > 0 ? target * w : 0;
  }
  return true;
}

void SimWorldNavigationTerrain_SetMountainJoin(
    const float *rise, const float *weight, float native_to_relief) {
  s_mountain_join_enabled = CopyMountainConstraint(rise, weight, native_to_relief,
      s_mountain_join_target, s_mountain_join_weight);
}

void SimWorldNavigationTerrain_SetMountainContinuationLimit(
    const float *rise, const float *weight, float native_to_relief) {
  s_mountain_limit_enabled = CopyMountainConstraint(rise, weight, native_to_relief,
      s_mountain_limit_target, s_mountain_limit_weight);
}

void SimWorldNavigationTerrain_SetMountainReplacement(const uint8_t *cells) {
  memset(s_mountain_replacement, 0, sizeof(s_mountain_replacement));
  if (!cells) return;
  /* Native source silhouettes lean into their footprint. Remove the old
   * colour-derived peak beneath the entire source and a one-cell shoulder,
   * then feather to neighbouring inferred ranges at the outer vertices. */
  for (int y = 0; y <= kWorldCells; y++)
    for (int x = 0; x <= kWorldCells; x++) {
      unsigned owned = 0;
      for (int corner_y = -1; corner_y <= 0; corner_y++)
        for (int corner_x = -1; corner_x <= 0; corner_x++) {
          bool near = false;
          for (int dy = -1; dy <= 1 && !near; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              const int cx = x + corner_x + dx, cy = y + corner_y + dy;
              if (cx >= 0 && cy >= 0 && cx < kWorldCells && cy < kWorldCells &&
                  cells[CellIndex(cx, cy)]) { near = true; break; }
            }
          owned += near ? 1u : 0u;
        }
      s_mountain_replacement[VertexIndex(x, y)] = owned * 0.25f;
    }
}

bool SimWorldNavigationTerrain_SampleHeights(
    float tile_x, float tile_y,
    SimWorldNavigationTerrainHeights *out) {
  if (!out || !isfinite(tile_x) || !isfinite(tile_y)) return false;
  *out = (SimWorldNavigationTerrainHeights){0};
  out->height_units = HeightOwned(tile_x, tile_y, &out->authored_weight,
      0, 0, 0, &out->floor_height_units);
  return true;
}

bool SimWorldNavigationTerrain_Sample(
    float tile_x, float tile_y,
    SimWorldNavigationTerrainSample *out) {
  SimWorldNavigationTerrainHeights heights;
  if (!out || !SimWorldNavigationTerrain_SampleHeights(tile_x, tile_y, &heights)) return false;
  *out = (SimWorldNavigationTerrainSample){
    .height_units = heights.height_units,
    .floor_height_units = heights.floor_height_units,
    .authored_weight = heights.authored_weight,
  };
  const float step = kSlopeSampleStepTiles;
  const float west = HeightOnly(tile_x - step, tile_y, NULL);
  const float east = HeightOnly(tile_x + step, tile_y, NULL);
  const float north = HeightOnly(tile_x, tile_y - step, NULL);
  const float south = HeightOnly(tile_x, tile_y + step, NULL);
  out->slope_x = (east - west) / (step * 2.0f);
  out->slope_y = (south - north) / (step * 2.0f);
  return true;
}

float SimWorldNavigationTerrain_HeightUnits(float tile_x, float tile_y) {
  if (!isfinite(tile_x) || !isfinite(tile_y)) return 0.0f;
  return HeightOnly(tile_x, tile_y, NULL);
}
