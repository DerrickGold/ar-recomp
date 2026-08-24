#ifndef AR_PRESENT_SIM3D_SHADOWS_H
#define AR_PRESENT_SIM3D_SHADOWS_H

#include <SDL3/SDL.h>

#include "present.h"
#include "presentation_outcome.h"
#include "sim/sim_render_metadata.h"

/* SimShadowLight is declared in present_sim3d_internal.h: the world-map
 * renderer shares it. */

/* Extra billboard scale on top of the perspective scale the lift produces.
 * Paired with the shadow's footprint shrink, a rising actor grows while its
 * shadow shrinks, which is what reads as height -- so the two live together. */
float SimBillboardHeightPop(
    SDL_Rect source, float height_world, unsigned height_pop_pct);

/* Where an object is actually drawn in world units, which is what its shadow
 * has to be cast from. */
void SimObjectDrawnWorld(
    const SimRenderObject *object, int *world_x, int *world_y);

/* A render target with the accumulate blend the caster mask needs. Exposed
 * because the rim light builds its mask the same way. */
SDL_Texture *CreateSimShadowTarget(int w, int h);

/* Accumulates every classified caster into the screen-space mask, blurs it,
 * and composites it -- or, in an elevated town, hands it to the shared depth
 * pass to be sampled by visible terrain tops only. */
PresentationOutcome DrawSimShadowMask(
    const FrameSlot *slot, bool virtual_height, bool soft_shadows,
    bool terrain_depth_receiver, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16]);

void PresentSim3DShadows_ResetResources(void);

#endif /* AR_PRESENT_SIM3D_SHADOWS_H */
