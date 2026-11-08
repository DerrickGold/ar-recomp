#ifndef AR_PRESENT_INTERNAL_H
#define AR_PRESENT_INTERNAL_H
/* Private scene/host-UI stages shared by present.c and present_frame.c.
 * Feature renderers declare their own entry points in their owning folders.
 * Every game-state decision arrives through the completed FrameSlot. */

#include <stdbool.h>

#include "present/present.h"
#include "render/presentation_options.h"
#include "render/render_types.h"

void PresentCompositeScene(const FrameSlot *slot, float alpha);
bool PresentBlankScene(void);
bool PresentAuthenticScene(const FrameSlot *slot, ArRenderRectI viewport);
/* Draw the authentic comparison and native help into the caller's current
 * output target. The caller owns its clear, frame and letterboxing. */
bool PresentAuthenticComparisonImage(const FrameSlot *slot,
                                     ArRenderRectI viewport);
bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      ArRenderRectI priority_viewport,
                                      PictureInPictureCorner corner);
bool PresentComparisonTransitionOverlay(uint8_t alpha, const char *label);
void PresentHostUi(const FrameSlot *slot, ArRenderRectI viewport,
                   ArRenderExtentI output_size,
                   double presentation_fps);

#endif /* AR_PRESENT_INTERNAL_H */
