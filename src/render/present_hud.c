#include "render/present_hud.h"

#include "app/settings.h"  /* Pixel-aspect enum only; no live settings. */
#include "app/session_fatal.h"
#include "present/present.h"
#include "present/presentation_surface.h"
#include "present/presentation_upload_mirror.h"
#include "render/localized_text_presenter.h"
#include "render/text_cell_composite.h"
#include "sim/menu/present_sim_menu.h"

static ArRenderTexture s_background_texture, s_object_texture;
static PresentationUploadMirror s_background_mirror, s_object_mirror;

ArRenderTexture PresentHud_BackgroundTexture(void) { return s_background_texture; }
ArRenderTexture PresentHud_ObjectTexture(void) { return s_object_texture; }

void PresentHud_DestroySources(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_object_texture);
  ArRenderDevice_DestroyTexture(device, s_background_texture);
  s_object_texture = ArRenderTexture_Invalid();
  s_background_texture = ArRenderTexture_Invalid();
  PresentationUploadMirror_Reset(&s_background_mirror);
  PresentationUploadMirror_Reset(&s_object_mirror);
}

bool PresentHud_CreateSources(ArRenderDevice *device, int snes_height) {
  const ArRenderTextureDesc desc = {
    .width = SR_PPU_SURFACE_MAX_WIDTH,
    .height = snes_height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(device, &desc, &s_background_texture) ||
      !ArRenderDevice_CreateTexture(device, &desc, &s_object_texture)) {
    PresentHud_DestroySources(device);
    return false;
  }
  return true;
}

static uint64_t UploadSource(
    ArRenderDevice *device, ArRenderTexture texture, PresentationUploadMirror *mirror,
    const uint8_t *pixels, int width, int height, int pitch, int x, int y) {
  PresentationUploadResult result;
  return PresentationUploadMirror_UploadArgb8888(
      mirror, device, texture, pixels, width, height, pitch, x, y, &result)
      ? result.uploaded_bytes : 0;
}

PresentHudUploadResult PresentHud_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  PresentHudUploadResult uploaded = {0};
  /* Refresh HUD textures for both presentation paths. Diorama anchors the HUD
   * through the same textures as flat mode; skipping this upload would combine
   * stale pixels with the current frame's split geometry. */
  const FrameSlotOverlayCapture *hud_bg_capture =
      &slot->overlay_captures[kFrameSlotOverlay_Bg3];
  const bool captured_bg3 =
      hud_bg_capture->y1 > hud_bg_capture->y0 &&
      (hud_bg_capture->flags & kFrameSlotOverlayFlag_RemoveFromGame) != 0u;
  if (slot->hud_split_height || captured_bg3) {
    int split_rows = slot->hud_split_height;
    if (ArRenderTexture_IsValid(s_background_texture)) {
      int rows = hud_bg_capture->y1;
      if (rows < split_rows) rows = split_rows;
      const ArRenderRectI hud = {0, 0, slot->snes_width, rows};
      const SrPpuSurfaceView *surface = PresentationSurface_Bound(
          &slot->sim3d_output_surfaces.hud_bg);
      if (!surface)
        surface = PresentationSurface_Bound(
            &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0]);
      if (PresentationSurface_Holds(surface, hud.w, hud.h))
        uploaded.background_bytes = UploadSource(
            device, s_background_texture,
            &s_background_mirror,
            surface->data, hud.w, hud.h,
            (int)surface->pitch_bytes, hud.x, hud.y);
    }
    if (ArRenderTexture_IsValid(s_object_texture)) {
      /* Choose the surface before its extent: the two are not interchangeable.
       * The promoted-icon surface is described by the promote's own latched
       * row count, never by overlay_captures[Obj] -- that capture is whatever
       * policy claimed the single OBJ slot last, and a full-frame scene claim
       * legitimately overwrites it. Taking the extent from the capture while
       * taking the pixels from the promoted surface asked for more rows than
       * that surface has, so validation rejected the upload and the icon's
       * texture was silently never filled. It is the same rule hud_icon_first/count
       * already follow. */
      bool promoted_icon = false;
      const SrPpuSurfaceView *surface = PresentationSurface_Bound(
          &slot->sim3d_output_surfaces.hud_obj);
      if (!surface) {
        surface = PresentationSurface_Bound(&slot->hud_obj_surface);
        promoted_icon = surface != NULL;
      }
      if (!surface)
        surface = PresentationSurface_Bound(
            &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0]);
      int rows = promoted_icon
          ? slot->hud_icon_rows
          : slot->overlay_captures[kFrameSlotOverlay_Obj].y1;
      if (rows < split_rows) rows = split_rows;
      const ArRenderRectI hud = {0, 0, slot->snes_width, rows};
      if (PresentationSurface_Holds(surface, hud.w, hud.h))
        uploaded.object_bytes = UploadSource(
            device, s_object_texture,
            &s_object_mirror,
            surface->data, hud.w, hud.h,
            (int)surface->pitch_bytes, hud.x, hud.y);
    }
  }
  return uploaded;
}

