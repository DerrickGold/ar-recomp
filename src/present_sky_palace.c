#include "present_sky_palace.h"
#include "present_internal.h"

PresentationOutcome PresentSkyPalace_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    ArRenderTexture foreground, const ArRenderRectF *source,
    const ArRenderRectF *destination) {
  if (!slot || slot->sim.view != kSimView_SkyPalace ||
      !ArRenderTexture_IsValid(foreground))
    return kPresentationOutcome_CoreFailure;
  const PresentationOutcome outcome = PresentWorldNavigationBackdrop(slot, viewport);
  if (!PresentationOutcome_IsUsable(outcome)) return outcome;
  if (!ArRenderDevice_DrawTexture(device, foreground, source, destination))
    return kPresentationOutcome_CoreFailure;
  return outcome;
}
