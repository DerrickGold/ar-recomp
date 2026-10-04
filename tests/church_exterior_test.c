#include "sim/church/church_exterior.h"
#include "sim/voxels/sim_background_voxel_models.h"
#include <math.h>
#include <stdio.h>
#include "sim/town/sim_town_terrain.h"
#include <string.h>

static int failures;
#define CHECK(test)                                                                                \
  do {                                                                                             \
    if (!(test)) {                                                                                 \
      fprintf(stderr, "line %d: %s\n", __LINE__, #test);                                           \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)
static SimBackgroundVoxelScene town;
static uint32_t source[512 * 512], east[512 * 512], west[512 * 512], duplicate[512 * 512];

static double ShadowCenter(const uint32_t *pixels, bool y_axis) {
  double weighted = 0, total = 0;
  for (int y = 0; y < 512; y++) {
    for (int x = 0; x < 512; x++) {
      const unsigned shadow = 160 - (pixels[y * 512 + x] & 255);
      weighted += (y_axis ? y : x) * shadow;
      total += shadow;
    }
  }
  CHECK(total > 1000);
  return total > 0 ? weighted / total : 0;
}

int main(void) {
  for (int i = 0; i < 512 * 512; i++)
    source[i] = 0xffc0b0a0;
  ChurchSceneOptions options = {
      .light_azimuth_deg = 0, .light_elevation_deg = 35, .style = kSimBackgroundVoxelStyle_Varied};
  town.town = 1;
  CHECK(ChurchExterior_BakeGround(&town, &options, source, east));
  CHECK(memcmp(source, east, sizeof(source)) == 0);
  CHECK(!ChurchExterior_BakeGround(&town, &options, source, source));
  town.object_count = 1;
  town.objects[0] = (SimBackgroundVoxelObject){.kind = kSimBackgroundVoxel_House,
                                               .town = 1,
                                               .development_level = 2,
                                               .cell_x = 16,
                                               .cell_y = 16,
                                               .source_cells_w = 1,
                                               .source_cells_h = 1,
                                               .footprint_cells_w = 1,
                                               .footprint_cells_d = 1};
  CHECK(ChurchExterior_BakeGround(&town, &options, source, east));
  CHECK(ShadowCenter(east, false) > 267);
  options.light_azimuth_deg = 180;
  CHECK(ChurchExterior_BakeGround(&town, &options, source, west));
  CHECK(ShadowCenter(west, false) < 261);
  options.light_azimuth_deg = 90;
  CHECK(ChurchExterior_BakeGround(&town, &options, source, duplicate));
  CHECK(ShadowCenter(duplicate, true) < 261); /* SIM atlas Y opposes world Y. */
  options.light_azimuth_deg = 0;
  town.objects[1] = town.objects[0];
  town.object_count = 2;
  CHECK(ChurchExterior_BakeGround(&town, &options, source, duplicate));
  CHECK(memcmp(east, duplicate, sizeof(east)) == 0);
  bool feathered = false;
  for (int i = 0; i < 512 * 512; i++) {
    CHECK(source[i] == 0xffc0b0a0); /* Original SIM snapshot is immutable. */
    CHECK((east[i] & 0xff000000u) == 0xff000000u);
    CHECK((east[i] & 255) >= 92 && (east[i] & 255) <= 160);
    if ((east[i] & 255) > 95 && (east[i] & 255) < 155) feathered = true;
  }
  CHECK(feathered);
  CHECK(east[0] == source[0] && east[511 * 512 + 511] == source[511 * 512 + 511]);
  /* Put a caster beside a real terrain slope: projected silhouettes must
   * follow the captured landscape height rather than stay on a flat plane. */
  float largest_slope = 0;
  town.object_count = 1;
  for (int t = 1; t <= 6; t++) {
    for (int y = 2; y < 29; y++) {
      for (int x = 2; x < 29; x++) {
        const float slope = fabsf(SimTownTerrain_HeightUnitsAt(t, x * 16 + 28, y * 16 + 8) -
                                  SimTownTerrain_HeightUnitsAt(t, x * 16 + 8, y * 16 + 8));
        if (slope <= largest_slope) continue;
        largest_slope = slope;
        town.town = town.objects[0].town = t;
        town.objects[0].cell_x = x;
        town.objects[0].cell_y = y;
      }
    }
  }
  CHECK(largest_slope > .1f);
  CHECK(ChurchExterior_BakeGround(&town, &options, source, east));
  options.landscape_height_pct = 150;
  CHECK(ChurchExterior_BakeGround(&town, &options, source, west));
  CHECK(memcmp(east, west, sizeof(east)) != 0);
  return failures ? 1 : 0;
}
