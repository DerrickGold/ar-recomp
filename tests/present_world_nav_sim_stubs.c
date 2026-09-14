/* Navigation-only contract fixture: no live SIM canvas/model publisher.
 * The full GPU fixture links the real SIM providers. Unexpected entry here
 * must fail loudly rather than accidentally validate a partial town scene. */
#undef NDEBUG
#include <assert.h>
#include "present_sim3d_project.h"
#include "present_sim_globe_mountains.h"
#include "present_sim_globe_terrain.h"
#include "present_sim_globe_water.h"
#include "sim/sim_background_voxels.h"

SimBackgroundVoxelRenderParams SimVoxelRenderParams(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectI viewport, const float matrix[16]) {
  assert(!"SIM preparation in navigation-only fixture");
  return (SimBackgroundVoxelRenderParams){0};
}

bool PresentSimGlobeMountains_Prepare(const SimBackgroundVoxelRenderParams *params,
    const SimGlobeMapping *map, PresentSimGlobeGroundSample sample, uint32_t geography) {
  assert(!"SIM mountains in navigation-only fixture"); return false;
}
bool PresentSimGlobeMountains_Append(const float matrix[16], float radius) {
  assert(!"SIM mountains in navigation-only fixture"); return false;
}
bool PresentSimGlobeMountains_AppendEffects(const float matrix[16], ArRenderRectI viewport,
    uint16_t frame, uint8_t detail, uint8_t style, PresentSimGlobeGroundSample sample) {
  assert(!"SIM mountain effects in navigation-only fixture"); return false;
}
void PresentSimGlobeMountains_Reset(void) {}

bool PresentSimGlobeTerrain_Prepare(const SimGlobeMapping *map, uint32_t geography) {
  assert(!"SIM terrain in navigation-only fixture"); return false;
}
bool PresentSimGlobeTerrain_Append(const float matrix[16], float radius,
    ArRenderTexture ground, ArRenderTexture shadow, float shadow_opacity) {
  assert(!"SIM terrain in navigation-only fixture"); return false;
}
void PresentSimGlobeTerrain_Reset(void) {}

bool PresentSimGlobeWater_Matches(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground) {
  assert(!"SIM water in navigation-only fixture"); return false;
}
bool PresentSimGlobeWater_Prepare(const SimGlobeMapping *map,
    const SimWorldNavigationTownGround *ground, const Sim3DDepthSurfaceVertex *grid) {
  assert(!"SIM water in navigation-only fixture"); return false;
}
bool PresentSimGlobeWater_Append(const float matrix[16], float radius,
    ArRenderTexture ground, const Sim3DDepthSurfaceFocus *focus) {
  assert(!"SIM water in navigation-only fixture"); return false;
}
void PresentSimGlobeWater_Reset(void) {}

ArRenderTexture SimBackgroundVoxelRenderer_GroundTexture(uint32_t serial) {
  assert(!"SIM canvas in navigation-only fixture"); return ArRenderTexture_Invalid();
}
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void) {
  assert(!"SIM scene in navigation-only fixture"); return NULL;
}
