#include "sim/church/church_landscape.h"
#include "sim/world_nav/sim_world_navigation_terrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                                              \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)
static uint32_t atlas[512 * 512], world[1024 * 1024];
static uint32_t pixels_serial = 1, geography_serial = 1;
static bool world_available = true;
static unsigned height_queries;
const uint32_t *SimBackgroundVoxels_AtlasPixels(void) { return atlas; }
bool SimBackgroundVoxels_MountainTileSource(uint8_t tile, int *x, int *y) {
  *x = 7;
  *y = 3;
  return tile == 42;
}
const uint32_t *SimWorldMap_BakedPixels(void) { return world_available ? world : NULL; }
bool SimWorldMap_CopyWaterFrames(uint32_t pixels[4][64]) {
  memset(pixels, 0, 4 * 64 * sizeof(uint32_t));
  pixels[2][3] = 0xff1234aa;
  return true;
}
bool SimWorldMap_CellIsOpenWater(int x, int y) { return x == 4 && y == 9; }
uint32_t SimWorldMap_Serial(void) { return pixels_serial; }
uint32_t SimWorldMap_GeographySerial(void) { return geography_serial; }
bool SimWorldMap_OriginForTown(uint8_t town, int *x, int *y) {
  *x = town == 4 ? 16 : 64;
  *y = town == 4 ? 32 : 96;
  return true;
}
float SimTownTerrain_HeightUnitsAt(uint8_t town, float x, float y) {
  (void)town;
  return x / 16 + y / 32;
}
float SimWorldNavigationTerrain_HeightUnits(float x, float y) {
  height_queries++;
  return x / 4 + y / 8;
}
bool SimWorldNavigationTerrain_RegisterTownFloor(uint8_t town, float x, float y, float height,
                                                 float *out) {
  (void)x;
  (void)y;
  *out = height + town * 2;
  return true;
}

int main(void) {
  ChurchLandscape *snapshot = calloc(1, sizeof(*snapshot));
  SimBackgroundVoxelScene town = {.town = 4, .object_count = 1};
  town.objects[0] =
      (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_Cathedral, .cell_x = 8, .cell_y = 12};
  CHECK(snapshot);
  atlas[11] = 0xffabcdef;
  world[23] = 0xff123456;
  CHECK(ChurchLandscape_Capture(snapshot, &town));
  CHECK(snapshot->water_valid && snapshot->water_frames[2][3] == 0xff1234aa &&
        snapshot->open_water[9 * 128 + 4]);
  CHECK(snapshot->world_valid && snapshot->datum == 8);
  CHECK(snapshot->mountain_sources[42] == 104);
  CHECK(snapshot->mountain_sources[41] == 0);
  CHECK(snapshot->mountain_atlas[11] == atlas[11] && snapshot->world_pixels[23] == world[23]);
  /* Both snapshots remain owned after native church code overwrites SIM art. */
  memset(atlas, 0, sizeof(atlas));
  memset(world, 0, sizeof(world));
  CHECK(snapshot->mountain_atlas[11] == 0xffabcdef && snapshot->world_pixels[23] == 0xff123456);
  CHECK(fabsf(ChurchLandscape_Terrain(snapshot, 4, 137, 201) - (137 + 201 / 2.0f)) < .001f);
  CHECK(fabsf(snapshot->world_heights[(32 + 7) * 129 + 16] - snapshot->town_heights[7 * 33]) <
        .001f);
  CHECK(fabsf(snapshot->world_heights[(32 + 32) * 129 + 16 + 9] -
              snapshot->town_heights[32 * 33 + 9]) < .001f);
  /* Palette/wave updates refresh color without recomputing geography. */
  unsigned queries = height_queries;
  pixels_serial++;
  world[23] = 0xff445566;
  CHECK(ChurchLandscape_Capture(snapshot, &town));
  CHECK(snapshot->world_pixels[23] == 0xff445566 && height_queries == queries);
  geography_serial++;
  CHECK(ChurchLandscape_Capture(snapshot, &town) && height_queries > queries);
  /* A region switch must re-register the sea datum even with the same map. */
  town.town = 5;
  queries = height_queries;
  CHECK(ChurchLandscape_Capture(snapshot, &town));
  CHECK(snapshot->datum == 10 && snapshot->origin_x == 64 && snapshot->origin_y == 96);
  CHECK(height_queries > queries);
  world_available = false;
  CHECK(ChurchLandscape_Capture(snapshot, &town) && !snapshot->world_valid);
  CHECK(fabsf(ChurchLandscape_Terrain(snapshot, 5, 137, 201) - (137 + 201 / 2.0f)) < .001f);
  free(snapshot);
  puts("church landscape: owned art, datum, coastline seams, caching and region changes PASS");
  return 0;
}
