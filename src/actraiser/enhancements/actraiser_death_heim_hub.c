/* ActRaiser Death Heim hub: diorama passes for the Death Heim hub statues:
 * which eye pixels their sprites win, and promoting the statue faces.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"
#include "diorama/diorama.h"
#include "host/host_frame_surfaces.h"

/* Native actor ownership identifies the eyes. Scanout records which pixels
 * their actual OAM slots win, including overlaps with identical colours. */
enum { kDeathHeimHubFaceRows = 9 * 16 };
static uint32_t s_death_heim_hub_eye_winners[
    kActRaiserAuthenticWidth * kActRaiserAuthenticHeight];
static bool s_death_heim_hub_eyes_ready;

void ActRaiser_DioramaDeathHeimEyesPrepare(void) {
  s_death_heim_hub_eyes_ready = false;
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !g_diorama_frame_active) return;
  const ActRaiserSpriteOwnership ownership = ActRaiserSpriteOwnership_Presented(
      g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap]);
  uint8_t first, count;
  if (!ActRaiserSpriteOwnership_Range(&ownership, kActRaiserSprite_StatueEyes,
                                     &first, &count)) return;
  const SrPpuObjCaptureRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_OBJ_CAPTURE_WINNERS,
    .lifetime_generation = frame->lifetime_generation,
    .range_first = first, .range_count = count,
    .range_width = kActRaiserAuthenticWidth,
    .range_height = kActRaiserAuthenticHeight,
    .range_pixels = (uint8_t *)s_death_heim_hub_eye_winners,
    .range_pixel_byte_size = sizeof(s_death_heim_hub_eye_winners),
    .range_pitch_bytes = kActRaiserAuthenticWidth * sizeof(uint32_t),
  };
  s_death_heim_hub_eyes_ready = ActRaiser_ConfigurePpuObjCapture(&request);
}

static bool s_death_heim_hub_faces_promoted;

bool ActRaiser_DioramaDeathHeimHubFacesPromoted(void) {
  return s_death_heim_hub_faces_promoted;
}

void ActRaiser_DioramaDeathHeimHubStatuesFinish(int width) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();

  s_death_heim_hub_faces_promoted = false;
  if (!frame || !g_diorama_frame_active ||
      g_ram[kActRaiserWram_MapGroup] != kActRaiserMapGroup_DeathHeim ||
      g_ram[kActRaiserWram_CurrentMap] != kActRaiserDeathHeimMap_Hub ||
      width < kActRaiserAuthenticWidth)
    return;

  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  if (!room || !DioramaLayerOrder_VirtualLayerHasClassification(
                   &room->virtual_layers[1]))
    return;

  uint32_t *bg2 =
      (uint32_t *)g_diorama_layer_pixels[SR_PPU_OVERLAY_BG2];
  uint32_t *bg2_hi =
      (uint32_t *)g_diorama_layer_pixels[kDioramaPlane_Bg2Hi];
  uint32_t *faces =
      (uint32_t *)g_diorama_layer_pixels[kDioramaPlane_Bg2Far];
  if (!bg2 || !bg2_hi || !faces) return;

  const int extra = (width - kActRaiserAuthenticWidth) / 2;
  const ActionApronGeometry geom = { extra, SR_PPU_OBJ_APRON };
  const int plane_width = ActionApron_SurfaceWidth(&geom);
  /* 0701 is deliberately native-only (docs/rendering-engine.md): its BG2
   * cannot acquire virtual-band metadata from the world provider. Perform the
   * same authored cell split on the already-isolated pixels instead. Rows
   * 0..8 are statues/faces; row 9 begins the divider/fog/water. Both authentic
   * priority bands feed one focal plane because the distinction here is
   * semantic depth, not SNES paint order. */
  uint32_t *sources[] = { bg2, bg2_hi };
  for (size_t source = 0; source < sizeof(sources) / sizeof(sources[0]);
       source++) {
    for (int screen_y = 0; screen_y < kDeathHeimHubFaceRows; screen_y++) {
      const size_t row =
          (size_t)(screen_y + g_ws_extra_top) * plane_width;
      for (int screen_x = 0; screen_x < kActRaiserAuthenticWidth;
           screen_x++) {
        const int column = ActionApron_SurfaceColumn(&geom, screen_x);
        if (column < 0 || column >= plane_width) continue;
        const size_t index = row + column;
        const uint32_t pixel = sources[source][index];
        if (!pixel) continue;
        faces[index] = pixel;
        sources[source][index] = 0;
        s_death_heim_hub_faces_promoted = true;
      }
    }
  }

  if (!s_death_heim_hub_eyes_ready) return;
  for (int y = 0; y < kActRaiserAuthenticHeight; ++y) {
    for (int x = 0; x < kActRaiserAuthenticWidth; ++x) {
      if (!s_death_heim_hub_eye_winners[y * kActRaiserAuthenticWidth + x])
        continue;
      const size_t index = (size_t)(y + g_ws_extra_top) * plane_width +
          ActionApron_SurfaceColumn(&geom, x);
      /* Use the captured winner's band, allowing artwork to change priority.
       * The mask supplies identity; the plane supplies its final colour/math. */
      for (int priority = 0; priority < 4; ++priority) {
        uint32_t *plane = (uint32_t *)g_diorama_layer_pixels[
            ActRaiser_DioramaObjPlaneForPriority(priority)];
        if (plane && plane[index]) {
          faces[index] = plane[index];
          plane[index] = 0;
        }
      }
    }
  }
}
