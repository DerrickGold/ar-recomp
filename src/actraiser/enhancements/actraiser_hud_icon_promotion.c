/* ActRaiser HUD icon promotion: lifts the HUD's OBJ icon out of the
 * widescreen and diorama scene into the flat HUD (promote before scanout,
 * prepare and finish around it) and restores the pixels it covered.
 * Phase: game (frame transaction). */
#include "actraiser/enhancements/actraiser_enhancements_internal.h"
#include "diorama/diorama.h"
#include "host/host_frame_surfaces.h"

/* The OAM slots ActRaiser_WidescreenHudObjPromote validated THIS frame.
 *
 * Kept separately from overlayCaptures[Obj] on purpose: the diorama block
 * further down ActRaiserDrawPpuFrame legitimately re-captures OBJ as a
 * full-frame scene layer over OAM slots 0..127, and PpuSetOverlayCapture
 * resets oamFirst/oamCount, so the capture stops being able to answer "which
 * sprites are the flat HUD icon" the moment the diorama is on. Anything that
 * needs that answer must read this record via ActRaiser_HudObjIconRange
 * instead of re-deriving it from the capture. */
static uint8_t s_hud_obj_icon_first;
static uint8_t s_hud_obj_icon_count;
/* Rows of the promoted capture, latched for the same reason as the OAM range:
 * the surface holding the icon is described by what the promote claimed, not
 * by the single OBJ overlay capture that a later full-frame scene claim
 * legitimately overwrites. */
static uint8_t s_hud_obj_icon_rows;

bool ActRaiser_HudObjIconRange(uint8_t *first, uint8_t *count, uint8_t *rows) {
  /* Writes every output on every path, including "nothing promoted" (0/0/0):
   * FrameSlot slots are recycled, so leaving them untouched would republish
   * the previous occupant's icon range on a frame that has no icon. */
  if (first) *first = s_hud_obj_icon_first;
  if (count) *count = s_hud_obj_icon_count;
  if (rows) *rows = s_hud_obj_icon_rows;
  return s_hud_obj_icon_count != 0;
}

bool ActRaiser_HudObjSurfaceView(SrPpuSurfaceView *surface) {
  if (!surface) return false;
  *surface = (SrPpuSurfaceView){0};
  if (!s_hud_obj_icon_count) return false;

  const int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
  const int height = kActRaiserAuthenticHeight;
  if (width <= 0 || width > (int)SR_PPU_SURFACE_MAX_WIDTH ||
      height > kHostDisplayFramebufferHeight)
    return false;

  const uint64_t pitch = (uint64_t)(uint32_t)width * sizeof(uint32_t);
  *surface = (SrPpuSurfaceView){
    .flags = SR_PPU_SURFACE_BOUND | SR_PPU_SURFACE_HAS_CONTENT,
    .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32,
    .data = g_hud_obj_pixels,
    .byte_size = pitch * (uint32_t)height,
    .pitch_bytes = pitch,
    .width_pixels = (uint32_t)width,
    .height_pixels = (uint32_t)height,
    .scale = 1u,
  };
  return true;
}

