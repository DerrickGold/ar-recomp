#ifndef AR_PRESENT_SIM3D_CLOUDS_H
#define AR_PRESENT_SIM3D_CLOUDS_H

#include "present.h"
#include "render/render_types.h"
#include "presentation_outcome.h"
#include "present_sim_globe.h"

/* SimCloudTexel and the layer table are declared in
 * present_sim3d_internal.h: the world-map sky shares them. */

/* Covers the ground beyond OAM's reach, which is permanently actor-free and
 * reads as a bug rather than as distance without it. Drawn last, over the
 * objects, so what it hides is unresolvably distant instead of missing.
 * A completed globe view selects curved shell sampling; NULL retains the
 * flat underlay's shroud. Both share the same settings and texture effect. */
PresentationOutcome DrawSimCloudShroud(
    const FrameSlot *slot, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16], const PresentSimGlobeView *globe);

void PresentSim3DClouds_ResetResources(void);

#endif /* AR_PRESENT_SIM3D_CLOUDS_H */
