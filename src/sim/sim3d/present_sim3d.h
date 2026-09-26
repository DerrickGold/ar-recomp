#ifndef AR_PRESENT_SIM3D_H
#define AR_PRESENT_SIM3D_H
/* Town presentation: enhanced scene composition and the flat town underlay used
 * by native menus. All scene decisions come from the captured FrameSlot. */

#include "present/presentation_outcome.h"
#include "render/render_types.h"
struct FrameSlot;

/* Enter without a custom GPU state. This stage owns and unbinds every state it
 * binds; apply an outer shader after it returns. */
PresentationOutcome PresentSim3D(const struct FrameSlot *slot);
bool PresentSimMenuFlatTown(const struct FrameSlot *slot, ArRenderRectI source,
                            ArRenderRectI viewport);
/* Release town presentation caches while the host render device is alive. */
void PresentSim3D_ResetResources(void);
#endif
