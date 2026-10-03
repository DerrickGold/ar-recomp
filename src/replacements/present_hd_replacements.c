#include "replacements/present_hd_replacements.h"

#include "present/present.h"
#include "present/presentation_surface.h"
#include "replacements/hd_replacement_host.h"

static ArRenderRectF ToRenderRectF(ArRenderRectI rect) {
  return (ArRenderRectF){(float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h};
}

uint64_t PresentHdReplacements_UploadMode7(
    ArRenderDevice *device, ArRenderTexture texture, const FrameSlot *slot) {
  if (ArRenderTexture_IsValid(texture) && slot->m7_active) {
    const ArRenderRectI src = {
      slot->visible_x0 * kHdMode7Scale, 0,
      slot->visible_width * kHdMode7Scale,
      slot->snes_height * kHdMode7Scale,
    };
    const SrPpuSurfaceView *surface =
        PresentationSurface_Bound(&slot->ppu_surfaces.mode7);
    const uint8_t *pixels =
        PresentationSurface_Region(surface, src.x, src.y, src.w, src.h);
    const ArRenderRectI destination = {src.x, src.y, src.w, src.h};
    if (pixels && ArRenderDevice_UpdateTexture(
            device, texture, &destination, pixels,
            (int)surface->pitch_bytes)) {
      return (uint64_t)src.w * (uint64_t)src.h * sizeof(uint32_t);
    }
  }
  return 0;
}

void PresentHdReplacements_DrawMode7(
    ArRenderDevice *device, ArRenderTexture texture, const FrameSlot *slot,
    ArRenderRectI viewport) {
  if (!ArRenderTexture_IsValid(texture) || !slot->m7_active) return;
  const ArRenderRectI src = {
    slot->visible_x0 * kHdMode7Scale, 0,
    slot->visible_width * kHdMode7Scale,
    slot->snes_height * kHdMode7Scale,
  };
  const ArRenderRectF source = ToRenderRectF(src);
  const ArRenderRectF destination = ToRenderRectF(viewport);
  (void)ArRenderDevice_DrawTexture(
      device, texture, &source, &destination);
}

/* Draw every active HD replacement over the region its capture removed this
 * frame. Master brightness is resolved on the host texture so INIDISP fades
 * apply to the substituted art; forced blank suppresses it entirely. */
void PresentHdReplacements_DrawScreen(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport) {
  if (slot->inidisp & 0x80) return;

  int vis_w = slot->visible_width;
  int vis_x0 = slot->visible_x0;
  int extra = (slot->snes_width - kFrameSlotAuthenticWidth) / 2;
  double scale_x = (double)viewport.w / vis_w;
  double scale_y = (double)viewport.h / FrameSlot_VisibleHeight(slot);

  for (int i = 0; i < slot->hd_entry_count; i++) {
    const FrameSlotHdEntry *entry = &slot->hd_entries[i];
    if (!entry->active || !ArRenderTexture_IsValid(entry->texture)) continue;
    const FrameSlotOverlayCapture *capture =
        &slot->overlay_captures[entry->source];
    if (capture->x1 <= capture->x0 || capture->y1 <= capture->y0 ||
        !(capture->flags & kFrameSlotOverlayFlag_RemoveFromGame))
      continue;
    int dx0 = (int)((capture->x0 + entry->image_inset_left + extra - vis_x0) *
                    scale_x + 0.5);
    int dx1 = (int)((capture->x1 + extra - vis_x0) * scale_x + 0.5);
    int dy0 = (int)((slot->visible_top + capture->y0) * scale_y + 0.5);
    int dy1 = (int)((slot->visible_top + capture->y1) * scale_y + 0.5);
    const ArRenderRectI dst = {
      viewport.x + dx0, viewport.y + dy0, dx1 - dx0, dy1 - dy0,
    };
    if (dst.w <= 0 || dst.h <= 0) continue;

    const uint8_t mod = entry->brightness_mod
        ? (uint8_t)((slot->inidisp & 0xf) * 255 / 15) : 255;
    const float modulation = (float)mod / 255.0f;
    const ArRenderRectF destination = ToRenderRectF(dst);
    (void)ArRenderDevice_DrawTextureTinted(
        device, entry->texture, NULL, &destination,
        (ArRenderColorF){modulation, modulation, modulation, 1.0f});
  }
}

