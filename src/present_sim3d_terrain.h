#ifndef AR_PRESENT_SIM3D_TERRAIN_H
#define AR_PRESENT_SIM3D_TERRAIN_H

#include "present.h"
#include "render/render_types.h"
#include "present_sim3d_project.h"
#include "render/render_device.h"

#if AR_SIM3D_TERRAIN_ELEVATION

/* Draws the audited terrain surface as the town's ground: per-cell corner
 * lighting cached by town and landscape height, a painter order cached across
 * ordinary panning, and one top plus its exposed skirts per cell. */
bool DrawSimTownTerrain(
    ArRenderDevice *device, ArRenderTexture texture,
    const FrameSlot *slot, float extent_x0,
    float extent_y0, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16], const SimCullFade *fade);

#endif /* AR_SIM3D_TERRAIN_ELEVATION */

#endif /* AR_PRESENT_SIM3D_TERRAIN_H */