/* Promote the HUD producer's uploaded sprite range into the fixed HUD. */
static SrResult ActRaiser_WidescreenHudObjPromoteTransaction(
    void *user_data, SrRunnerHandle *runner,
    const SrPpuFrameTransactionContext *context) {
  (void)user_data;
  (void)runner;
  s_hud_obj_icon_first = 0;
  s_hud_obj_icon_count = 0;
  s_hud_obj_icon_rows = 0;
  if (!context)
    return SR_RESULT_OK;
  /* A regular HUD split is valid without a widescreen margin budget (4:3).
   * The native fallback handles flat Diorama modes that intentionally have no
   * split, notably Wide Raw. */
  const bool native_flat_diorama =
      Diorama_IsActiveThisFrame() && g_settings.diorama_hud_flat &&
      context->frame.hud_split_height == 0;
  if (!context->frame.margin_budget &&
      !context->frame.hud_split_height && !native_flat_diorama)
    return SR_RESULT_OK;

  uint8 capture_height = 0;
  uint8 capture_first = kActRaiserHudObjOamFirst;
  uint8 capture_count = kActRaiserHudObjOamCount;
  uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool split_action_hud =
      context->frame.hud_split_height == kActRaiserActionHudHeight &&
      context->frame.hud_left_end == kActRaiserActionHudLeftEnd &&
      context->frame.hud_right_start == kActRaiserActionHudRightStart &&
      context->frame.hud_player_row_y == kActRaiserActionHudPlayerRowY &&
      context->frame.hud_left_only_y == kActRaiserActionHudEnemyRowY;
  if ((split_action_hud || native_flat_diorama) &&
      ActRaiser_IsActionMapGroup(map_group)) {
    capture_height = kActRaiserActionHudHeight;
  } else if (context->frame.hud_split_height ==
                 kActRaiserSimulationHudHeight &&
             context->frame.hud_left_end == kActRaiserSimulationHudSplit &&
             context->frame.hud_right_start ==
                 kActRaiserSimulationHudSplit &&
             context->frame.hud_left_only_y ==
                 kActRaiserSimulationHudHeight &&
             map_group == kActRaiserMapGroup_NonAction &&
             map_number >= kActRaiserSimulationTown_First &&
             map_number <= kActRaiserNonActionMap_SkyPalace) {
    capture_height = kActRaiserSimulationHudHeight;
  } else {
    return SR_RESULT_OK;
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
  if (!owned) return SR_RESULT_OK;

  const SrPpuOverlayCaptureState expected =
      ActRaiser_OverlayCaptureState(
          &context->frame.overlays[SR_PPU_OVERLAY_OBJ]);
  const SrPpuOverlayCaptureState replacement = {
    .x1 = kActRaiserAuthenticWidth,
    .y1 = capture_height,
    .flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME,
    .oam_first = capture_first,
    .oam_count = capture_count,
  };
  if (ActRaiser_ExchangeOverlayCapture(
          SR_PPU_OVERLAY_OBJ, context->lifetime_generation,
          &expected, &replacement)) {
    s_hud_obj_icon_first = capture_first;
    s_hud_obj_icon_count = capture_count;
    s_hud_obj_icon_rows = capture_height;
  }
  return SR_RESULT_OK;
}

void ActRaiser_WidescreenHudObjPromote(void) {
  if (!ActRaiser_Runner() || !ActRaiser_RunnerApi() ||
      ActRaiser_RunnerApi()->struct_size <
          SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE ||
      !ActRaiser_RunnerApi()->visit_ppu_frame_transaction)
    return;
  const SrPpuFrameTransactionRequest request = {
    .struct_size = sizeof(request),
    .callback = ActRaiser_WidescreenHudObjPromoteTransaction,
  };
  (void)ActRaiser_RunnerApi()->visit_ppu_frame_transaction(ActRaiser_Runner(), &request);
}

/* Split the promoted HUD icon back out of the diorama's OBJ planes. The
 * action-mode counterpart of sim3d.c's PrepareHudHandoff/RestoreTownHudPolicy
 * pair, and the reason the selected-magic icon is pinned beside the right HUD
 * group in diorama mode instead of riding the tilted scene at its authentic
 * centre-screen X.
 *
 * Why this is needed at all: ActRaiser_WidescreenHudObjPromote captures the
 * icon's OAM range into g_hud_obj_pixels for present.c to anchor, but the
 * diorama block later in this same frame re-captures OBJ as a full-frame scene
 * layer over slots 0..127 -- a strictly wider claim on the ONE capture slot the
 * PPU gives each source, and PpuSetOverlayCapture resets the OAM range as it
 * lands. That claim is right for the player and enemies and wrong for the HUD
 * icon, and there is no second OBJ capture to put the icon in, so the split has
 * to happen on captured pixels rather than on capture policy.
 *
 * Two host phases around one PPU-owned capture, because the icon's
 * tile/palette state can change during the raster:
 *
 *   Prepare, BEFORE scanout -- resolve the icon's OAM range and footprint and
 *     register an independent semantic range capture. The real sprite
 *     evaluator then writes those selected slots to g_hud_obj_pixels at the
 *     exact instant it fetches each displayed pixel. This is intentionally not
 *     a PpuRasterizeObjRange call on either side of scanout: the game-over
 *     return to Sky Palace changes the live OBJ state within the frame, and a
 *     second decode can disagree with the pixels the evaluator just emitted.
 *   Finish, AFTER scanout -- use that completed HUD surface as the opacity
 *     mask while clearing the same pixels from the diorama plane that holds
 *     the icon's priority band, which only has content once scanout has run.
 *     The diorama's OBJ capture is RemoveFromGame, so the icon is already out
 *     of the backdrop frame; only the plane needs erasing. */
enum { kActRaiserHudIconRasterLimit = 64 };
static SrPpuObjResolveResult s_hud_icon_bounds;
static int s_hud_icon_priority;
static bool s_hud_icon_ready;
static bool s_hud_icon_ppu_relocated;

/* The icon-footprint restore layer (see the block comment in
 * ActRaiser_DioramaHudObjPrepare): per footprint pixel, the colour and the
 * priority band of the sprite the promoted icon was covering. Static rather
 * than stack — three 64x64 scratch buffers is 36 KB, well past a sane frame. */
enum { kActRaiserHudRestoreNone = 0xFF };
static uint32_t s_hud_restore_argb[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];
static uint8_t s_hud_restore_prio[kActRaiserHudIconRasterLimit *
                                  kActRaiserHudIconRasterLimit];
static uint32_t s_hud_restore_slot[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];

/* g_diorama_layer_pixels[] index for an OBJ priority band. Band N is the plane
 * the diorama's kPrioBands table bound for band N, and band == OAM priority
 * (ppu.c's split does band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM
 * priority in those two bits). Band 0 is the primary source slot. */
int ActRaiser_DioramaObjPlaneForPriority(int priority) {
  return priority ? kDioramaPlane_Obj1 + (priority - 1) : SR_PPU_OVERLAY_OBJ;
}

void ActRaiser_DioramaHudObjPrepare(void) {
  const SrPpuFrameTransactionContext *frame = ActRaiser_PpuFrame();

  s_hud_icon_ready = false;
  s_hud_icon_ppu_relocated = false;
  /* Only the flat-HUD variant anchors a host overlay. With diorama_hud_flat
   * off the whole status bar is deliberately a tilted plane (see the A5/A7
   * note in the capture block), and the icon belongs on it. */
  if (!frame || !g_diorama_frame_active || !g_settings.diorama_hud_flat ||
      !s_hud_obj_icon_count)
    return;

  const uint8_t first = s_hud_obj_icon_first;
  const uint8_t count = s_hud_obj_icon_count;
  const int priority = (frame->oam.data[first * 2 + 1] >> 12) & 3;
  SrPpuObjResolveResult bounds;
  if (!ActRaiser_ResolvePpuObjRange(
          first, count, (uint8_t)priority, &bounds))
    return;
  const int raster_width = bounds.x1 - bounds.x0;
  const int raster_height = bounds.y1 - bounds.y0;
  /* Every promoted signature is 16x16, whether the ROM spent four small slots
   * on it or one large one; this ceiling is slack, not a target, and a range
   * that overruns it is refused rather than clipped. */
  if (raster_width <= 0 || raster_height <= 0 ||
      raster_width > kActRaiserHudIconRasterLimit ||
      raster_height > kActRaiserHudIconRasterLimit)
    return;

  s_hud_icon_bounds = bounds;
  s_hud_icon_priority = priority;
  const int surface_width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
  const SrPpuOverlayCaptureState *obj_capture =
      ActRaiser_PpuCapture(SR_PPU_OVERLAY_OBJ);
  const bool relocate = obj_capture &&
      (obj_capture->flags & SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN) != 0u;
  const SrPpuObjCaptureRequest capture_request = {
    .struct_size = sizeof(capture_request),
    .flags = SR_PPU_OBJ_CAPTURE_RANGE |
             (relocate ? SR_PPU_OBJ_CAPTURE_RELOCATED : 0u),
    .lifetime_generation = frame->lifetime_generation,
    .range_first = first,
    .range_count = count,
    .range_x = bounds.x0,
    .range_y = bounds.y0,
    .range_width = (uint32_t)raster_width,
    .range_height = (uint32_t)raster_height,
    .range_pixels = g_hud_obj_pixels,
    .range_pixel_byte_size =
        (uint64_t)surface_width * sizeof(uint32_t) *
        kHostDisplayFramebufferHeight,
    .range_pitch_bytes = (uint64_t)surface_width * sizeof(uint32_t),
    .relocated_first = relocate ? first : 0u,
    .relocated_count = relocate ? count : 0u,
  };
  if (!ActRaiser_ConfigurePpuObjCapture(&capture_request))
    return;
  s_hud_icon_ready = true;
  s_hud_icon_ppu_relocated = relocate;

  /* --- Restore layer: what the promoted sprites were HIDING ---------------
   *
   * Every captured OBJ pixel competes in ONE shared z-buffer
   * (ppu.c `overlayBuffers[SR_PPU_OVERLAY_OBJ]`: first opaque writer wins,
   * OAM walked in slot order); only at scanout is the surviving pixel routed
   * to the plane matching its own priority. The promoted icon is the LEADING
   * slot range, so wherever it overlaps a world sprite it wins the z-test and
   * that sprite's pixels are never captured into ANY plane.
   *
   * That is correct on hardware -- the icon is in front, so it hides what is
   * behind it. It stops being correct the moment we MOVE the icon to the HUD
   * anchor: zeroing it out of its own band then leaves a hole shaped like the
   * icon cut out of whatever stood behind it. Found 2026-08-05 as a bite taken
   * out of a level gargoyle, and it needs only an on-screen overlap, so it is
   * independent of the vertical band.
   *
   * Fix: replay the same first-writer-wins rule over the icon's footprint with
   * the promoted slots REMOVED, keeping each restored pixel's colour AND the
   * priority band it belongs to. Rasterised here, pre-scanout, so the
   * best-effort underlay cannot accidentally use the NEXT frame's streamed
   * art. Unlike the visible icon this is only a rare overlap repair; capturing
   * its complete per-line OAM competition would require a second OBJ resolve.
   *
   * Not a full re-render: the hardware sprite-per-line limits are not replayed,
   * so a footprint contested by more than 34 slivers on a line could restore a
   * pixel the PPU would have dropped. Bounded by a 16x16 HUD icon, and failing
   * that way (showing the sprite) beats failing the other (a hole). */
  memset(s_hud_restore_prio, kActRaiserHudRestoreNone,
         (size_t)raster_width * raster_height);
  uint8_t index = frame->state.object_priority_rotation
      ? (uint8_t)(frame->state.oam_address_low & 0xfe) : 0;
  for (int evaluated = 0; evaluated < 128;
       evaluated++, index = (uint8_t)(index + 2)) {
    const int slot = index >> 1;
    if (slot >= first && slot < first + count)
      continue;                       /* the promoted sprites themselves */
    const int slot_priority =
        (frame->oam.data[slot * 2 + 1] >> 12) & 3;
    SrPpuObjResolveResult sb;
    if (!ActRaiser_ResolvePpuObjRange(
            (uint8_t)slot, 1, (uint8_t)slot_priority, &sb))
      continue;
    /* Cheap reject before rasterising: most slots cannot touch the icon. */
    if (sb.x1 <= bounds.x0 || sb.x0 >= bounds.x1 ||
        sb.y1 <= bounds.y0 || sb.y0 >= bounds.y1)
      continue;
    const int sw = sb.x1 - sb.x0, sh = sb.y1 - sb.y0;
    if (sw <= 0 || sh <= 0 ||
        sw > kActRaiserHudIconRasterLimit || sh > kActRaiserHudIconRasterLimit)
      continue;
    SrPpuObjRasterResult raster;
    if (!ActRaiser_RasterizePpuObjRange(
            (uint8_t)slot, 1, (uint8_t)slot_priority,
            s_hud_restore_slot, (size_t)sw * sizeof(uint32_t),
            sizeof(s_hud_restore_slot), &raster))
      continue;
    for (int y = 0; y < sh; y++) {
      const int fy = sb.y0 + y - bounds.y0;
      if (fy < 0 || fy >= raster_height) continue;
      for (int x = 0; x < sw; x++) {
        const int fx = sb.x0 + x - bounds.x0;
        if (fx < 0 || fx >= raster_width) continue;
        const size_t fi = (size_t)fy * raster_width + fx;
        if (s_hud_restore_prio[fi] != kActRaiserHudRestoreNone)
          continue;                   /* an earlier slot already won here */
        const uint32_t px = s_hud_restore_slot[(size_t)y * sw + x];
        if (!px) continue;
        s_hud_restore_argb[fi] = px;
        s_hud_restore_prio[fi] = (uint8_t)slot_priority;
      }
    }
  }
}

void ActRaiser_DioramaHudObjFinish(int width) {

  /* Bound against the PLANE width -- the wider of the two destinations, and the
   * one the apron grew. Both surfaces are allocated kPpuSurfaceWidth wide. */
  if (!s_hud_icon_ready || width <= 0 ||
      width + SR_PPU_OBJ_APRON * 2 > SR_PPU_SURFACE_MAX_WIDTH)
    return;


  const int raster_width = s_hud_icon_bounds.x1 - s_hud_icon_bounds.x0;
  const int raster_height = s_hud_icon_bounds.y1 - s_hud_icon_bounds.y0;
  /* Band index == OAM priority (ppu.c's priority-split resolve does
   * band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM priority in those two
   * bits), so this is the same plane the diorama's kPrioBands table bound. */
  uint32_t *plane = (uint32_t *)g_diorama_layer_pixels[
      ActRaiser_DioramaObjPlaneForPriority(s_hud_icon_priority)];
  const size_t pitch = (size_t)width * 4;
  const int extra = (width - kActRaiserAuthenticWidth) / 2;
  /* TWO destinations, TWO widths. g_hud_obj_pixels stays at the DISPLAY width
   * (it is a screen-anchored overlay); the diorama OBJ planes are bound
   * APRON-wide. One `width` indexing both put the punch-out at the wrong stride
   * and the wrong column, so the promoted icon was never erased from the tilted
   * plane -- it rode the OBJ plane into the scene as a full-size sprite (the
   * priority-3 fire icon floating mid-scene, measured at gf1636 of
   * saves/legacy/artifacts2.rec).
   *
   * The plane side goes through the apron geometry rather than re-deriving it,
   * so this function and the apron pass cannot disagree about where a screen
   * column lands -- disagreeing is exactly what the bug WAS. */
  const ActionApronGeometry plane_geom = { extra, SR_PPU_OBJ_APRON };
  const int plane_width = ActionApron_SurfaceWidth(&plane_geom);
  /* Two destinations, two row origins. g_hud_obj_pixels is consumed in
   * authentic screen space (the promoted HUD overlay), so it indexes by
   * screen_y. The diorama PLANE is consumed in capture space, whose row 0 is
   * screen y = -g_ws_extra_top, so the hole punched in it must carry that
   * offset -- otherwise the icon is erased from the wrong rows and a ghost of
   * it stays in the tilted OBJ plane. Zero without a vertical margin. */
  const int plane_row_bias = g_ws_extra_top;

  for (int y = 0; y < raster_height; y++) {
    const int screen_y = s_hud_icon_bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kActRaiserAuthenticHeight) continue;
    const uint32_t *hud_src = (const uint32_t *)(
        g_hud_obj_pixels + (size_t)screen_y * pitch);
    for (int x = 0; x < raster_width; x++) {
      const int texture_x = s_hud_icon_bounds.x0 + x + extra;
      if (texture_x < 0 || texture_x >= width) continue;
      const uint32_t pixel = hud_src[texture_x];
      if (!pixel) continue;
      const size_t plane_index =
          (size_t)(screen_y + plane_row_bias) * plane_width +
          ActionApron_SurfaceColumn(&plane_geom, s_hud_icon_bounds.x0 + x);
      if (plane && !s_hud_icon_ppu_relocated) plane[plane_index] = 0;
      /* Hand the pixel back to whatever the icon was covering, in ITS band --
       * the capture never recorded it, because the icon won the shared OBJ
       * z-test here. Only inside the icon's OPAQUE footprint: where the icon
       * was transparent the real capture already placed the right pixel in the
       * right band. Ordered after the zero above so a restore into the icon's
       * own band survives it. */
      const size_t footprint_index = (size_t)y * raster_width + x;
      const uint8_t restore_priority = s_hud_restore_prio[footprint_index];
      if (!s_hud_icon_ppu_relocated &&
          restore_priority != kActRaiserHudRestoreNone) {
        uint32_t *restore_plane = (uint32_t *)g_diorama_layer_pixels[
            ActRaiser_DioramaObjPlaneForPriority(restore_priority)];
        if (restore_plane)
          restore_plane[plane_index] = s_hud_restore_argb[footprint_index];
      }
    }
  }
}
