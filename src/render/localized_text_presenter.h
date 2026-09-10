#ifndef AR_RENDER_LOCALIZED_TEXT_PRESENTER_H
#define AR_RENDER_LOCALIZED_TEXT_PRESENTER_H

#include <stdbool.h>
#include <stddef.h>

#include "localization/localization_frame.h"
#include "localization/text_backend.h"
#include "render/hud_layout.h"
#include "render/text_cell_composite.h"
#include "render/text_surface_cache.h"

enum {
  kArLocalizedPreparedMaskCapacity =
      kArTextCellRecordCapacity * kArTextCellMaximumChunkPieces,
};

typedef struct ArLocalizedPreparedText {
  ArTextSurface surface;
  ArRenderRectI destination;
  uint32_t revealed_cluster_count;
  uint32_t cluster_count;
} ArLocalizedPreparedText;

typedef struct ArLocalizedPreparedFrame {
  ArLocalizedPreparedText texts[kArTextCellRecordCapacity];
  uint8_t text_count;
  ArRenderRectI masks[kArLocalizedPreparedMaskCapacity];
  size_t mask_count;
} ArLocalizedPreparedFrame;

/* The host injects a portable factory once. No SDL type crosses this API. */
void ArLocalizedTextPresenter_SetBackend(const ArTextBackend *backend);

/* Rasterize/upload every viable replacement before publishing its masks.
 * Failure of one record leaves that record's native cells unclaimed. */
void ArLocalizedTextPresenter_Prepare(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    bool bg3_state_valid, uint16_t bg3_tilemap_base_words,
    unsigned bg3_map_width_tiles, unsigned bg3_map_height_tiles,
    uint16_t bg3_hscroll, uint16_t bg3_vscroll,
    unsigned visible_width, unsigned visible_height,
    const HudPresentationChunk *chunks, size_t chunk_count,
    ArLocalizedPreparedFrame *prepared);

/* Called while the HUD composite target is active. */
bool ArLocalizedTextPresenter_Draw(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared);
void ArLocalizedTextPresenter_Reset(ArRenderDevice *device);

#endif /* AR_RENDER_LOCALIZED_TEXT_PRESENTER_H */
