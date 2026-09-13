#include "presentation_view.h"
#include "present_sky_palace.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static int backdrop_calls, foreground_calls;
static bool reject_foreground;
static PresentationOutcome backdrop_outcome;
PresentationOutcome PresentWorldNavigationBackdrop(const FrameSlot *slot, ArRenderRectI viewport) {
  (void)slot; (void)viewport; ++backdrop_calls; return backdrop_outcome;
}
bool ArRenderDevice_DrawTexture(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination) {
  (void)device; (void)source; (void)destination;
  assert(texture.value == 42); /* only the Palace foreground is allowed */
  ++foreground_calls; return !reject_foreground;
}
int main(void) {
  static FrameSlot slot;
  slot.inidisp = 15;
  slot.sim.master_enabled = true;
  slot.sim.view = kSimView_Enhanced;
  slot.sim.requested_features = kSimFeature_SeparatedComposite | kSimFeature_GroundProjection;
  PresentationViewDecision view = PresentationView_Resolve(&slot, kRenderComparison_Enhanced);
  assert(view.scene == kPerformanceScene_Native && view.unexpected_native);
  slot.sim.separated_valid = true;
  slot.sim.effective_features = slot.sim.requested_features;
  view = PresentationView_Resolve(&slot, kRenderComparison_Enhanced);
  assert(view.scene == kPerformanceScene_Town && !view.unexpected_native);
  slot.sim.effective_features = kSimFeature_SeparatedComposite;
  view = PresentationView_Resolve(&slot, kRenderComparison_Enhanced);
  assert(view.scene == kPerformanceScene_Native && !view.unexpected_native);
  slot.sim.effective_features = slot.sim.requested_features;
  slot.sim.view = kSimView_WorldNavigation;
  assert(PresentationView_Resolve(&slot, kRenderComparison_Enhanced).scene == kPerformanceScene_World);
  slot.sim.view = kSimView_Enhanced;
  slot.diorama_active = true;
  assert(PresentationView_Resolve(&slot, kRenderComparison_Enhanced).scene == kPerformanceScene_Action);
  assert(PresentationView_Resolve(&slot, kRenderComparison_Authentic).scene == kPerformanceScene_Native);
  slot.diorama_active = false;
  slot.sim.master_enabled = false;
  view = PresentationView_Resolve(&slot, kRenderComparison_Enhanced);
  assert(view.scene == kPerformanceScene_Native && !view.unexpected_native);
  slot.sim.master_enabled = true;
  slot.sim.view = kSimView_AuthenticFallback;
  for (int blank = 0; blank < 2; blank++) {
    slot.inidisp = blank ? 0x8f : 0;
    assert(!PresentationView_Resolve(&slot, kRenderComparison_Enhanced).unexpected_native);
  }
  slot.inidisp = 7;
  assert(PresentationView_Resolve(&slot, kRenderComparison_Enhanced).unexpected_native);
  assert(!PresentationView_Resolve(&slot, kRenderComparison_Authentic).unexpected_native);
  slot.sim.view = kSimView_SkyPalace;
  assert(PresentationView_Resolve(&slot, kRenderComparison_Enhanced).scene == kPerformanceScene_Palace);
  const ArRenderRectI viewport = {0, 0, 640, 480};
  assert(PresentSkyPalace_Draw(NULL, &slot, viewport, (ArRenderTexture){0}, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  assert(backdrop_calls == 0 && foreground_calls == 0);
  backdrop_outcome = kPresentationOutcome_CoreFailure;
  assert(PresentSkyPalace_Draw(NULL, &slot, viewport, (ArRenderTexture){42}, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  assert(backdrop_calls == 1 && foreground_calls == 0);
  backdrop_outcome = kPresentationOutcome_OptionalOmitted;
  assert(PresentSkyPalace_Draw(NULL, &slot, viewport, (ArRenderTexture){42}, NULL, NULL)
      == kPresentationOutcome_OptionalOmitted);
  assert(foreground_calls == 1);
  backdrop_outcome = kPresentationOutcome_Complete;
  reject_foreground = true;
  assert(PresentSkyPalace_Draw(NULL, &slot, viewport, (ArRenderTexture){42}, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  puts("presentation view: PASS");
  return 0;
}
