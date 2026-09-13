#ifndef AR_PRESENT_SKY_PALACE_H
#define AR_PRESENT_SKY_PALACE_H

#include "present.h"
#include "presentation_outcome.h"

/* The selected Palace is one complete scene. Neither missing foreground nor
 * a rejected backdrop may be replaced by the native game framebuffer. */
PresentationOutcome PresentSkyPalace_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    ArRenderTexture foreground, const ArRenderRectF *source,
    const ArRenderRectF *destination);

#endif
