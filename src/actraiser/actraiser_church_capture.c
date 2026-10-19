#include "actraiser/actraiser_church_capture.h"
#include "actraiser_game.h"

#include <string.h>

/* Winner capture uses the runner's margin/apron origin even with independent
 * storage. Match the OBJ surface geometry before reducing it to native cells. */
static uint32_t s_winners[SR_PPU_SURFACE_MAX_WIDTH * (kChurchActorY + kChurchActorSize)];
static ChurchActorMask s_pending, s_completed;
static unsigned s_width, s_origin;

void ActRaiserChurchCapture_BeginFrame(void) { s_pending = s_completed = (ChurchActorMask){0}; }

void ActRaiserChurchCapture_Prepare(const SnesRunnerApi *api, SrRunnerHandle *runner,
                                    const SrPpuFrameTransactionContext *frame,
                                    const ActRaiserSpriteOwnership *ownership) {
  if (!frame || !ownership || !ownership->valid ||
      ownership->group != kActRaiserMapGroup_NonAction ||
      ownership->map != kActRaiserNonActionMap_Temple)
    return;
  const SrPpuWritableSurfaceView *objects = &frame->overlays[SR_PPU_OVERLAY_OBJ];
  if (objects->width_pixels < kActRaiserAuthenticWidth ||
      objects->width_pixels > SR_PPU_SURFACE_MAX_WIDTH ||
      objects->pitch_bytes != objects->width_pixels * sizeof(uint32_t) || objects->origin_x < 0 ||
      (unsigned)objects->origin_x > objects->width_pixels - kActRaiserAuthenticWidth)
    return;
  s_width = objects->width_pixels;
  s_origin = (unsigned)objects->origin_x;
  uint8_t first, count;
  if (!ActRaiserSpriteOwnership_Range(ownership, kActRaiserSprite_ChurchVisitor, &first, &count)) {
    /* An empty audience is valid. Disjoint ranges must never swallow a menu
     * sprite between visitors; let the native scene handle that frame. */
    for (unsigned slot = 0; slot < kActRaiserSpriteSlots; slot++)
      if (ownership->slots[slot] == kActRaiserSprite_ChurchVisitor) return;
    memset(s_winners, 0, sizeof(s_winners));
  } else {
    if (!api || !runner || api->struct_size < SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE ||
        !api->configure_ppu_obj_capture)
      return;
    const SrPpuObjCaptureRequest request = {
        .struct_size = sizeof(request),
        .flags = SR_PPU_OBJ_CAPTURE_WINNERS,
        .lifetime_generation = frame->lifetime_generation,
        .range_first = first,
        .range_count = count,
        .range_x = kChurchActorX,
        .range_y = kChurchActorY,
        .range_width = kChurchActorCount * kChurchActorSize,
        .range_height = kChurchActorSize,
        .range_pixels = (uint8_t *)s_winners,
        .range_pixel_byte_size = sizeof(s_winners),
        .range_pitch_bytes = objects->pitch_bytes,
    };
    if (api->configure_ppu_obj_capture(runner, &request) != SR_RESULT_OK) return;
  }
  s_pending = (ChurchActorMask){.valid = true, .lifetime_generation = frame->lifetime_generation};
}

void ActRaiserChurchCapture_Complete(SrResult status, const SrPpuScanoutResult *result) {
  if (!s_pending.valid || status != SR_RESULT_OK || !result ||
      result->lifetime_generation != s_pending.lifetime_generation)
    return;
  for (int actor = 0; actor < kChurchActorCount; actor++)
    for (int y = 0; y < kChurchActorSize; y++)
      for (int x = 0; x < kChurchActorSize; x++) {
        const uint32_t pixel = s_winners[(kChurchActorY + y) * s_width + s_origin + kChurchActorX +
                                         actor * kChurchActorSize + x];
        if (pixel >> 24) s_pending.rows[actor][y] |= UINT32_C(1) << x;
      }
  s_completed = s_pending;
}

ChurchActorMask ActRaiserChurchCapture_Mask(void) { return s_completed; }
