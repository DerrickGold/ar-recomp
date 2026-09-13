#include "present_retained_image.h"

#include <string.h>

PresentRetainedImageAction PresentRetainedImage_Choose(PresentRetainedImage *image,
    bool same_image, bool same_view) {
  if (!image || image->unavailable) return kPresentRetainedImage_Render;
  if (image->ready && same_image) {
    image->reused = true;
    return kPresentRetainedImage_Reuse;
  }
  const bool capture = same_view && (same_image || image->reused);
  image->ready = image->reused = false;
  return capture ? kPresentRetainedImage_RenderAndCapture : kPresentRetainedImage_Render;
}

void PresentRetainedImage_Reset(PresentRetainedImage *image, ArRenderDevice *device) {
  if (!image) return;
  ArRenderDevice_DestroyTexture(device, image->texture);
  memset(image, 0, sizeof(*image));
}

PresentationOutcome PresentRetainedImage_Capture(PresentRetainedImage *image,
    ArRenderDevice *device, ArRenderTexture source, int width, int height) {
  if (!image) return kPresentationOutcome_OptionalOmitted;
  image->ready = false;
  if (image->unavailable || width <= 0 || height <= 0 ||
      !ArRenderTexture_IsValid(source) || ArRenderTexture_Equals(source, image->texture))
    return kPresentationOutcome_OptionalOmitted;
  if (image->width != width || image->height != height)
    PresentRetainedImage_Reset(image, device);
  if (!ArRenderTexture_IsValid(image->texture)) {
    const ArRenderTextureDesc desc = {
      .width = width, .height = height, .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Target, .filter = kArRenderFilter_Linear,
      .blend = kArRenderBlendMode_AlphaPremultiplied,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, &image->texture)) {
      image->unavailable = true;
      return kPresentationOutcome_OptionalOmitted;
    }
    image->width = width; image->height = height;
  }
  ArRenderTargetState saved = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(device, image->texture, &saved);
  if (begin != kArRenderTargetBegin_Ready) {
    image->unavailable = true;
    return begin == kArRenderTargetBegin_StateLost
        ? kPresentationOutcome_CoreFailure : kPresentationOutcome_OptionalOmitted;
  }
  const ArRenderRectF destination = {0, 0, width, height};
  /* A one-to-one opaque copy preserves premultiplied RGB AND alpha, including
   * transparent edge pixels. Never blend against the previous cache image. */
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend | kArRenderDrawState_Tint,
    .blend = kArRenderBlendMode_Opaque, .tint = {1,1,1,1},
  };
  const bool copied = ArRenderDevice_DrawTextureWithState(device, source, NULL, &destination, &state);
  if (!ArRenderDevice_EndTarget(device, &saved)) return kPresentationOutcome_CoreFailure;
  image->ready = copied;
  image->unavailable = !copied;
  return copied ? kPresentationOutcome_Complete : kPresentationOutcome_OptionalOmitted;
}
