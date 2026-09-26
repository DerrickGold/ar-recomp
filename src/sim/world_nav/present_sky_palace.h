#ifndef AR_PRESENT_SKY_PALACE_H
#define AR_PRESENT_SKY_PALACE_H

#include "present/present.h"
#include "present/presentation_outcome.h"

/* Consume the native frame and sky winner mask before releasing producer
 * pixels. Every upload invalidates stale foreground, even without a device.
 * Retained draws use only the published GPU texture. */
void PresentSkyPalace_Upload(ArRenderDevice *device, const FrameSlot *slot);
bool PresentSkyPalace_ForegroundReady(void);
void PresentSkyPalace_Reset(ArRenderDevice *device);

/* The selected Palace is one complete scene. Neither missing foreground nor
 * a rejected backdrop may be replaced by the native game framebuffer. */
PresentationOutcome PresentSkyPalace_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    const ArRenderRectF *source,
    const ArRenderRectF *destination);

#endif
