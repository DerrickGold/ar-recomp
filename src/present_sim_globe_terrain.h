#ifndef AR_PRESENT_SIM_GLOBE_TERRAIN_H
#define AR_PRESENT_SIM_GLOBE_TERRAIN_H

#include "present_sim_globe_mapping.h"
#include "render/render_types.h"

/* The normal SIM ground source embedded on the
 * world's registered curved floor. Does not own or modify the SIM texture. */
bool PresentSimGlobeTerrain_Prepare(const SimGlobeMapping *map, uint32_t geography);
bool PresentSimGlobeTerrain_Append(const float matrix[16], float radius,
    ArRenderTexture ground, ArRenderTexture shadow, float shadow_opacity);
void PresentSimGlobeTerrain_Reset(void);

#endif
