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
    SR_PPU_SURFACE_MAX_WIDTH * kActRaiserAuthenticHeight];
static bool s_death_heim_hub_eyes_ready;
static int s_death_heim_hub_eye_pitch;

static bool CompletionScene(const SrPpuFrameTransactionContext *frame) {
  return ActRaiser_IsDeathHeimCompletionScene(
      g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap],
      g_ram[kActRaiserWram_DeathHeimProgress],
      frame->state.background_tilemap_control[0],
      frame->state.background_tilemap_control[1]);
}

void ActRaiser_DioramaDeathHeimEyesPrepare(void) {
  s_death_heim_hub_eyes_ready = false;
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !g_diorama_frame_active || CompletionScene(frame)) return;
  const ActRaiserSpriteOwnership ownership = ActRaiserSpriteOwnership_Presented(
      g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap]);
  uint8_t first, count;
  if (!ActRaiserSpriteOwnership_Range(&ownership, kActRaiserSprite_StatueEyes,
                                     &first, &count)) return;
  /* Winner capture shares the scene surface's horizontal origin, including
   * its margin budget and object apron. A 256-wide mask shifts every winner
   * right by the runtime's margin origin and can spill into the next row. */
  s_death_heim_hub_eye_pitch = kActRaiserAuthenticWidth +
      2 * (g_ws_extra + SR_PPU_OBJ_APRON);
  if ((unsigned)s_death_heim_hub_eye_pitch > SR_PPU_SURFACE_MAX_WIDTH) return;
  const SrPpuObjCaptureRequest request = {
    .struct_size = sizeof(request),
    .flags = SR_PPU_OBJ_CAPTURE_WINNERS,
    .lifetime_generation = frame->lifetime_generation,
    .range_first = first, .range_count = count,
    .range_width = kActRaiserAuthenticWidth,
    .range_height = kActRaiserAuthenticHeight,
    .range_pixels = (uint8_t *)s_death_heim_hub_eye_winners,
    .range_pixel_byte_size = sizeof(s_death_heim_hub_eye_winners),
    .range_pitch_bytes = s_death_heim_hub_eye_pitch * sizeof(uint32_t),
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
      CompletionScene(frame) ||
      width < kActRaiserAuthenticWidth)
    return;

  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  const bool classified = room && room->section == kDioramaLayerSection_Room &&
      DioramaLayerOrder_VirtualLayerHasClassification(&room->virtual_layers[1]);
  if (!classified && g_settings.diorama_skybox == kDioramaSky_Off)
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
  /* Native capture now classifies the selected $0701 page directly. Retain
   * the old face promotion only as a fallback when that capture is absent;
   * otherwise it would overwrite individually authored depth bands. */
  const bool native_edits = classified && ActRaiserActionBg_CaptureTilesBound(1);
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
        if (native_edits) {
          if (faces[index]) s_death_heim_hub_faces_promoted = true;
          continue;
        }
        const uint32_t pixel = sources[source][index];
        if (!pixel) continue;
        faces[index] = pixel;
        sources[source][index] = 0;
        s_death_heim_hub_faces_promoted = true;
      }
    }
  }

  if (!s_death_heim_hub_eyes_ready || s_death_heim_hub_eye_pitch != plane_width) return;
  for (int y = 0; y < kActRaiserAuthenticHeight; ++y) {
    for (int x = 0; x < kActRaiserAuthenticWidth; ++x) {
      const int column = ActionApron_SurfaceColumn(&geom, x);
      if (!s_death_heim_hub_eye_winners[y * plane_width + column])
        continue;
      if (native_edits) {
        uint16_t entry;
        uint8_t band, local_x, local_y;
        bool black, blank;
        /* R8 keeps the face rows at zero scroll. Move an eye only when its
         * underlying face tile is still authored into the far band. */
        const bool stamped = ActRaiserActionBg_StampAt(1, x, y + 1, 0, 0,
            &entry, &band, &local_x, &local_y, &black, &blank);
        if ((stamped ? blank : !ActRaiserActionBg_NativeSceneryAt(1, x, y + 1, 0, 0,
                &entry, &band, &local_x, &local_y, &black)) || band != 0)
          continue;
      }
      const size_t index = (size_t)(y + g_ws_extra_top) * plane_width +
          column;
      /* Use the captured winner's band, allowing artwork to change priority.
       * The mask supplies identity; the plane supplies its final colour/math. */
      for (int priority = 0; priority < 4; ++priority) {
        uint32_t *plane = (uint32_t *)g_diorama_layer_pixels[
            DioramaPlaneForObjectPriority(priority)];
        if (plane && plane[index]) {
          faces[index] = plane[index];
          plane[index] = 0;
          s_death_heim_hub_faces_promoted = true;
        }
      }
    }
  }
}
