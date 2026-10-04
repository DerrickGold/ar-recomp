#ifndef AR_CHURCH_SCENE_H
#define AR_CHURCH_SCENE_H
/* Prototype nave, viewed from the Master's chair toward the town-facing door.
 * Receives immutable town geometry; never reads game state. */
#include "render/render_device.h"
#include "sim/church/church_landscape.h"
#include "sim/voxels/sim_background_voxels.h"

typedef struct ChurchSceneOptions {
  const ChurchLandscape *landscape;
  float brightness;
  bool reference_altar; /* Plain block and floor guides for perspective review. */
  double seconds;       /* Captured presentation time; dust never reads a live clock. */
  float landscape_height_pct;
  uint32_t town_serial;
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg, shading, style;
  ArRenderTexture people;        /* Native UI, people and Palace columns, 256 x 512. */
  ArRenderRectI actor_bounds[2]; /* Opaque bounds within each 32x32 native cell. */
} ChurchSceneOptions;

/* Diagnostic work counters; reset with scene resources, never a GPU timer. */
typedef struct ChurchSceneStats {
  unsigned geometry_builds, room_bakes;
  size_t exterior_quads;
  unsigned moving_trees;
} ChurchSceneStats;
ChurchSceneStats ChurchScene_Stats(void);

/* Draws into the current viewport. Ground pixels are the last complete town
 * canvas (512 square ARGB), not the church's replacement VRAM contents. */
void ChurchScene_Reset(ArRenderDevice *device);
bool ChurchScene_Draw(ArRenderDevice *device, ArRenderRectI viewport,
                      const SimBackgroundVoxelScene *town, const uint32_t *ground,
                      const ChurchSceneOptions *options);
#endif
