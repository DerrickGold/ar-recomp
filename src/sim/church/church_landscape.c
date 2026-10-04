#include "sim/church/church_landscape.h"

#include <math.h>
#include <string.h>
#include "sim/town/sim_town_terrain.h"
#include "sim/world_nav/sim_world_navigation_terrain.h"

bool ChurchLandscape_Capture(ChurchLandscape *out, const SimBackgroundVoxelScene *town) {
  if (!out || !town) return false;
  const uint32_t *atlas = SimBackgroundVoxels_AtlasPixels();
  if (!atlas && town->mountains.cell_count) return false;
  if (atlas)
    memcpy(out->mountain_atlas, atlas, sizeof(out->mountain_atlas));
  else
    memset(out->mountain_atlas, 0, sizeof(out->mountain_atlas));
  memset(out->mountain_sources, 0, sizeof(out->mountain_sources));
  for (int tile = 0; tile < 256; tile++) {
    int x, y;
    if (SimBackgroundVoxels_MountainTileSource((uint8_t)tile, &x, &y))
      out->mountain_sources[tile] = (uint16_t)(y * 32 + x + 1);
  }
  const bool previous_world = out->world_valid;
  const uint32_t *world = SimWorldMap_BakedPixels();
  out->world_valid = world && SimWorldMap_OriginForTown(town->town, &out->origin_x, &out->origin_y);
  if (!out->world_valid) return true;
  const uint32_t world_serial = SimWorldMap_Serial();
  const uint32_t geography_serial = SimWorldMap_GeographySerial();
  if (!previous_world || out->world_serial != world_serial)
    memcpy(out->world_pixels, world, sizeof(out->world_pixels));
  out->water_valid = SimWorldMap_CopyWaterFrames(out->water_frames);
  out->world_serial = world_serial;
  if (previous_world && out->town == town->town && out->geography_serial == geography_serial)
    return true;
  /* Register the local cathedral datum with the same sea level used by SIM's
   * surrounding world. This preserves the room's floor at local zero. */
  float anchor_x = 16, anchor_y = 16;
  for (unsigned i = 0; i < town->object_count; i++) {
    if (town->objects[i].kind != kSimBackgroundVoxel_Cathedral) continue;
    anchor_x = town->objects[i].cell_x + 1;
    anchor_y = town->objects[i].cell_y + 1;
    break;
  }
  const float native = SimTownTerrain_HeightUnitsAt(town->town, anchor_x * 16, anchor_y * 16);
  float registered;
  if (!SimWorldNavigationTerrain_RegisterTownFloor(town->town, anchor_x, anchor_y, native,
                                                   &registered))
    return false;
  out->datum = registered - native;
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 128; x++)
      out->open_water[y * 128 + x] = SimWorldMap_CellIsOpenWater(x, y);
  for (int y = 0; y <= 128; y++)
    for (int x = 0; x <= 128; x++)
      out->world_heights[y * 129 + x] = SimWorldNavigationTerrain_HeightUnits(x, y) - out->datum;
  for (int y = 0; y <= 32; y++) {
    for (int x = 0; x <= 32; x++) {
      const float height = SimTownTerrain_HeightUnitsAt(town->town, x * 16, y * 16);
      if (!SimWorldNavigationTerrain_RegisterTownFloor(town->town, x, y, height, &registered))
        return false;
      out->town_heights[y * 33 + x] = registered - out->datum;
      /* The border's exact native floor meets the same outside world mesh. */
      const int wx = out->origin_x + x, wy = out->origin_y + y;
      if ((x == 0 || x == 32 || y == 0 || y == 32) && wx >= 0 && wx <= 128 && wy >= 0 && wy <= 128)
        out->world_heights[wy * 129 + wx] = registered - out->datum;
    }
  }
  out->town = town->town;
  out->geography_serial = geography_serial;
  return true;
}
