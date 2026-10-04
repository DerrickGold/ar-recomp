/* One semantic HUD owner for flat action, SIM, Palace, diorama and church.
 * Capture and scene exclusion share live scanout; publish only after completion. */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"
#include "diorama/diorama.h"

static HudIconFrame s_pending_icon, s_completed_icon;
static uint64_t s_icon_serial;
static uint32_t s_icon_pixels[SR_PPU_SURFACE_MAX_WIDTH * kActRaiserActionHudHeight];

HudIconFrame ActRaiser_HudIconFrame(void) { return s_completed_icon; }

void ActRaiser_HudIconBeginFrame(void) {
  s_pending_icon = (HudIconFrame){0};
  s_completed_icon = (HudIconFrame){0};
}

void ActRaiser_HudIconPrepare(void) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();
  if (!frame || !frame->overlays[SR_PPU_OVERLAY_OBJ].data ||
      (Diorama_IsActiveThisFrame() && !g_settings.diorama_hud_flat))
    return;
  /* A regular HUD split is valid without a widescreen margin budget (4:3).
   * The native fallback handles flat Diorama modes that intentionally have no
   * split, notably Wide Raw. */
  const bool native_flat_diorama = Diorama_IsActiveThisFrame() && g_settings.diorama_hud_flat &&
                                   frame->frame.hud_split_height == 0;
  if (!frame->frame.margin_budget && !frame->frame.hud_split_height && !native_flat_diorama) return;

  uint8 capture_height = 0;
  uint8 capture_first = 0;
  uint8 capture_count = 0;
  uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool split_action_hud = frame->frame.hud_split_height == kActRaiserActionHudHeight &&
                                frame->frame.hud_left_end == kActRaiserActionHudLeftEnd &&
                                frame->frame.hud_right_start == kActRaiserActionHudRightStart &&
                                frame->frame.hud_player_row_y == kActRaiserActionHudPlayerRowY &&
                                frame->frame.hud_left_only_y == kActRaiserActionHudEnemyRowY;
  if ((split_action_hud || native_flat_diorama) &&
      ActRaiser_IsActionMapGroup(map_group)) {
    capture_height = kActRaiserActionHudHeight;
  } else if (frame->frame.hud_split_height == kActRaiserSimulationHudHeight &&
             frame->frame.hud_left_end == kActRaiserSimulationHudSplit &&
             frame->frame.hud_right_start == kActRaiserSimulationHudSplit &&
             frame->frame.hud_left_only_y == kActRaiserSimulationHudHeight &&
             map_group == kActRaiserMapGroup_NonAction &&
             map_number >= kActRaiserSimulationTown_First &&
             (map_number <= kActRaiserNonActionMap_SkyPalace ||
              (map_number == kActRaiserNonActionMap_Temple && Sim3D_ChurchIsOn()))) {
    capture_height = kActRaiserSimulationHudHeight;
  } else {
    return;
  }

  const ActRaiserSpriteOwnership ownership =
      ActRaiserSpriteOwnership_Presented(map_group, map_number);
  const bool owned = ActRaiserSpriteOwnership_Range(
      &ownership, kActRaiserSprite_HudIcon, &capture_first, &capture_count);
  if (map_number == kActRaiserNonActionMap_SkyPalace &&
      map_group == kActRaiserMapGroup_NonAction &&
      ActRaiser_DeveloperFlagEnabled(kActRaiserDeveloperFlag_HudIconLog)) {
    static int last_spell = -1, last_slot = -2, last_count = -1;
    const int spell = g_ram[kActRaiserWram_SelectedMagic];
    const int slot = owned ? capture_first : -1;
    if (spell != last_spell || slot != last_slot || capture_count != last_count) {
      fprintf(stderr, "[hud-icon] gf=%u sky-palace spell=%d -> slot=%d count=%u\n",
          (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
          spell, slot, capture_count);
      last_spell = spell;
      last_slot = slot;
      last_count = capture_count;
    }
  }
  if (!owned) return;

  if (!frame->oam.data || frame->oam.element_count <= capture_first * 2u + 1u) return;
  SrPpuObjResolveResult bounds;
  const uint8_t priority = (frame->oam.data[capture_first * 2u + 1u] >> 12) & 3u;
  if (!ActRaiser_ResolvePpuObjRange(capture_first, capture_count, priority, &bounds)) return;
  /* Hourglass pieces overlap by one pixel. All supported producers fit the
   * native 16x16 HUD cell, emitted as one large or four small sprites. */
  if (bounds.x0 < 0 || bounds.y0 < 0 || bounds.x0 + 16 > kActRaiserAuthenticWidth ||
      bounds.y0 + 16 > capture_height || bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0 ||
      bounds.x1 - bounds.x0 > 16 || bounds.y1 - bounds.y0 > 16)
    return;
  const uint32_t width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
  const uint64_t pitch = width * sizeof(uint32_t);
  const SrPpuObjCaptureRequest request = {
      .struct_size = sizeof(request),
      .flags = SR_PPU_OBJ_CAPTURE_RANGE | SR_PPU_OBJ_CAPTURE_HANDOFF,
      .lifetime_generation = frame->lifetime_generation,
      .range_first = capture_first,
      .range_count = capture_count,
      .range_x = bounds.x0,
      .range_y = bounds.y0,
      .range_width = 16,
      .range_height = 16,
      .range_pixels = (uint8_t *)s_icon_pixels,
      .range_pixel_byte_size = sizeof(s_icon_pixels),
      .range_pitch_bytes = pitch,
  };
  if (!ActRaiser_ConfigurePpuObjCapture(&request)) return;
  s_pending_icon = (HudIconFrame){
      .lifetime_generation = frame->lifetime_generation,
      .x = bounds.x0,
      .y = bounds.y0,
      .width = 16,
      .height = 16,
      .first = capture_first,
      .count = capture_count,
      .surface =
          {
              .flags = SR_PPU_SURFACE_BOUND | SR_PPU_SURFACE_HAS_CONTENT,
              .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32,
              .data = (const uint8_t *)s_icon_pixels,
              .byte_size = pitch * (bounds.y0 + 16),
              .pitch_bytes = pitch,
              .width_pixels = width,
              .height_pixels = bounds.y0 + 16,
              .origin_x = g_ws_extra,
              .scale = 1,
          },
  };
}

void ActRaiser_HudIconComplete(SrResult status, const SrPpuScanoutResult *result) {
  if (status != SR_RESULT_OK || !result || !s_pending_icon.count ||
      result->lifetime_generation != s_pending_icon.lifetime_generation)
    return;
  s_completed_icon = s_pending_icon;
  s_completed_icon.scene_removed = true;
  s_completed_icon.frame_serial = ++s_icon_serial;
}
