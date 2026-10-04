#ifndef AR_CHURCH_LANDSCAPE_H
#define AR_CHURCH_LANDSCAPE_H
/* Owned terrain/art snapshot. The native church reuses SIM's live buffers. */
#include "sim/voxels/sim_background_voxels.h"
#include "sim/town/sim_town_terrain.h"
#include <math.h>

typedef struct ChurchLandscape {
  uint32_t water_frames[4][64];
  uint8_t open_water[128 * 128];
  bool water_valid;
  uint32_t mountain_atlas[512 * 512];
  uint16_t mountain_sources[256];
  uint32_t world_pixels[1024 * 1024];
  float world_heights[129 * 129];
  float town_heights[33 * 33];
  uint32_t world_serial, geography_serial;
  uint8_t town;
  float datum;
  int origin_x, origin_y; /* Town origin in world-map cells. */
  bool world_valid;
} ChurchLandscape;

/* Called only during enhanced SIM capture, before the room transition. */
bool ChurchLandscape_Capture(ChurchLandscape *out, const SimBackgroundVoxelScene *town);
/* Registered terrain in native town-pixel units, before landscape scaling. */
static inline float ChurchLandscape_Terrain(const ChurchLandscape *landscape, uint8_t town, float x,
                                            float y) {
  if (!landscape || !landscape->world_valid) return SimTownTerrain_HeightUnitsAt(town, x, y) * 16;
  const float px = fminf(31.99999f, fmaxf(0, x / 16));
  const float py = fminf(31.99999f, fmaxf(0, y / 16));
  const int ix = (int)px, iy = (int)py;
  const float u = px - ix, v = py - iy;
  const float *h = landscape->town_heights + iy * 33 + ix;
  return ((h[0] * (1 - u) + h[1] * u) * (1 - v) + (h[33] * (1 - u) + h[34] * u) * v) * 16;
}
#endif