static void RenderHudChunk(ArRenderDevice *device, ArRenderTexture texture,
                           ArRenderRectI src, ArRenderRectI dst) {
  if (!ArRenderTexture_IsValid(texture) ||
      src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
    return;
  const ArRenderRectF source = (ArRenderRectF){src.x, src.y, src.w, src.h};
  const ArRenderRectF destination = (ArRenderRectF){dst.x, dst.y, dst.w, dst.h};
  (void)ArRenderDevice_DrawTexture(
      device, texture, &source, &destination);
}

static HudProjectionInputs BuildProjectionInputs(const FrameSlot *slot) {
  HudProjectionInputs in = {0};
  in.hud_bg_texture = s_background_texture;
  in.hud_obj_texture = s_object_texture;
  in.hud_scale_percent = slot->hud_scale_percent;
  in.crt_pixel_aspect = slot->pixel_aspect == kPixelAspect_Crt43;
  in.snes_width = slot->snes_width;
  in.snes_height = FrameSlot_VisibleHeight(slot);
  in.authentic_y0 = slot->visible_top;
  in.visible_width = slot->visible_width;
  in.authentic_width = kFrameSlotAuthenticWidth;
  in.hud_split_height = slot->hud_split_height;
  in.hud_left_end = slot->hud_left_end;
  in.hud_right_start = slot->hud_right_start;
  in.hud_player_row_y = slot->hud_player_row_y;
  in.hud_left_only_y = slot->hud_left_only_y;
  in.extra_left_right = slot->extra_left_right;
  {
    const FrameSlotOverlayCapture *bg3 =
        &slot->overlay_captures[kFrameSlotOverlay_Bg3];
    if (!PresentSimMenu_Active(slot) &&
        bg3->y1 > (int16_t)slot->hud_split_height && bg3->y1 <= 240)
      in.hud_body_y1 = (uint8_t)bg3->y1;
  }

  /* The promote's own latched range, NOT overlay_captures[Obj].oamFirst/Count:
   * in diorama mode that capture describes the full-frame 0..127 scene claim
   * that legitimately overwrote the icon's capture, so keying off it dropped
   * obj_icon_valid and the icon fell back to whatever the scene did with it
   * (drawn tilted and centered rather than anchored beside the right group).
   *
   * Any nonzero count, not ==4: the promote only ever latches a range it has
   * validated as a 16x16 HUD icon, and Sky Palace spends 1 slot on that icon
   * for three of the four spells and 4 for Magical Fire. Demanding 4 here was
   * the second half of the bug that stranded those three at centre screen. */
  if (slot->oam_valid && slot->hud_icon_count) {
    int first = slot->hud_icon_first;
    in.obj_icon_x = (slot->oam[first * 2] & 0xff) |
        ((slot->high_oam[first >> 2] >> ((first & 3) * 2)) & 1) << 8;
    in.obj_icon_y = slot->oam[first * 2] >> 8;
    in.obj_icon_valid = true;
  }
  return in;
}

int PresentHud_BuildChunks(const FrameSlot *slot, ArRenderRectI viewport,
                           HudPresentationChunk chunks[kHudPresentationChunkCapacity]) {
  const HudProjectionInputs in = BuildProjectionInputs(slot);
  int count = ArHudLayout_BuildPresentationChunks(viewport, &in, chunks);
  /* Town notices survive modern-menu targeting and action handoffs, even
   * without enhanced text. Project the native composer's shared bounds so
   * the preserved BG3 strip and localized replacement always line up. */
  if (PresentSimMenu_Active(slot) && slot->bg3_state_valid &&
      slot->visible_width > 0 && slot->snes_height > 0) {
    ArRenderRectI regions[kArTextCellMaximumProjectedRegions];
    const size_t region_count = ArTextCellComposite_ProjectRegion(
        slot->sim_menu.preserved_bg3_region,
        slot->bg3_tilemap_width_tiles,
        slot->bg3_tilemap_height_tiles, slot->bg3_hscroll, slot->bg3_vscroll,
        256, slot->snes_height, regions);
    const double sx=(double)viewport.w/slot->visible_width;
    const double sy=(double)viewport.h/slot->snes_height;
    for (size_t i=0;i<region_count && count<kHudPresentationChunkCapacity;++i) {
      const ArRenderRectI source=regions[i];
      if (source.y < slot->hud_split_height || source.y+source.h >
          slot->overlay_captures[kFrameSlotOverlay_Bg3].y1) continue;
      chunks[count++]=(HudPresentationChunk){
        .texture=s_background_texture,
        .texture_source={source.x+(slot->snes_width-256)/2,source.y,source.w,source.h},
        .screen_source=source,
        .output_destination={viewport.x+(viewport.w-256*sx)/2+source.x*sx,
            viewport.y+source.y*sy,source.w*sx,source.h*sy},
        .inspector_kind=kInspectorPresentation_HudBg,
      };
    }
  }
  return count;
}

static void PresentHudChunksDirect(ArRenderDevice *device, const FrameSlot *slot,
                                   ArRenderRectI viewport) {
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = PresentHud_BuildChunks(slot, viewport, chunks);
  for (int i = 0; i < count; i++)
    RenderHudChunk(device, chunks[i].texture, chunks[i].texture_source,
                   chunks[i].output_destination);
}

void PresentHud_Draw(ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport) {
  const FrameSlotOverlayCapture *bg3 =
      &slot->overlay_captures[kFrameSlotOverlay_Bg3];
  if (bg3->y1 > (int16_t)slot->hud_split_height)
    PresentHud_DrawComposited(device, slot, viewport);
  else
    PresentHudChunksDirect(device, slot, viewport);
}

/* Assemble HUD chunks into a single screen-space texture. This keeps their
 * rounding and localized replacement masks inside one composite, so seams
 * cannot expose the tilted scene behind them. Recreate it when the viewport
 * changes size, and release it with the localized-text cache on device reset. */
static ArRenderTexture s_hud_composite_texture;
static int s_hud_composite_w, s_hud_composite_h;

static ArRenderTexture EnsureHudCompositeTexture(ArRenderDevice *device, int w, int h) {
  if (!ArRenderDevice_IsReady(device) || w <= 0 || h <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_hud_composite_texture) &&
      s_hud_composite_w == w &&
      s_hud_composite_h == h)
    return s_hud_composite_texture;
  ArRenderDevice_DestroyTexture(device, s_hud_composite_texture);
  s_hud_composite_texture = ArRenderTexture_Invalid();
  s_hud_composite_w = w;
  s_hud_composite_h = h;
  const ArRenderTextureDesc desc = {
    .width = w,
    .height = h,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  (void)ArRenderDevice_CreateTexture(
      device, &desc, &s_hud_composite_texture);
  return s_hud_composite_texture;
}

void PresentHud_DrawComposited(ArRenderDevice *device, const FrameSlot *slot,
                               ArRenderRectI viewport) {
  ArRenderTexture composite = EnsureHudCompositeTexture(
      device, viewport.w, viewport.h);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  if (!ArRenderTexture_IsValid(composite)) {
    PresentHudChunksDirect(device, slot, viewport);
    return;
  }

  ArRenderRectI local_viewport = {0, 0, viewport.w, viewport.h};
  int count = PresentHud_BuildChunks(slot, local_viewport, chunks);
  if (count <= 0) return;
  ArLocalizedPreparedFrame localized;
  ArLocalizedTextPresenter_Prepare(
      device, &slot->localization,
      slot->bg3_state_valid, slot->bg3_tilemap_base_words,
      slot->bg3_tilemap_width_tiles, slot->bg3_tilemap_height_tiles,
      slot->bg3_hscroll, slot->bg3_vscroll,
      kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight,
      chunks, (size_t)count, &localized);

  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, composite, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (begin == kArRenderTargetBegin_StateLost)
      SessionFatal_Request(
          "The renderer lost its scene target while beginning HUD composition "
          "(%s). Restart the game; if this repeats, update your graphics "
          "driver.", ArRenderDevice_LastError(device));
    else {
      PresentHudChunksDirect(device, slot, viewport);
    }
    return;
  }
  bool target_ready =
      ArRenderDevice_UseOutputCoordinates(device) &&
      ArRenderDevice_Clear(
          device,
          (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  bool localized_drawn = false;
  if (target_ready) {
    bool masks_valid = true;
    for (int i = 0; i < count; i++) {
      if (!localized.mask_count) {
        RenderHudChunk(device, chunks[i].texture, chunks[i].texture_source,
                       chunks[i].output_destination);
        continue;
      }
      HudPresentationChunk pieces[kArTextCellMaximumChunkPieces];
      const size_t piece_count = ArTextCellComposite_SubtractMasks(
          &chunks[i], localized.masks, localized.mask_count,
          pieces, kArTextCellMaximumChunkPieces);
      if (piece_count == SIZE_MAX) {
        masks_valid = false;
        break;
      }
      for (size_t piece = 0; piece < piece_count; ++piece)
        RenderHudChunk(device, pieces[piece].texture, pieces[piece].texture_source,
                       pieces[piece].output_destination);
    }
    /* Discard a failed partial composite. Painting transparent native chunks
     * over it cannot erase already-drawn enhanced ink. After restoring the
     * output target below, draw only the untouched native chunks instead. */
    const float text_brightness = slot->inidisp & 0x80 ? 0.0f : (slot->inidisp & 15) / 15.0f;
    if (!masks_valid || !ArLocalizedTextPresenter_DrawWithBrightness(
            device, &localized, text_brightness)) {
      target_ready = false;
    } else {
      localized_drawn = true;
    }
  }
  if (!ArRenderDevice_EndTarget(device, &target_state)) {
    SessionFatal_Request(
        "The renderer could not restore its scene target after composing the "
        "HUD (%s). Restart the game; if this repeats, update your graphics "
        "driver.", ArRenderDevice_LastError(device));
    return;
  }
  if (!target_ready) {
    PresentHudChunksDirect(device, slot, viewport);
    return;
  }

  const ArRenderRectF destination = {
    viewport.x, viewport.y, viewport.w, viewport.h,
  };
  /* Ordinary alpha draws into a transparent target leave premultiplied RGB.
   * Use the matching over operation when flattening that group, otherwise its
   * alpha is applied twice and transparent texels can replace the scene with
   * the target's clear colour on some GPU backends. */
  const ArRenderDrawState over = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_AlphaPremultiplied,
  };
  if (ArRenderDevice_DrawTextureWithState(
          device, composite, NULL, &destination, &over)) {
    if (localized_drawn) {
      ArTextPresentation_ReportPage(localized.ready_dialogue_ticket,
          localized.dialogue_page_start,localized.dialogue_page_end);
      ArTextPresentation_MarkReady(localized.ready_dialogue_ticket);
    }
  } else {
    PresentHudChunksDirect(device, slot, viewport);
  }
}

void PresentHud_Reset(ArRenderDevice *device) {
  PresentationUploadMirror_Reset(&s_background_mirror);
  PresentationUploadMirror_Reset(&s_object_mirror);
  ArRenderDevice_DestroyTexture(device, s_hud_composite_texture);
  s_hud_composite_texture = ArRenderTexture_Invalid();
  s_hud_composite_w = s_hud_composite_h = 0;
  ArLocalizedTextPresenter_Reset(device);
}
