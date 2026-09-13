#include "present_retained_image.h"
/* Keep checks and fixture setup active in release-configured test builds. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static int created, destroyed, drawn, begun, ended;
static bool fail_create, fail_draw, fail_restore;
static ArRenderTargetBeginResult begin_result;

bool ArRenderDevice_CreateTexture(ArRenderDevice *device,
    const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)device;
  assert(desc->usage == kArRenderTextureUsage_Target);
  assert(desc->filter == kArRenderFilter_Linear);
  ++created;
  if (fail_create) return false;
  *out = (ArRenderTexture){100 + created}; return true;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device; destroyed += ArRenderTexture_IsValid(texture);
}
ArRenderTargetBeginResult ArRenderDevice_BeginTarget(ArRenderDevice *device,
    ArRenderTexture texture, ArRenderTargetState *state) {
  (void)device; (void)texture; ++begun;
  state->valid = begin_result == kArRenderTargetBegin_Ready;
  return begin_result;
}
bool ArRenderDevice_EndTarget(ArRenderDevice *device, const ArRenderTargetState *state) {
  (void)device; assert(state->valid); ++ended; return !fail_restore;
}
bool ArRenderDevice_DrawTextureWithState(ArRenderDevice *device,
    ArRenderTexture texture, const ArRenderRectF *source,
    const ArRenderRectF *destination, const ArRenderDrawState *state) {
  (void)device; (void)texture;
  assert(!source && destination->x == 0 && destination->y == 0);
  assert(state->blend == kArRenderBlendMode_Opaque);
  assert(state->tint.r == 1 && state->tint.g == 1 && state->tint.b == 1 && state->tint.a == 1);
  ++drawn; return !fail_draw;
}

int main(void) {
  ArRenderDevice device = {0};
  const ArRenderTexture source = {1};
  PresentRetainedImage image = {0};
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_Complete);
  assert(image.ready && created == 1 && drawn == 1 && begun == 1 && ended == 1);
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_Complete);
  assert(created == 1 && drawn == 2 && ended == 2);
  assert(PresentRetainedImage_Capture(&image,&device,source,64,32) == kPresentationOutcome_Complete);
  assert(created == 2 && destroyed == 1 && image.width == 64);
  assert(PresentRetainedImage_Capture(&image,&device,image.texture,64,32) == kPresentationOutcome_OptionalOmitted);
  assert(!image.ready && drawn == 3);
  fail_draw = true;
  assert(PresentRetainedImage_Capture(&image,&device,source,64,32) == kPresentationOutcome_OptionalOmitted);
  assert(!image.ready && image.unavailable && begun == ended);
  const int draws = drawn;
  assert(PresentRetainedImage_Capture(&image,&device,source,64,32) == kPresentationOutcome_OptionalOmitted);
  assert(drawn == draws);
  PresentRetainedImage_Reset(&image,&device); fail_draw = false;
  fail_create = true;
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_OptionalOmitted);
  assert(!image.ready && image.unavailable);
  PresentRetainedImage_Reset(&image,&device); fail_create = false;
  begin_result = kArRenderTargetBegin_Omitted;
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_OptionalOmitted);
  PresentRetainedImage_Reset(&image,&device);
  begin_result = kArRenderTargetBegin_StateLost;
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_CoreFailure);
  PresentRetainedImage_Reset(&image,&device);
  begin_result = kArRenderTargetBegin_Ready; fail_restore = true;
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_CoreFailure);
  assert(!image.ready);
  PresentRetainedImage_Reset(&image,&device);
  fail_restore = false;
  assert(PresentRetainedImage_Choose(&image,false,false) == kPresentRetainedImage_Render);
  assert(PresentRetainedImage_Choose(&image,true,true) == kPresentRetainedImage_RenderAndCapture);
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_Complete);
  assert(PresentRetainedImage_Choose(&image,true,true) == kPresentRetainedImage_Reuse);
  assert(PresentRetainedImage_Choose(&image,false,true) == kPresentRetainedImage_RenderAndCapture);
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_Complete);
  const int captures = drawn;
  for (int frame = 0; frame < 300; ++frame) {
    assert(PresentRetainedImage_Choose(&image,false,true) == kPresentRetainedImage_Render);
    assert(PresentRetainedImage_Choose(&image,false,false) == kPresentRetainedImage_Render);
  }
  assert(drawn == captures && !image.ready);
  assert(PresentRetainedImage_Choose(&image,true,true) == kPresentRetainedImage_RenderAndCapture);
  assert(PresentRetainedImage_Capture(&image,&device,source,32,16) == kPresentationOutcome_Complete);
  assert(PresentRetainedImage_Choose(&image,true,true) == kPresentRetainedImage_Reuse);
  PresentRetainedImage_Reset(&image,&device);
  puts("retained image policy: reuse, moving view, continuous changes, bounded misprediction and recovery passed");
  puts("retained image: copy, reuse, resize, rejection, state-loss and reset passed");
}
