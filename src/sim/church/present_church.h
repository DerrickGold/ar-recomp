#ifndef AR_PRESENT_CHURCH_H
#define AR_PRESENT_CHURCH_H
#include "present/present.h"
#include "present/presentation_outcome.h"
/* Optional interior presentation cache, consumed on the render owner. */
void PresentChurch_Upload(ArRenderDevice *device, const FrameSlot *slot);
bool PresentChurch_Active(const FrameSlot *slot);
PresentationOutcome PresentChurch_Draw(ArRenderDevice *device, const FrameSlot *slot,
                                       ArRenderRectI viewport);
ArRenderTexture PresentChurch_UiBackdrop(void);
/* Place comparison above the church dialogue, clear of the right-hand menu. */
void PresentChurch_PlaceComparison(const FrameSlot *slot, ArRenderRectI viewport,
                                   int margin, int frame_size, ArRenderRectF *destination);
/* Device/target loss retains the immutable town; session end releases it. */
void PresentChurch_ResetResources(ArRenderDevice *device);
void PresentChurch_Reset(ArRenderDevice *device);
#endif
