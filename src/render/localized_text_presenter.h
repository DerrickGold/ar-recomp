#ifndef AR_RENDER_LOCALIZED_TEXT_PRESENTER_H
#define AR_RENDER_LOCALIZED_TEXT_PRESENTER_H

#include <stdbool.h>
#include <stddef.h>

#include "localization/localization_frame.h"
#include "localization/text_backend.h"
#include "localization/text_presentation.h"
#include "render/hud_layout.h"
#include "render/text_cell_composite.h"
#include "render/text_surface_cache.h"

enum {
  kArLocalizedPreparedMaskCapacity =
      kArTextCellRecordCapacity * kArTextCellMaximumChunkPieces,
  kArLocalizedPreparedTextCapacity = 64,
  /* Per-cluster horizontal shifts for texts laid out on a uniform key pitch.
   * Pooled, so one keyboard's clusters do not cost every prepared text an
   * array of its own. */
  kArLocalizedPreparedClusterShiftCapacity = 1024,
};

typedef struct ArLocalizedPreparedIndicator {
  ArLocalizationIndicatorKind kind;
  ArRenderRectI destination;
  ArRenderTexture texture;
} ArLocalizedPreparedIndicator;

typedef struct ArLocalizedPreparedInlineObject {
  ArLocalizationInlineObjectKind kind;
  ArRenderRectI destination;
  ArRenderTexture texture;
} ArLocalizedPreparedInlineObject;

typedef struct ArLocalizedPreparedText {
  ArTextSurface surface;
  ArRenderRectI destination;
  uint32_t revealed_cluster_count;
  uint32_t cluster_count;
  /* Empty for ordinary fixed text; dialogue is clipped to this software window. */
  ArRenderRectI viewport;
  /* Start of this text's run in the frame's cluster shift pool, or negative
   * when the text flows and every cluster draws where it was shaped. An index
   * rather than a pointer, so a prepared frame stays safe to copy. */
  int32_t cluster_shift_offset;
} ArLocalizedPreparedText;

typedef struct ArLocalizedPreparedDecoration {
  ArRenderRectI destination;
  ArRenderTexture texture;
} ArLocalizedPreparedDecoration;

typedef struct ArLocalizedPreparedFrame {
  ArLocalizedPreparedText texts[kArLocalizedPreparedTextCapacity];
  uint8_t text_count;
  ArRenderRectI masks[kArLocalizedPreparedMaskCapacity];
  size_t mask_count;
  ArLocalizedPreparedIndicator
      indicators[kArLocalizationFrameIndicatorCapacity];
  uint8_t indicator_count;
  ArLocalizedPreparedInlineObject
      inline_objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t inline_object_count;
  ArLocalizedPreparedDecoration decorations[kArTextCellRecordCapacity * 2];
  uint8_t decoration_count;
  int cluster_shifts[kArLocalizedPreparedClusterShiftCapacity];
  size_t cluster_shift_count;
  uint64_t ready_dialogue_ticket;
} ArLocalizedPreparedFrame;

/* The host injects a portable factory once. No SDL type crosses this API. */
void ArLocalizedTextPresenter_SetBackend(const ArTextBackend *backend);
/* Provider/context outlive active and pending backend leases. Reset resources
 * before destroying the host store. Changing providers invalidates caches. */
void ArLocalizedTextPresenter_SetFontResources(const ArFontResources *resources);
void ArLocalizedTextPresenter_DiscardPreparedFont(ArRenderDevice *device);

/* Synchronous selection preflight, before authored waits/pages are enabled.
 * Opens, rasters and uploads into one retained candidate cache. Only a later
 * frame with that identity replaces the live font; semantic rejection after
 * preflight cannot invalidate active surfaces. Repeated requests reuse the
 * active/candidate resources without font or GPU work. Same-thread only. */
bool ArLocalizedTextPresenter_PrepareFont(
    ArRenderDevice *device, const ArTextPresentationFont *font,
    char *error, size_t error_capacity);

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

/* Prepare one authentic-pixel screen-space claim. The owning scene projects
 * the record's source region into `bounds`; this layer handles only generic
 * shaping, fitting, direction and cached texture preparation. A successful
 * intentional blank returns true with no prepared text, allowing the caller
 * to suppress native glyphs while retaining surrounding native artwork. */
bool ArLocalizedTextPresenter_PrepareScreenText(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    uint32_t surface_id, ArRenderRectI bounds,
    ArLocalizedPreparedFrame *prepared);

/* Called while the HUD composite target is active. */
bool ArLocalizedTextPresenter_Draw(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared);
/* Post-raster master brightness, 0..1; does not invalidate cached text or
 * reapply brightness to native chunks already shaded by their producer. */
bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared, float brightness);
void ArLocalizedTextPresenter_Reset(ArRenderDevice *device);

#endif /* AR_RENDER_LOCALIZED_TEXT_PRESENTER_H */
