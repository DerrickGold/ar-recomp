#ifndef SIM_WORLD_NAVIGATION_TERRAIN_H
#define SIM_WORLD_NAVIGATION_TERRAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_world_map.h"

/* Continuous low-detail relief for the 128x128 Mode-7 world.
 *
 * Inside each town window this registers the audited ground heightfield to a
 * common world plain (raising low-datum towns without flattening contours), plus the
 * mountain layer (towns render mountains separately from their ground).
 * The authored world rock palette supplies continuous mountain silhouettes
 * both inside and outside towns. Lowlands blend four cells either side of
 * borders. The inferred ocean outside town ownership remains at sea datum;
 * authored towns retain their native land/water contours instead of acquiring
 * an additional coast-derived slope through otherwise level ground.
 * The transition is evaluated in world-map tile
 * coordinates: one tile is one town cell, so there is no second scale or
 * hand-authored registration for the two representations to drift between. */
typedef struct SimWorldNavigationTerrainSample {
  float height_units;
  /* Same datum/coast/ground grading, without the separately owned rock
   * rise. Native mountain geometry must not be anchored on another peak. */
  float floor_height_units;
  /* Derivatives in elevation units per world-map tile. */
  float slope_x;
  float slope_y;
  /* 1 in an audited town interior, 0 in wholly inferred terrain. Boundary
   * blends occupy the interval and let presentation keep inferred relief more
   * restrained than known town geography. */
  float authored_weight;
} SimWorldNavigationTerrainSample;

/* Embed an explicitly owned native terrain height (including either side of
 * a hard cliff) using the world's existing datum/coast/border registration.
 * Local XY is in town cells, inclusive [0,32]. No inferred mountain rise is
 * added; the native town owns its relief objects. No preparation or mutation. */
bool SimWorldNavigationTerrain_RegisterTownFloor(uint8_t town,
    float local_x, float local_y, float height, float *out);

/* Retained globe geometry consumes heights and ownership, not derivatives.
 * Keep the cheaper query explicit instead of changing full-sample semantics. */
typedef struct SimWorldNavigationTerrainHeights {
  float height_units;
  float floor_height_units;
  float authored_weight;
} SimWorldNavigationTerrainHeights;

/* Samples a finite point in [0,128] on each axis. Out-of-range coordinates
 * are clamped so atmosphere meshes can safely extend a shadow by a few tiles.
 * False is reserved for a null output or non-finite input. Queries do not
 * prepare or mutate state: parallel readers are safe while the owner keeps
 * the world map/prior and all mountain constraints frozen until they join. */
bool SimWorldNavigationTerrain_Sample(
    float tile_x, float tile_y,
    SimWorldNavigationTerrainSample *out_sample);
/* Same heights/ownership and input contract, without four derivative queries. */
bool SimWorldNavigationTerrain_SampleHeights(
    float tile_x, float tile_y,
    SimWorldNavigationTerrainHeights *out_heights);

/* Height-only convenience form used while preparing the retained mesh. */
float SimWorldNavigationTerrain_HeightUnits(float tile_x, float tile_y);
float SimWorldNavigationTerrain_FloorHeightUnits(float tile_x, float tile_y);
/* Exact NW/NE/SE/SW heights owned by one authored town cell. Unlike the
 * point sampler, a cliff's shared coordinate retains this side's height.
 * The world datum, coast and boundary grading are still applied identically. */
bool SimWorldNavigationTerrain_TownCellCorners(
    uint8_t town, int cell_x, int cell_y, float height[4]);
/* Upper bound of inferred rock rise before native-boundary registration. */
float SimWorldNavigationTerrain_MaxMountainRise(void);

/* Rebuild from the developed tilemap's native material indices. The geography
 * serial owns this cache; palette changes and animated waves cannot alter
 * terrain height. Town base elevations retain their native relative contours. */
bool SimWorldNavigationTerrain_RebuildWorldPrior(void);

/* Serial of the last accepted world prior, or zero while the deterministic
 * town-edge fallback is in use. */
uint32_t SimWorldNavigationTerrain_WorldPriorSerial(void);

/* Optional ownership of inferred rock by separately rendered native mountain
 * objects. One byte per world-map cell; NULL restores all overview relief.
 * The consumer must invalidate its retained terrain projection after a change. */
void SimWorldNavigationTerrain_SetMountainReplacement(const uint8_t *cells);
/* Optional 129x129 native-boundary ridge factors; copied, never borrowed.
 * NULL restores the original inferred profile. Town floors are unaffected. */
void SimWorldNavigationTerrain_SetMountainTransition(const float *ridge_scale);
/* Optional 129x129 physical native-edge rise and blend weight, copied. The
 * presenter supplies physical-tile to current relief-unit conversion so the
 * edge stays attached when landscape height changes. Targets are premultiplied
 * by weight before interpolation; zero weight preserves the old relief exactly.
 * NULL/invalid conversion disables the join. Native floor height is unaffected. */
void SimWorldNavigationTerrain_SetMountainJoin(
    const float *rise, const float *weight, float native_to_relief);
/* Same copied-field contract, but only lowers inferred rock to a continued
 * mountain edge limit. It never raises terrain or changes the native floor. */
void SimWorldNavigationTerrain_SetMountainContinuationLimit(
    const float *rise, const float *weight, float native_to_relief);

#endif  /* SIM_WORLD_NAVIGATION_TERRAIN_H */
