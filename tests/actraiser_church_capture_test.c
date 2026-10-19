#include "actraiser/actraiser_church_capture.h"
#include "actraiser_game.h"
#include "support/test_assert.h"

#include <stdio.h>
#include <string.h>

static SrPpuObjCaptureRequest s_request;
static SrPpuFrameTransactionContext s_frame;
static unsigned s_requests;
static bool s_reject;
static SrRunnerHandle *const s_runner = (SrRunnerHandle *)(uintptr_t)1;

static SrResult Configure(SrRunnerHandle *runner, const SrPpuObjCaptureRequest *request) {
  assert(runner == s_runner);
  assert(request->flags == SR_PPU_OBJ_CAPTURE_WINNERS);
  assert(request->range_x == 96 && request->range_y == 72);
  assert(request->range_width == 64 && request->range_height == 32);
  assert(request->range_pitch_bytes == s_frame.overlays[SR_PPU_OVERLAY_OBJ].pitch_bytes);
  assert(request->range_pixel_byte_size >= request->range_pitch_bytes * 104);
  s_request = *request;
  s_requests++;
  memset(request->range_pixels, 0, request->range_pixel_byte_size);
  return s_reject ? SR_RESULT_INVALID_ARGUMENT : SR_RESULT_OK;
}

static const SnesRunnerApi s_api = {
    .struct_size = sizeof(SnesRunnerApi),
    .configure_ppu_obj_capture = Configure,
};

static ActRaiserSpriteOwnership Audience(unsigned parts) {
  const uint8_t shadow[kActRaiserSpriteShadowBytes] = {0};
  ActRaiserSpriteOwnership_Begin(0, 8, 1);
  ActRaiserSpriteOwnership_RecordSim(0x081a, 0x23, 0, 16);
  ActRaiserSpriteOwnership_RecordSim(0x082c, 0x24, 16, 32);
  ActRaiserSpriteOwnership_RecordSim(0x0f0c, 5, 32, 32 + parts * 4);
  ActRaiserSpriteOwnership_RecordSim(0x0f32, 6, 32 + parts * 4, 32 + parts * 8);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 8, shadow);
  return ActRaiserSpriteOwnership_Presented(0, 8);
}

int main(void) {
  SrPpuScanoutResult result = {.struct_size = sizeof(result), .lifetime_generation = 7};
  s_frame.lifetime_generation = 7;
  /* Native, widescreen and apron-backed surfaces must yield identical masks.
   * Multipart artwork can also change counts without changing the block list. */
  const unsigned origins[] = {0, 43, 64, 43 + 64, 128 + 64};
  for (unsigned geometry = 0; geometry < sizeof(origins) / sizeof(origins[0]); geometry++)
    for (unsigned parts = 1; parts <= 4; parts++) {
      const unsigned origin = origins[geometry], width = 256 + 2 * origin;
      s_frame.overlays[SR_PPU_OVERLAY_OBJ] = (SrPpuWritableSurfaceView){
          .origin_x = origin, .width_pixels = width, .pitch_bytes = width * sizeof(uint32_t)};
      ActRaiserSpriteOwnership ownership = Audience(parts);
      for (unsigned slot = 0; slot < 8; slot++)
        assert(ownership.slots[slot] == kActRaiserSprite_Unowned);
      ActRaiserChurchCapture_BeginFrame();
      ActRaiserChurchCapture_Prepare(&s_api, s_runner, &s_frame, &ownership);
      assert(s_request.range_first == 8 && s_request.range_count == parts * 2);
      assert(!ActRaiserChurchCapture_Mask().valid);
      uint32_t *pixels = (uint32_t *)s_request.range_pixels;
      pixels[73 * width + origin + 104] = 0xffeecc88;
      pixels[89 * width + origin + 151] = 0xffeecc88;
      pixels[103 * width + origin + 159] = 0xff000000; /* Opaque black is still owned. */
      pixels[88 * width + origin + 152] = 0x00eecc88;  /* Transparent RGB is not a winner. */
      ActRaiserChurchCapture_Complete(SR_RESULT_OK, &result);
      ChurchActorMask mask = ActRaiserChurchCapture_Mask();
      assert(mask.valid && mask.lifetime_generation == 7);
      for (unsigned actor = 0; actor < 2; actor++)
        for (unsigned y = 0; y < 32; y++) {
          const uint32_t expected = actor == 0 && y == 1    ? 1u << 8
                                    : actor == 1 && y == 17 ? 1u << 23
                                    : actor == 1 && y == 31 ? 1u << 31
                                                            : 0;
          assert(mask.rows[actor][y] == expected);
        }
      ActRaiserChurchCapture_BeginFrame();
      assert(!ActRaiserChurchCapture_Mask().valid);
    }

  /* No visitor means an empty block list, including after a populated frame. */
  ActRaiserSpriteOwnership empty = Audience(0);
  const unsigned requests = s_requests;
  ActRaiserChurchCapture_Prepare(&s_api, s_runner, &s_frame, &empty);
  ActRaiserChurchCapture_Complete(SR_RESULT_OK, &result);
  ChurchActorMask mask = ActRaiserChurchCapture_Mask();
  const uint32_t zero[2][32] = {{0}};
  assert(mask.valid && !memcmp(mask.rows, zero, sizeof(zero)) && s_requests == requests);

  /* Invalid/stale ownership, a gap containing UI, and failed scanout all
   * leave the optional 3D scene unavailable instead of guessing at pixels. */
  for (unsigned failure = 0; failure < 10; failure++) {
    ActRaiserSpriteOwnership ownership = Audience(2);
    s_frame.overlays[SR_PPU_OVERLAY_OBJ] =
        (SrPpuWritableSurfaceView){.width_pixels = 256, .pitch_bytes = 256 * sizeof(uint32_t)};
    ActRaiserChurchCapture_BeginFrame();
    if (failure == 0) ownership.valid = false;
    if (failure == 1) ownership.slots[9] = kActRaiserSprite_Unowned;
    if (failure == 2) ownership.map = 1;
    if (failure == 6)
      s_frame.overlays[SR_PPU_OVERLAY_OBJ].width_pixels = SR_PPU_SURFACE_MAX_WIDTH + 1;
    if (failure == 7) s_frame.overlays[SR_PPU_OVERLAY_OBJ].origin_x = 1;
    if (failure == 8) s_frame.overlays[SR_PPU_OVERLAY_OBJ].pitch_bytes++;
    s_reject = failure == 3;
    ActRaiserChurchCapture_Prepare(&s_api, s_runner, failure == 9 ? NULL : &s_frame, &ownership);
    result.lifetime_generation = failure == 4 ? 8 : 7;
    ActRaiserChurchCapture_Complete(failure == 5 ? SR_RESULT_INVALID_ARGUMENT : SR_RESULT_OK,
                                    &result);
    assert(!ActRaiserChurchCapture_Mask().valid);
  }
  /* The same world record in town is not a church visitor. */
  const uint8_t shadow[kActRaiserSpriteShadowBytes] = {0};
  ActRaiserSpriteOwnership_Begin(0, 1, 1);
  ActRaiserSpriteOwnership_RecordSim(0x0f0c, 5, 0, 8);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  assert(ActRaiserSpriteOwnership_Presented(0, 1).slots[0] == kActRaiserSprite_Unowned);
  puts("church sprite block list: native owners, winner masks, publication and fallback passed");
  return 0;
}
