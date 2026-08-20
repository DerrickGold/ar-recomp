#ifndef SIM_BACKGROUND_VOXELS_H
#define SIM_BACKGROUND_VOXELS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_mountains.h"
#include "sim_town_canvas.h"
#include "sim_background_voxel_types.h"

typedef struct SimBackgroundVoxelScene {
  uint8_t town;
  bool overflow;
  uint16_t object_count;
  uint16_t tree_cell_count;
  uint16_t tree_group_count;
  /* Clearable single-cell brush: round bushes and Marahna's palms. */
  uint16_t brush_cell_count;
  SimBackgroundMountainField mountains;
  SimBackgroundMountainCaps mountain_caps;
  SimBackgroundVoxelObject objects[kSimBackgroundMaxObjects];
} SimBackgroundVoxelScene;

/* Pure classification seam, used by the game-thread builder and ROM-free
 * tests. Every object comes from town state - structure records, the cell map's
 * terrain metatile identities and its reserved landmark plots - so the result
 * does not depend on the current palette, brightness or fade.
 *
 * `wind_stops_all` is the Extras enhancement: the "no wind" story event stills
 * only the windmills that existed when it fired, so a mill the town builds
 * afterwards keeps turning through it. True holds every mill in the town for
 * the duration; false reproduces the original game. It changes nothing else -
 * a windmill's built-versus-scaffold state always comes from the frame its
 * plot is drawing, under either setting. */
void SimBackgroundVoxels_Classify(uint8_t town, const uint8_t *wram,
                                  bool wind_stops_all,
                                  SimBackgroundVoxelScene *out);

void SimBackgroundVoxels_Reset(void);
/* Publishes scene topology and pixels independently. `canvas_layout_serial`
 * advances only for displayed tilemap changes; character animation, palette
 * cycling and fades can therefore refresh enhanced pixels without rescanning
 * structure records, mountains, bridges and foliage. The original canvas is
 * never modified: this owns a cutout atlas plus an inpainted ground copy used
 * only by enhanced SIM presentation. */
void SimBackgroundVoxels_Build(uint8_t town, const uint8_t *wram,
                               const uint32_t *canvas_pixels,
                               const uint16_t *vram,
                               uint32_t canvas_serial,
                               uint32_t canvas_layout_serial,
                               bool wind_stops_all);

uint32_t SimBackgroundVoxels_Serial(void);
uint32_t SimBackgroundVoxels_SceneSerial(void);
uint32_t SimBackgroundVoxels_GroundSerial(void);
uint32_t SimBackgroundVoxels_AtlasSerial(void);
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void);
const uint32_t *SimBackgroundVoxels_AtlasPixels(void);
const uint32_t *SimBackgroundVoxels_GroundPixels(void);
/* Render-thread upload cursor for the enhanced ground. Regions accumulate
 * until drained, independently of SimTownCanvas's authentic-canvas cursor. */
bool SimBackgroundVoxels_TakeGroundDirtyRect(
    int *x, int *y, int *width, int *height);

typedef struct SimBackgroundVoxelBuildStats {
  uint64_t build_calls;
  uint64_t scene_rebuilds;
  uint64_t pixel_refreshes;
  uint64_t ground_pixels_changed;
  uint64_t atlas_pixels_changed;
} SimBackgroundVoxelBuildStats;

SimBackgroundVoxelBuildStats SimBackgroundVoxels_BuildStats(void);
void SimBackgroundVoxels_ResetBuildStats(void);

/* Is this town cell mountain terrain in the published scene?
 *
 * The 2D town already answers "is this actor on a mountain?" -- it stands on a
 * mountain metatile or it stands on ground -- and that is exactly the question
 * of whether a mountain drawn in front of it should hide it. Presentation uses
 * this to split actors into the band drawn under the mountain art and the band
 * drawn over it. Out-of-range cells and a town with no published scene answer
 * false, so an actor defaults to the occluded band. */
bool SimBackgroundVoxels_CellIsMountain(int cell_x, int cell_y);

/* Is there mountain terrain BETWEEN this cell and the camera -- that is, south
 * of it in the same column? Only such a mountain can hide what stands here.
 * The camera looks from the south, so a mass north of an actor is behind it,
 * and a mass the actor stands south of cannot occlude it however tall the art
 * reaches up the screen. Without this test a villager standing in front of a
 * peak keeps his feet but loses his head, because the sprite's upper rows
 * reach into screen space the mountain's sheared art occupies. */
bool SimBackgroundVoxels_MountainInFrontOf(int cell_x, int cell_y);

/* Where the mountain art's surface sits at a town-map position, for placing an
 * actor the 2D game draws standing on it. The renderer shears a mountain out of
 * its own authentic art -- height is `(baseline - y) * face_height_scale` and
 * the art is pulled toward its base by `face_depth_scale` -- so an actor put
 * through the same transform lands exactly on the drawn slope instead of at
 * the mass's foot. `out_map_y` is the displaced town-pixel Y, `out_height` the
 * altitude in town pixels. False when the position is not mountain. */
bool SimBackgroundVoxels_MountainSurface(int map_x, int map_y,
                                         float *out_map_y, float *out_height);

/* Height in town pixels of the structure model standing on a town-map
 * position, for art the ROM anchors to a building's record cell. Zero when no
 * structure covers it, so an unattached bubble simply stays put. Trees and
 * foliage are excluded: nothing is anchored to them, and a bubble that drifted
 * onto a forest cell should not climb it. */
float SimBackgroundVoxels_StructureHeight(int map_x, int map_y);

/* Resolves a clean terrain-metatile source synthesized in the mountain atlas.
 * Unlike SimBackgroundMountains_TileSource, this is independent of whether a
 * pristine instance happens to be visible in the town's composed cell map. */
bool SimBackgroundVoxels_MountainTileSource(uint8_t tile,
                                            int *cell_x, int *cell_y);

#endif  /* SIM_BACKGROUND_VOXELS_H */
