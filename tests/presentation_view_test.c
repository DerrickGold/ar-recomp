#include "present/presentation_view.h"
#include "sim/world_nav/present_sky_palace.h"
#include "support/test_assert.h"
#include <stdio.h>

static int backdrop_calls, foreground_calls;
static bool reject_foreground;
static PresentationOutcome backdrop_outcome;
PresentationOutcome PresentWorldNavigationBackdrop(const FrameSlot *slot, ArRenderRectI viewport) {
  (void)slot; (void)viewport; ++backdrop_calls; return backdrop_outcome;
}
static bool fail_create, fail_upload;
static int texture_created, texture_destroyed;
static uint32_t uploaded_first;
static bool Create(void *ctx, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)ctx;
  assert(desc->usage == kArRenderTextureUsage_Streaming);
  if (fail_create) return false;
  *out = (ArRenderTexture){42};
  texture_created++;
  return true;
}
static void Destroy(void *ctx, ArRenderTexture texture) {
  (void)ctx;
  assert(texture.value == 42);
  texture_destroyed++;
}
static bool Update(void *ctx, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  (void)ctx;
  assert(texture.value == 42 && rect->w == 16 && rect->h == 8 && pitch >= 16 * 4);
  if (fail_upload) return false;
  uploaded_first = *(const uint32_t *)pixels;
  return true;
}
static bool Draw(void *ctx, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *destination, const ArRenderDrawState *state) {
  (void)ctx;
  (void)source;
  (void)destination;
  (void)state;
  assert(texture.value == 42);
  ++foreground_calls;
  return !reject_foreground;
}
static const ArRenderBackendOps ops = {
  .struct_size = sizeof(ops), .create_texture = Create, .destroy_texture = Destroy,
  .update_texture = Update, .draw_texture = Draw,
};
static uint32_t native_pixels[16 * 8], mask_pixels[16 * 8];
static SrPpuSurfaceView Surface(uint32_t *pixels) {
  return (SrPpuSurfaceView){.data = (uint8_t *)pixels, .byte_size = sizeof(native_pixels),
    .pitch_bytes = 16 * 4, .width_pixels = 16, .height_pixels = 8,
    .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32};
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
  ArRenderDevice device = {.ops = &ops, .context = &texture_created,
      .capabilities = {.flags = kArRenderCapability_StreamingTextures}};
  slot.snes_width = 16;
  slot.snes_height = 8;
  slot.ppu_surfaces.main = Surface(native_pixels);
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0] = Surface(mask_pixels);
  for (int i = 0; i < 16 * 8; i++) mask_pixels[i] = 0xff000000;
  native_pixels[0] = 0x123456;
  const ArRenderRectI viewport = {0, 0, 640, 480};
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  assert(backdrop_calls == 0 && foreground_calls == 0);
  fail_create = true;
  PresentSkyPalace_Upload(&device, &slot);
  assert(!PresentSkyPalace_ForegroundReady());
  fail_create = false;
  PresentSkyPalace_Upload(&device, &slot);
  assert(PresentSkyPalace_ForegroundReady() && uploaded_first == 0xff123456);
  backdrop_outcome = kPresentationOutcome_CoreFailure;
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  assert(backdrop_calls == 1 && foreground_calls == 0);
  backdrop_outcome = kPresentationOutcome_OptionalOmitted;
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_OptionalOmitted);
  assert(foreground_calls == 1);
  backdrop_outcome = kPresentationOutcome_Complete;
  reject_foreground = true;
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  reject_foreground = false;
  native_pixels[0] = 0x654321;
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_Complete);
  assert(uploaded_first == 0xff123456); /* Retained drawing never re-reads producer pixels. */
  fail_upload = true;
  PresentSkyPalace_Upload(&device, &slot);
  assert(!PresentSkyPalace_ForegroundReady());
  assert(PresentSkyPalace_Draw(&device, &slot, viewport, NULL, NULL)
      == kPresentationOutcome_CoreFailure);
  fail_upload = false;
  PresentSkyPalace_Upload(&device, &slot);
  assert(PresentSkyPalace_ForegroundReady() && uploaded_first == 0xff654321);
  mask_pixels[0] = 123; /* Arbitrary overlays are not valid winner masks. */
  PresentSkyPalace_Upload(&device, &slot);
  assert(!PresentSkyPalace_ForegroundReady());
  mask_pixels[0] = 0xffffffff;
  PresentSkyPalace_Upload(&device, &slot);
  assert(PresentSkyPalace_ForegroundReady() && uploaded_first == 0);
  PresentSkyPalace_Upload(NULL, &slot);
  assert(!PresentSkyPalace_ForegroundReady());
  PresentSkyPalace_Upload(&device, &slot);
  slot.sim.view = kSimView_WorldNavigation;
  PresentSkyPalace_Upload(&device, &slot);
  assert(!PresentSkyPalace_ForegroundReady());
  PresentSkyPalace_Reset(&device);
  assert(texture_created == texture_destroyed);
  puts("presentation view: selection and Palace foreground lifecycle passed");
  return 0;
}
