#ifndef AR_PRESENT_HD_REPLACEMENTS_H
#define AR_PRESENT_HD_REPLACEMENTS_H
/* HD artwork composition from captured frame values and borrowed host textures.
 * Screen substitutions follow native erasure bounds, crop and master brightness;
 * affine substitutions use the runner's already-resolved Mode-7 surface.
 * The frame sequencer chooses when each layer is drawn. */

#include "render/render_device.h"
struct FrameSlot;

/* Reports successful transfer bytes; invalid or failed uploads report zero. */
uint64_t PresentHdReplacements_UploadMode7(
    ArRenderDevice *device, ArRenderTexture texture, const struct FrameSlot *slot);
void PresentHdReplacements_DrawMode7(
    ArRenderDevice *device, ArRenderTexture texture, const struct FrameSlot *slot,
    ArRenderRectI viewport);
void PresentHdReplacements_DrawScreen(
    ArRenderDevice *device, const struct FrameSlot *slot, ArRenderRectI viewport);
#endif
