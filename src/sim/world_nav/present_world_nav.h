#ifndef AR_PRESENT_WORLD_NAV_H
#define AR_PRESENT_WORLD_NAV_H
/* World-map navigation presentation and its resource lifetime. Sky Palace can
 * reuse the globe backdrop without composing the navigation screen's UI. */

#include "present/presentation_outcome.h"
#include "render/render_types.h"
struct FrameSlot;

PresentationOutcome PresentWorldNavigation3D(const struct FrameSlot *slot);
/* Draw into the caller's local viewport without changing output target or
 * viewport. A core failure aborts the scene instead of exposing native pixels. */
PresentationOutcome PresentWorldNavigationBackdrop(
    const struct FrameSlot *slot, ArRenderRectI viewport);
void PresentWorldNav_ResetResources(void);
#endif
