#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim/sim_world_navigation_terrain.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim_world_navigation_cliffs.h"

enum { kRomBytes = 0x100000, kMapOffset = 0x33341, kChrOffset = 0x70000 };

static float ReferenceClamp(float x, float low, float high) {
  return x < low ? low : x > high ? high : x;
}

static float ReferenceSmooth(float x) {
  x = ReferenceClamp(x, 0, 1);
  return x * x * (3 - 2 * x);
}

/* Independent fallback oracle deliberately samples ALL six towns, including
 * zero-weight ones. It must not share the production influence rejection. */
static float ReferenceRegisteredFloor(float x, float y, uint8_t owner,
                                      int cx, int cy, float *influence) {
  static const float datum[] = {0, 3, 4, 4, 0, 4};
  x = ReferenceClamp(x, 0, 128); y = ReferenceClamp(y, 0, 128);
  float total = 0, weights = 0;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    const float lx = x - ox, ly = y - oy;
    const float weight = ReferenceSmooth((lx + 4) / 8) *
        ReferenceSmooth((32 - lx + 4) / 8) *
        ReferenceSmooth((ly + 4) / 8) * ReferenceSmooth((32 - ly + 4) / 8);
    float height = SimTownTerrain_HeightUnitsAt(town,
        ReferenceClamp(lx, 0, 32) * 16, ReferenceClamp(ly, 0, 32) * 16);
    if (town == owner) {
      SimTownTerrainSample sample;
      assert(SimTownTerrain_SampleCell(town, cx, cy, lx - cx, ly - cy, &sample));
      height = sample.height_units;
    }
    total += (height + datum[town - 1]) * weight;
    weights += weight;
  }
  *influence = ReferenceClamp(weights, 0, 1);
  const float registered = weights > 0 ? total / weights : 4;
  return registered * *influence + 4 * (1 - *influence);
}

static void CheckRegisteredPoint(float x, float y) {
  float influence;
  const float expected = ReferenceRegisteredFloor(x, y, 0, 0, 0, &influence);
  SimWorldNavigationTerrainHeights sample;
  assert(SimWorldNavigationTerrain_SampleHeights(x, y, &sample));
  assert(sample.floor_height_units == expected && sample.height_units == expected);
  assert(sample.authored_weight == influence);
}

static void TestTownInfluenceRejection(void) {
  assert(SimWorldNavigationTerrain_WorldPriorSerial() == 0);
  /* Half-tile mesh plus the floats immediately inside/outside all four
   * feather edges, including corners and overlapping town influences. */
  for (int y = -2; y <= 258; y++)
    for (int x = -2; x <= 258; x++) CheckRegisteredPoint(x * .5f, y * .5f);
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    const float edge[] = {-4, 0, 4, 28, 32, 36};
    for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
      const float x = ox + edge[i], y = oy + edge[i];
      const float xx[] = {nextafterf(x, -INFINITY), x, nextafterf(x, INFINITY)};
      const float yy[] = {nextafterf(y, -INFINITY), y, nextafterf(y, INFINITY)};
      for (int at = -4; at <= 36; at++)
        for (int p = 0; p < 3; p++) {
          CheckRegisteredPoint(xx[p], oy + at);
          CheckRegisteredPoint(ox + at, yy[p]);
          for (int q = 0; q < 3; q++) CheckRegisteredPoint(xx[p], yy[q]);
        }
    }
    static const int dx[] = {0, 1, 1, 0}, dy[] = {0, 0, 1, 1};
    for (int cy = 0; cy < 32; cy++)
      for (int cx = 0; cx < 32; cx++) {
        float height[4];
        assert(SimWorldNavigationTerrain_TownCellCorners(town, cx, cy, height));
        for (int p = 0; p < 4; p++) {
          float influence;
          assert(height[p] == ReferenceRegisteredFloor(
              ox + cx + dx[p], oy + cy + dy[p], town, cx, cy, &influence));
        }
      }
  }
}

