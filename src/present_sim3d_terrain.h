#ifndef AR_PRESENT_SIM3D_TERRAIN_H
#define AR_PRESENT_SIM3D_TERRAIN_H

#include "present.h"
#include "render/render_types.h"
#include "present_sim3d_project.h"
#include "render/render_device.h"

#if AR_SIM3D_TERRAIN_ELEVATION

/* Synchronous, borrowed quad data before camera projection. XY is native
 * town pixels, heights are unscaled terrain units, UVs sample the cleaned SIM
 * canvas, and shade includes the audited slope/contact/skirt lighting once.
 * False stops emission. Shares the town/height terrain cache; does not project,
 * sort, draw or retain inputs. */
typedef bool (*SimTownTerrainEmit)(void *user, const float xy[4][2],
    const float height[4], const float uv[4][2], const float shade[4]);
bool EmitSimTownTerrainSource(uint8_t town, uint16_t landscape_height_pct,
    SimTownTerrainEmit emit, void *user);

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
