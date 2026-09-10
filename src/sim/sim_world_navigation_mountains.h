#ifndef SIM_WORLD_NAVIGATION_MOUNTAINS_H
#define SIM_WORLD_NAVIGATION_MOUNTAINS_H

#include <stddef.h>
#include "sim_background_mountain_mesh.h"
#include "sim_world_navigation_towns.h"

enum {
  kSimWorldNavigationMountainAtlasPixels = 512,
  /* Native mountain stamps never use tile $FF. Reserve its last bank for
   * the single 16x16 overhead crater, without enlarging the shared atlas. */
  kSimWorldNavigationCraterAtlasX = 496,
  kSimWorldNavigationCraterAtlasY = 496,
};

typedef struct SimWorldNavigationMountainFace {
  /* Fixed world-map tile coordinates; z is native mountain pixels / 16,
   * above the registered floor, not above the overview's inferred peak. */
  float x[4], y[4], z[4];
  SimBackgroundMountainMeshUV uv[4];
  uint8_t brightness[4];
  uint8_t town;
  /* Rear closure is independently auditable against retained ground/model
   * occupancy, including the volcano's clipped summit roof. */
  bool rear_slope;
  bool summit_roof;
  /* Optional continuation of a horizontally clipped source stamp. These
   * faces are restricted to semantic rock outside all six town windows. */
  bool exterior;
} SimWorldNavigationMountainFace;

/* Presentation-owned retained scene. Zero initialize before first Build,
 * destroy on renderer shutdown. Nothing aliases the captured input or ROM. */
typedef struct SimWorldNavigationMountainScene {
  SimWorldNavigationMountainFace *faces;
  size_t face_count, face_capacity;
  size_t exterior_face_count;
  uint32_t *atlas;
  uint8_t town_mask;
  float maximum_rise;
  float town_maximum_rise[kSimTownCount];
  /* Nonzero samples identify the accepted source town (1..6). */
  uint8_t replacement[kSimWorldMapTiles * kSimWorldMapTiles];
  /* Extra geometry's physical footprint. Boundary-height fitting must not
   * push the inferred surface up through a newly completed native slope. */
  uint8_t exterior_cells[kSimWorldMapTiles * kSimWorldMapTiles];
  /* Aitos' native $21 palette pulse affects its overhead opening (or the
   * two original crown tiles when overhead art is unavailable).
   * Retain source-index masks, including the cutout silhouette, so an RGB
   * collision cannot animate unrelated material. No per-frame allocation. */
  uint8_t lava_mask[2][kSimTownCellPixels * kSimTownCellPixels];
  uint8_t lava_tiles, lava_variant, lava_red;
  bool lava_ready;
  bool overhead_crater;
} SimWorldNavigationMountainScene;

typedef struct SimWorldNavigationMountainAtlasUpdate {
  uint16_t x, y, width, height;
} SimWorldNavigationMountainAtlasUpdate;

/* Reproduce the native Aitos red ramp from the captured game clock. Returns
 * true when a new palette state is prepared; update describes its bounded
 * atlas rectangle. Geometry and ownership remain unchanged. */
bool SimWorldNavigationMountains_UpdateLava(
    SimWorldNavigationMountainScene *scene, uint16_t game_frame,
    SimWorldNavigationMountainAtlasUpdate *update);

bool SimWorldNavigationMountains_Build(
    const SimWorldNavigationTownGround *ground,
    SimWorldNavigationMountainScene *out);
/* Append missing east/west stamp columns over confirmed exterior rock only.
 * Call after Build, before atlas upload. Requires the developed world map;
 * rebuild on geography changes, not water animation. Failure preserves the
 * original scene's faces and relief ownership. No town-mode meshes change. */
bool SimWorldNavigationMountains_ContinueEdges(
    const SimWorldNavigationTownGround *ground,
    SimWorldNavigationMountainScene *scene);
void SimWorldNavigationMountains_Destroy(SimWorldNavigationMountainScene *scene);
/* Remove old overview slopes only where complete native objects were built.
 * Unsupported or disabled towns retain both their art and inferred relief.
 * Optional kSimWorldMapBytes cell mask restricts an incremental rebake. */
bool SimWorldNavigationMountains_ClearGround(
    uint32_t *pixels, int pitch, const SimWorldNavigationTownGround *ground,
    uint8_t town_mask, const uint8_t *cells);

#endif  /* SIM_WORLD_NAVIGATION_MOUNTAINS_H */