static void TestSampleConsistency(void) {
  /* Both total and floor are required in one query by the retained world
   * mesh. Optional native constraints must affect its total and derivatives
   * exactly as they affect the height-only API, never its floor datum. */
  static const float points[][2] = {
    {28, 61}, {32, 64}, {32.5f, 64.5f}, {10, 50}, {60, 74}, {50, 74},
    {28, 76}, {48, 16}, {-1, 50}, {129, 129},
  };
  for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    const float x = points[i][0], y = points[i][1];
    SimWorldNavigationTerrainSample sample;
    SimWorldNavigationTerrainHeights heights;
    assert(SimWorldNavigationTerrain_Sample(x, y, &sample));
    assert(SimWorldNavigationTerrain_SampleHeights(x, y, &heights));
    assert(heights.height_units == sample.height_units);
    assert(heights.floor_height_units == sample.floor_height_units);
    assert(heights.authored_weight == sample.authored_weight);
    assert(sample.height_units == SimWorldNavigationTerrain_HeightUnits(x, y));
    assert(sample.floor_height_units == SimWorldNavigationTerrain_FloorHeightUnits(x, y));
    assert(sample.slope_x == SimWorldNavigationTerrain_HeightUnits(x + .5f, y) -
        SimWorldNavigationTerrain_HeightUnits(x - .5f, y));
    assert(sample.slope_y == SimWorldNavigationTerrain_HeightUnits(x, y + .5f) -
        SimWorldNavigationTerrain_HeightUnits(x, y - .5f));
    assert(sample.authored_weight >= 0 && sample.authored_weight <= 1);
  }
}

static void TestCliffOwnership(void) {
  SimWorldNavigationCliffScene scene = {0};
  assert(!SimWorldNavigationCliffs_Build(63, NULL));
  assert(SimWorldNavigationCliffs_Build(63, &scene));
  assert(scene.town_mask == 63 && scene.face_count > 0);
  size_t caps = 0;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    for (int cy = 0; cy < kSimTownCells; cy++)
      for (int cx = 0; cx < kSimTownCells; cx++) {
        float height[4];
        assert(SimWorldNavigationTerrain_TownCellCorners(town, cx, cy, height));
        const unsigned cap = scene.replacement[(oy + cy) * 128 + ox + cx];
        if (SimTownTerrain_IsFaceCell(town, cx, cy)) assert(cap);
        if (!cap) continue;
        caps++;
        assert(cap <= scene.face_count);
        const SimWorldNavigationCliffFace *face = &scene.faces[cap - 1];
        assert(!memcmp(height, face->height, sizeof(height)));
        assert(face->x[0] == ox + cx && face->y[0] == oy + cy);
        assert(face->x[2] == ox + cx + 1 && face->y[2] == oy + cy + 1);
        assert(face->shade == 1);
        /* Interior registration matches the full town, with no opposite-side
         * sampling or averaging through its authored hard cliff corners. */
        if (cx >= 4 && cx < 28 && cy >= 4 && cy < 28) {
          static const float datum[] = {0, 3, 4, 4, 0, 4};
          for (int p = 0; p < 4; p++)
            assert(fabsf(height[p] - SimTownTerrain_CornerUnits(town, cx, cy, p) - datum[town - 1]) < .00001f);
        }
      }
  }
  assert(caps < 1024); /* Sparse, not a private vertex for every town tile. */
  assert(scene.face_count > caps && scene.face_count < 2048);
  for (size_t i = caps; i < scene.face_count; i++) {
    const SimWorldNavigationCliffFace *face = &scene.faces[i];
    assert(face->x[0] == face->x[3] && face->x[1] == face->x[2]);
    assert(face->y[0] == face->y[3] && face->y[1] == face->y[2]);
    assert(face->height[0] >= face->height[3] - .00001f);
    assert(face->height[1] >= face->height[2] - .00001f);
    for (int p = 0; p < 4; p++) {
      assert(isfinite(face->height[p]));
      assert(face->u[p] >= 0 && face->u[p] <= 1 && face->v[p] >= 0 && face->v[p] <= 1);
    }
  }
  printf("native cliffs: %zu caps, %zu closed skirts\n", caps, scene.face_count - caps);
  assert(SimWorldNavigationCliffs_Build(0, &scene));
  assert(!scene.faces && !scene.face_count && !scene.town_mask);
  for (int i = 0; i < 128 * 128; i++) assert(!scene.replacement[i]);
  SimWorldNavigationCliffs_Destroy(&scene);
  SimWorldNavigationCliffs_Destroy(&scene);
  float h[4];
  assert(!SimWorldNavigationTerrain_TownCellCorners(0, 0, 0, h));
  assert(!SimWorldNavigationTerrain_TownCellCorners(1, -1, 0, h));
  assert(!SimWorldNavigationTerrain_TownCellCorners(1, 0, 32, h));
  assert(!SimWorldNavigationTerrain_TownCellCorners(1, 0, 0, NULL));
}

