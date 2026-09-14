#ifndef AR_PRESENT_SIM3D_SHADOWS_H
#define AR_PRESENT_SIM3D_SHADOWS_H

#include "present.h"
#include "presentation_outcome.h"
#include "render/render_types.h"
#include "sim/sim_render_metadata.h"

/* SimShadowLight is declared in present_sim3d_internal.h: the world-map
 * renderer shares it. */

/* Where an object is actually drawn in world units, which is what its shadow
 * has to be cast from. */
void SimObjectDrawnWorld(
    const SimRenderObject *object, int *world_x, int *world_y);

/* Accumulates every classified caster into the screen-space mask, blurs it,
 * and composites it -- or, in an elevated town, hands it to the shared depth
 * pass to be sampled by visible terrain tops only. */
PresentationOutcome DrawSimShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    bool terrain_depth_receiver, ArRenderRectI source,
    ArRenderRectI viewport,
    const float matrix[16]);

/* Prepare, but do not composite, a native-town XY mask for curved terrain.
 * The borrowed texture remains valid until the next shadow preparation or
 * resource reset. Must run before entering the world's shared depth pass. */
PresentationOutcome PrepareSimTownShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    ArRenderRectI source, ArRenderRectI viewport, const float matrix[16],
    ArRenderTexture *out_mask);

void PresentSim3DShadows_ResetResources(void);

#endif /* AR_PRESENT_SIM3D_SHADOWS_H */
