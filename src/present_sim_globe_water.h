#ifndef PRESENT_SIM_GLOBE_WATER_H
#define PRESENT_SIM_GLOBE_WATER_H
#include "present_sim_globe_mapping.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_world_navigation_towns.h"

/* A bounded water-only transition outside the active town. Uses immutable
 * semantic texels (including mixed coastal tiles) for coastline protection
 * and borrows the live native ground atlas for current water color/animation.
 * Resident sub-cell rectangles are rebuilt only on geography/style changes;
 * no per-frame CPU rebake, texture cache, or new shader/backend contract. */
bool PresentSimGlobeWater_Matches(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground);
/* Called with the globe's newly built 129x129 radial grid, before its scratch
 * storage is released. Subdivisions follow that grid's actual triangles, not
 * a separately sampled height field. No source pointer survives the call.
 * The world owner invalidates/rebuilds this with its surface revisions. */
bool PresentSimGlobeWater_Prepare(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground, const Sim3DDepthSurfaceVertex *grid);
size_t PresentSimGlobeWater_QuadCount(void);
bool PresentSimGlobeWater_Append(const float matrix[16], float radius,
    ArRenderTexture ground, const Sim3DDepthSurfaceFocus *focus);
void PresentSimGlobeWater_Reset(void);
#endif