int main(void) {
  TestTownInfluenceRejection();
  uint8_t *rom = calloc(1, kRomBytes);
  assert(rom);
  /* Green lowland, bright desert, bright snow, authored mountain rock. */
  const uint8_t material[] = {0x08, 0x2F, 0x1D, 0x44, 0x02};
  for (int tile = 0; tile < 5; tile++)
    memset(rom + kChrOffset + tile * 64, material[tile], 64);
  const uint16_t colours[] = {0x226C, 0x4AFF, 0x7FFF, 0x21B0, 0x7C21};
  for (int tile = 0; tile < 5; tile++) {
    const int at = 0xE3F93 + material[tile] * 2;
    rom[at] = (uint8_t)colours[tile];
    rom[at + 1] = (uint8_t)(colours[tile] >> 8);
  }
  assert(SimWorldMap_Init(rom, kRomBytes));
  free(rom);
  const uint32_t *pixels = SimWorldMap_BakedPixels();
  assert(SimWorldNavigationTerrain_RebuildWorldPrior(
      pixels, kSimWorldMapPixels, SimWorldMap_GeographySerial()));
  TestCliffOwnership();
  TestSampleConsistency();

  const float seam_floor = SimWorldNavigationTerrain_HeightUnits(32, 64);
  const float remote_floor = SimWorldNavigationTerrain_HeightUnits(10, 50);
  const float desert_floor = SimWorldNavigationTerrain_HeightUnits(28, 76);
  const float snow_floor = SimWorldNavigationTerrain_HeightUnits(48, 16);
  const float lake_floor = SimWorldNavigationTerrain_HeightUnits(60, 74);
  const float river_floor = SimWorldNavigationTerrain_HeightUnits(50, 74);
  /* Registered plains are close in altitude instead of a three-unit step
   * from Bloodpool to Fillmore or a sunken Kasandora patch. */
  assert(SimWorldNavigationTerrain_HeightUnits(60, 60) >= 3.8f);
  assert(SimWorldNavigationTerrain_HeightUnits(40, 80) >= 4.0f);
  assert(SimWorldNavigationTerrain_HeightUnits(40, 80) < 5.0f);
  for (float y = 49; y < 80; y += 0.5f) {
    const float west = SimWorldNavigationTerrain_HeightUnits(79.999f, y);
    const float east = SimWorldNavigationTerrain_HeightUnits(80.001f, y);
    assert(fabsf(west - east) < 0.02f);
  }
  uint8_t map[kSimWorldMapBytes] = {0};
  for (int y = 58; y < 70; y++)
    for (int x = 26; x < 38; x++) map[y * 128 + x] = 3;
  for (int y = 44; y < 56; y++)
    for (int x = 4; x < 16; x++) map[y * 128 + x] = 3;
  map[76 * 128 + 28] = 1;
  map[16 * 128 + 48] = 2;
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 4; x++) map[y * 128 + x] = 4;
  for (int x = 4; x < 58; x++) map[74 * 128 + x] = 4;
  for (int y = 72; y < 78; y++)
    for (int x = 58; x < 64; x++) map[y * 128 + x] = 4;
  assert(SimWorldMap_PublishBuiltTilemap(map) > 0);
  assert(SimWorldNavigationTerrain_RebuildWorldPrior(
      SimWorldMap_BakedPixels(), kSimWorldMapPixels,
      SimWorldMap_GeographySerial()));
  /* Mountain relief must survive exact-town constraints and continue through
   * both a shared town boundary and the otherwise inferred outside region. */
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) > seam_floor + 3.0f);
  assert(SimWorldNavigationTerrain_HeightUnits(10, 50) > remote_floor + 3.0f);
  const float native_floor = SimWorldNavigationTerrain_FloorHeightUnits(32, 64);
  assert(fabsf(native_floor - seam_floor) < .001f);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) - native_floor > 3);
  assert(SimWorldNavigationTerrain_HeightUnits(1, 50) == 0.0f);
  assert(SimWorldNavigationTerrain_HeightUnits(60, 74) == lake_floor);
  assert(SimWorldNavigationTerrain_HeightUnits(50, 74) == river_floor);
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(28, 76) -
               desert_floor) < 0.001f);
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(48, 16) -
               snow_floor) < 0.001f);
  const float north = SimWorldNavigationTerrain_HeightUnits(32, 63.999f);
  const float south = SimWorldNavigationTerrain_HeightUnits(32, 64.001f);
  assert(fabsf(north - south) < 0.02f);
  SimWorldNavigationTerrainSample sample;
  assert(SimWorldNavigationTerrain_Sample(28, 61, &sample));
  assert(sample.floor_height_units == SimWorldNavigationTerrain_FloorHeightUnits(28, 61));
  assert(sample.floor_height_units < sample.height_units);
  assert(isfinite(sample.height_units) && isfinite(sample.slope_x) &&
         isfinite(sample.slope_y));
  assert(sample.height_units > 0.5f && fabsf(sample.slope_x) > 0.1f);
  TestSampleConsistency();
  assert(!SimWorldNavigationTerrain_Sample(NAN, 0, &sample));
  assert(!SimWorldNavigationTerrain_Sample(0, INFINITY, &sample));
  SimWorldNavigationTerrainHeights heights;
  assert(!SimWorldNavigationTerrain_SampleHeights(0, 0, NULL));
  assert(!SimWorldNavigationTerrain_SampleHeights(NAN, 0, &heights));
  assert(!SimWorldNavigationTerrain_SampleHeights(0, INFINITY, &heights));
  const uint32_t geography = SimWorldMap_GeographySerial();
  const float height = SimWorldNavigationTerrain_HeightUnits(32, 64);
  (void)SimWorldMap_SetWaterAnimationSource(kWorldWaterSourceFirst);
  assert(SimWorldMap_GeographySerial() == geography);
  assert(SimWorldNavigationTerrain_RebuildWorldPrior(
      SimWorldMap_BakedPixels(), kSimWorldMapPixels, geography));
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  uint8_t replacement[kSimWorldMapBytes] = {0};
  for (int y = 58; y < 70; y++)
    for (int x = 26; x < 38; x++) replacement[y * 128 + x] = 1;
  SimWorldNavigationTerrain_SetMountainReplacement(replacement);
  TestSampleConsistency();
  assert(SimWorldNavigationTerrain_FloorHeightUnits(32, 64) == native_floor);
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(32, 64) - seam_floor) < 0.001f);
  assert(SimWorldNavigationTerrain_HeightUnits(10, 50) > remote_floor + 3.0f);
  assert(SimWorldNavigationTerrain_WorldPriorSerial() == geography);
  SimWorldNavigationTerrain_SetMountainReplacement(NULL);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  float ridge_scale[129 * 129];
  for (int i = 0; i < 129 * 129; i++) ridge_scale[i] = .5f;
  SimWorldNavigationTerrain_SetMountainTransition(ridge_scale);
  TestSampleConsistency();
  assert(SimWorldNavigationTerrain_FloorHeightUnits(32, 64) == native_floor);
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(32, 64) -
      (seam_floor + (height - seam_floor) * .5f)) < .001f);
  assert(SimWorldNavigationTerrain_HeightUnits(60, 74) == lake_floor);
  assert(SimWorldNavigationTerrain_HeightUnits(50, 74) == river_floor);
  SimWorldNavigationTerrain_SetMountainTransition(NULL);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  float join_rise[129 * 129], join_weight[129 * 129] = {0};
  for (int i = 0; i < 129 * 129; i++) join_rise[i] = NAN;
  const int anchor = 64 * 129 + 32;
  join_rise[anchor] = 2;
  join_weight[anchor] = 1;
  const float centre_floor = SimWorldNavigationTerrain_FloorHeightUnits(32.5f, 64.5f);
  const float centre_rise = SimWorldNavigationTerrain_HeightUnits(32.5f, 64.5f) - centre_floor;
  SimWorldNavigationTerrain_SetMountainJoin(join_rise, join_weight, 1);
  TestSampleConsistency();
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + 2);
  assert(SimWorldNavigationTerrain_FloorHeightUnits(32, 64) == native_floor);
  /* Premultiplication prevents the target from fading a second time when
   * the other three corners are inactive (or contain rejected data). */
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(32.5f, 64.5f) -
      (centre_floor + centre_rise * .75f + .5f)) < .00001f);
  assert(SimWorldNavigationTerrain_HeightUnits(60, 74) == lake_floor);
  assert(SimWorldNavigationTerrain_HeightUnits(50, 74) == river_floor);
  SimWorldNavigationTerrain_SetMountainJoin(join_rise, join_weight, 2);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + 4);
  join_rise[anchor] = 100; join_weight[anchor] = 0;
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + 4); /* copied */
  SimWorldNavigationTerrain_SetMountainJoin(join_rise, join_weight, 0);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  SimWorldNavigationTerrain_SetMountainJoin(join_rise, join_weight, NAN);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  SimWorldNavigationTerrain_SetMountainJoin(NULL, NULL, 0);
  join_rise[anchor] = .5f; join_weight[anchor] = 1;
  SimWorldNavigationTerrain_SetMountainContinuationLimit(join_rise, join_weight, 1);
  TestSampleConsistency();
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + .5f);
  assert(SimWorldNavigationTerrain_FloorHeightUnits(32, 64) == native_floor);
  assert(fabsf(SimWorldNavigationTerrain_HeightUnits(32.5f, 64.5f) -
      (centre_floor + fminf(centre_rise, centre_rise * .75f + .125f))) < .00001f);
  assert(SimWorldNavigationTerrain_HeightUnits(60, 74) == lake_floor);
  assert(SimWorldNavigationTerrain_HeightUnits(50, 74) == river_floor);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(join_rise, join_weight, 2);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + 1);
  join_rise[anchor] = 100;
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == native_floor + 1); /* copied */
  SimWorldNavigationTerrain_SetMountainContinuationLimit(join_rise, join_weight, 1);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height); /* Never raise lower terrain. */
  join_rise[anchor] = NAN;
  SimWorldNavigationTerrain_SetMountainContinuationLimit(join_rise, join_weight, 1);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(join_rise, join_weight, NAN);
  assert(SimWorldNavigationTerrain_HeightUnits(32, 64) == height);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(NULL, NULL, 0);
  TestSampleConsistency();
  assert(SimWorldNavigationTerrain_FloorHeightUnits(NAN, 0) == 0);
  assert(SimWorldNavigationTerrain_FloorHeightUnits(0, INFINITY) == 0);
  assert(!SimWorldNavigationTerrain_RebuildWorldPrior(NULL, 1024, 1));
  SimWorldMap_Shutdown();
  puts("sim_world_navigation_terrain_test: PASS");
  return 0;
}
