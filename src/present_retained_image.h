#ifndef AR_PRESENT_RETAINED_IMAGE_H
#define AR_PRESENT_RETAINED_IMAGE_H

#include "render/render_device.h"
#include "presentation_outcome.h"

/* Presentation-owned snapshot of a borrowed GPU composite. The caller owns
 * its semantic cache key and must reset before its render device is destroyed.
 * No readback, resampling or scene-specific policy crosses the render seam. */
typedef struct PresentRetainedImage {
  ArRenderTexture texture;
  int width, height;
  bool ready, unavailable, reused;
} PresentRetainedImage;

typedef enum PresentRetainedImageAction {
  kPresentRetainedImage_Render, kPresentRetainedImage_RenderAndCapture,
  kPresentRetainedImage_Reuse,
} PresentRetainedImageAction;

/* Caller certifies image/view equality from its owned semantic keys. A repeat
 * or a previously reused image pays for retention; continuously changing
 * views/images bypass copies after at most one failed prediction. */
PresentRetainedImageAction PresentRetainedImage_Choose(PresentRetainedImage *image,
    bool same_image, bool same_view);

PresentationOutcome PresentRetainedImage_Capture(PresentRetainedImage *image,
    ArRenderDevice *device, ArRenderTexture source, int width, int height);
void PresentRetainedImage_Reset(PresentRetainedImage *image, ArRenderDevice *device);

#endif
