#ifndef SIM_WORLD_MAP_BUILD_H
#define SIM_WORLD_MAP_BUILD_H

/* Main/CPU thread, once per rendered game frame. On town entry (including the
 * first frame after an act) or whenever the builder's simulation inputs change,
 * runs the ROM's developed-region overlay transactionally and publishes the
 * complete result to SimWorldMap. It never observes the live $7E:C000 shadow.
 *
 * See sim_world_map_build.c for the traced routine boundary, preconditions,
 * scratch footprint, and proof that the call closure is bounded and yield-free. */
void SimWorldMap_BuildIfNeeded(void);

#endif /* SIM_WORLD_MAP_BUILD_H */
