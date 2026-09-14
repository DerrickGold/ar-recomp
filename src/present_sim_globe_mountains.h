/* Continuous SIM town. Presentation-owned embedding of the
 * existing SIM mountain recipe, with no game-state or backend dependencies. */
#ifndef AR_PRESENT_SIM_GLOBE_MOUNTAINS_H
#define AR_PRESENT_SIM_GLOBE_MOUNTAINS_H
#include "present_sim_globe_mapping.h"
#include "sim/sim_background_voxel_renderer.h"

typedef float (*PresentSimGlobeGroundSample)(float chart_x, float chart_y);
bool PresentSimGlobeMountains_Prepare(
    const SimBackgroundVoxelRenderParams *params, const SimGlobeMapping *map,
    PresentSimGlobeGroundSample sample, uint32_t geography);
uint64_t PresentSimGlobeMountains_Revision(void);
bool PresentSimGlobeMountains_Append(const float matrix[16], float radius);
/* Owned native anchor re-encoded from the exact curved displayed mouth,
 * including facing and the native supporting floor. Cleared for other towns. */
bool PresentSimGlobeMountains_CraterAnchor(SimBackgroundCraterAnchor *out);
bool PresentSimGlobeMountains_AppendEffects(const float matrix[16], ArRenderRectI viewport,
    uint16_t frame, uint8_t detail, uint8_t style, PresentSimGlobeGroundSample sample);
void PresentSimGlobeMountains_Reset(void);
#endif
