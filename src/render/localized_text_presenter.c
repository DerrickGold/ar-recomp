#include "render/localized_text_presenter.h"

#include <stdio.h>
#include <string.h>

#include "localization/enhanced_text_settings.h"
#include "render/text_cell_composite.h"

enum { kSurfaceCacheCapacity = 128, kFontSizeCacheCapacity = 8 };

typedef struct LocalizedTextPresenterState {
  ArTextBackend backend;
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  bool cache_initialized;
  char active_stack[kArLocalizationFrameFontStackCapacity];
  char active_path[kArLocalizationFrameFontPathCapacity];
  uint64_t active_revision;
  bool error_reported;
} LocalizedTextPresenterState;

static LocalizedTextPresenterState s_presenter;

static void ReportOnce(const char *operation, const char *detail) {
  if (s_presenter.error_reported) return;
  s_presenter.error_reported = true;
  fprintf(stderr, "[localized-text] %s%s%s; native text retained\n",
          operation, detail && detail[0] ? ": " : "",
          detail && detail[0] ? detail : "");
}

void ArLocalizedTextPresenter_SetBackend(const ArTextBackend *backend) {
  s_presenter.backend = ArTextBackend_IsReady(backend)
      ? *backend : (ArTextBackend){0};
}

static void DestroyResources(ArRenderDevice *device) {
  if (s_presenter.cache_initialized)
    ArTextSurfaceCache_Destroy(&s_presenter.cache, device);
  ArTextBackendInstance_Destroy(&s_presenter.instance);
  s_presenter.cache_initialized = false;
  s_presenter.active_stack[0] = 0;
  s_presenter.active_path[0] = 0;
  s_presenter.active_revision = 0;
}

static bool ActivateFont(ArRenderDevice *device,
                         const ArLocalizationFrame *frame) {
  if (!ArTextBackend_IsReady(&s_presenter.backend) || !frame->font_revision ||
      !frame->font_stack_id[0] || !frame->primary_font_path[0])
    return false;
  if (s_presenter.cache_initialized &&
      s_presenter.active_revision == frame->font_revision &&
      !strcmp(s_presenter.active_stack, frame->font_stack_id) &&
      !strcmp(s_presenter.active_path, frame->primary_font_path))
    return true;
  const ArTextBackendConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = frame->font_stack_id,
      .primary_font_path = frame->primary_font_path,
      .font_revision = frame->font_revision,
      .cached_size_capacity = kFontSizeCacheCapacity,
  };
  ArTextBackendInstance instance = {0};
  char error[kArTextRasterErrorCapacity] = {0};
  if (!ArTextBackendInstance_Create(
          &instance, &s_presenter.backend, &config,
          error, sizeof(error))) {
    ReportOnce("font initialization failed", error);
    return false;
  }
  ArTextSurfaceCache cache = {0};
  if (!ArTextSurfaceCache_Init(&cache, kSurfaceCacheCapacity)) {
    ArTextBackendInstance_Destroy(&instance);
    ReportOnce("text cache initialization failed", NULL);
    return false;
  }
  DestroyResources(device);
  s_presenter.instance = instance;
  s_presenter.cache = cache;
  s_presenter.cache_initialized = true;
  snprintf(s_presenter.active_stack, sizeof(s_presenter.active_stack), "%s",
           frame->font_stack_id);
  snprintf(s_presenter.active_path, sizeof(s_presenter.active_path), "%s",
           frame->primary_font_path);
  s_presenter.active_revision = frame->font_revision;
  return true;
}

static bool RectangleContains(ArRenderRectI outer, ArRenderRectI inner) {
  return inner.w > 0 && inner.h > 0 && inner.x >= outer.x &&
      inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
      inner.y + inner.h <= outer.y + outer.h;
}

static bool ProjectTextDestination(
    const HudPresentationChunk *chunks, size_t chunk_count,
    ArRenderRectI screen_region, ArRenderRectI *destination) {
  for (size_t index = 0; index < chunk_count; ++index) {
    if (chunks[index].inspector_kind != kInspectorPresentation_HudBg ||
        !RectangleContains(chunks[index].screen_source, screen_region))
      continue;
    return ArTextCellComposite_ProjectToOutput(
        &chunks[index], screen_region, destination);
  }
  return false;
}

static size_t BuildReplacementMasks(
    const ArLocalizationTextSnapshot *snapshot,
    ArRenderRectI claim,
    unsigned bg3_map_width_tiles,
    unsigned bg3_map_height_tiles,
    uint16_t bg3_hscroll,
    uint16_t bg3_vscroll,
    unsigned visible_width,
    unsigned visible_height,
    ArRenderRectI masks[kArTextCellMaximumChunkPieces]) {
  if (!snapshot || !snapshot->native_preserve_count) {
    masks[0] = claim;
    return 1;
  }
  ArRenderRectI preserved[
      kArLocalizationFrameNativePreserveCapacity *
      kArTextCellMaximumProjectedRegions];
  size_t preserved_count = 0;
  for (uint8_t index = 0;
       index < snapshot->native_preserve_count; ++index) {
    ArRenderRectI projected[kArTextCellMaximumProjectedRegions];
    const size_t count = ArTextCellComposite_ProjectRegion(
        snapshot->native_preserves[index],
        bg3_map_width_tiles, bg3_map_height_tiles,
        bg3_hscroll, bg3_vscroll, visible_width, visible_height,
        projected);
    if (preserved_count + count >
        sizeof(preserved) / sizeof(preserved[0]))
      return SIZE_MAX;
    memcpy(&preserved[preserved_count], projected,
           count * sizeof(projected[0]));
    preserved_count += count;
  }
  if (!preserved_count) {
    masks[0] = claim;
    return 1;
  }
  const HudPresentationChunk claim_chunk = {
      .texture_source = claim,
      .screen_source = claim,
      .output_destination = claim,
  };
  HudPresentationChunk pieces[kArTextCellMaximumChunkPieces];
  const size_t piece_count = ArTextCellComposite_SubtractMasks(
      &claim_chunk, preserved, preserved_count, pieces,
      kArTextCellMaximumChunkPieces);
  if (piece_count == SIZE_MAX) return SIZE_MAX;
  for (size_t index = 0; index < piece_count; ++index)
    masks[index] = pieces[index].screen_source;
  return piece_count;
}

void ArLocalizedTextPresenter_Prepare(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    bool bg3_state_valid, uint16_t bg3_tilemap_base_words,
    unsigned bg3_map_width_tiles, unsigned bg3_map_height_tiles,
    uint16_t bg3_hscroll, uint16_t bg3_vscroll,
    unsigned visible_width, unsigned visible_height,
    const HudPresentationChunk *chunks, size_t chunk_count,
    ArLocalizedPreparedFrame *prepared) {
  if (!prepared) return;
  memset(prepared, 0, sizeof(*prepared));
  if (!device || !frame ||
      frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
      !frame->snapshot_count || !bg3_state_valid || !chunks || !chunk_count ||
      !ActivateFont(device, frame))
    return;

  for (uint8_t record_index = 0;
       record_index < frame->cells.count; ++record_index) {
    const ArTextCellRecord *record = &frame->cells.records[record_index];
    if (record->destination.background != 3u ||
        record->destination.screen != kArTextCellScreen_Composited ||
        record->destination.tilemap_base_words != bg3_tilemap_base_words ||
        record->snapshot_slot < 0 ||
        (uint8_t)record->snapshot_slot >= frame->snapshot_count)
      continue;
    ArRenderRectI projected[kArTextCellMaximumProjectedRegions];
    const size_t projected_count = ArTextCellComposite_ProjectRegion(
        record->region, bg3_map_width_tiles, bg3_map_height_tiles,
        bg3_hscroll, bg3_vscroll, visible_width, visible_height, projected);
    if (projected_count != 1u ||
        prepared->text_count >= kArTextCellRecordCapacity)
      continue;
    ArRenderRectI bounds;
    if (!ProjectTextDestination(chunks, chunk_count, projected[0], &bounds) ||
        bounds.w <= 0 || bounds.h <= 0)
      continue;
    const uint8_t snapshot_index = (uint8_t)record->snapshot_slot;
    const ArLocalizationTextSnapshot *snapshot =
        &frame->snapshots[snapshot_index];
    size_t utf8_bytes = 0;
    const char *utf8 = ArLocalizationFrame_GetText(
        frame, snapshot_index, &utf8_bytes);
    if (!utf8 || snapshot->surface_id != record->surface_id) continue;
    ArRenderRectI masks[kArTextCellMaximumChunkPieces];
    const size_t mask_count = BuildReplacementMasks(
        snapshot, projected[0], bg3_map_width_tiles,
        bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
        visible_width, visible_height, masks);
    if (!mask_count || mask_count == SIZE_MAX ||
        prepared->mask_count + mask_count >
            sizeof(prepared->masks) / sizeof(prepared->masks[0]))
      continue;

    const int base_pixels =
        (snapshot->native_font_pixels * bounds.h +
         (int)record->region.rows * 4) /
        ((int)record->region.rows * 8);
    int minimum_base_pixels = base_pixels * 2 / 3;
    if (minimum_base_pixels < 1) minimum_base_pixels = 1;
    ArTextRasterRequest request = {
        .struct_size = sizeof(request),
        .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
        .utf8 = utf8,
        .utf8_bytes = utf8_bytes,
        .font_stack_id = frame->font_stack_id,
        .font_stack_id_bytes = strlen(frame->font_stack_id),
        .source_revision = snapshot->source_revision,
        .font_revision = frame->font_revision,
        .style_id = kArTextStyle_RetailBlueWhiteBands,
        .flags = kArTextRasterFlag_WrapWords |
                 kArTextRasterFlag_PreserveHardBreaks |
                 kArTextRasterFlag_CropHorizontalWhitespace |
                 kArTextRasterFlag_IncludeRevealClusters,
        .direction = snapshot->direction,
        .alignment = snapshot->direction == kArTextDirection_RightToLeft
            ? kArTextHorizontalAlignment_Trailing
            : kArTextHorizontalAlignment_Leading,
        .font_pixels = base_pixels,
        .minimum_font_pixels = minimum_base_pixels,
        .maximum_width = bounds.w,
        .maximum_height = bounds.h,
        .filter = kArRenderFilter_Nearest,
        .language_bcp47 = frame->locale,
        .language_bcp47_bytes = strlen(frame->locale),
  };
    if (!ArEnhancedTextSettings_Apply(
            &frame->settings, base_pixels, minimum_base_pixels, &request))
      continue;
    ArTextSurface surface;
    char error[kArTextRasterErrorCapacity] = {0};
    if (!ArTextSurfaceCache_Acquire(
            &s_presenter.cache, device,
            ArTextBackendInstance_Get(&s_presenter.instance),
            &request, &surface, error, sizeof(error))) {
      ReportOnce("text surface acquisition failed", error);
      continue;
    }
    const ArRenderRectI destination = {
        bounds.x + (bounds.w - surface.width) / 2,
        bounds.y,
        surface.width,
        surface.height,
    };
    prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
        .surface = surface,
        .destination = destination,
        .revealed_cluster_count = snapshot->revealed_cluster_count,
        .cluster_count = snapshot->cluster_count,
    };
    memcpy(&prepared->masks[prepared->mask_count], masks,
           mask_count * sizeof(masks[0]));
    prepared->mask_count += mask_count;
  }
}

static bool DrawOne(ArRenderDevice *device,
                    const ArLocalizedPreparedText *text) {
  const ArRenderRectF destination = {
      (float)text->destination.x, (float)text->destination.y,
      (float)text->destination.w, (float)text->destination.h,
  };
  size_t revealed = text->revealed_cluster_count;
  if (revealed > text->surface.reveal_cluster_count)
    revealed = text->surface.reveal_cluster_count;
  if (revealed >= text->surface.reveal_cluster_count)
    return ArRenderDevice_DrawTexture(
        device, text->surface.texture, NULL, &destination);
  for (size_t index = 0; index < revealed; ++index) {
    const ArTextRevealCluster *cluster =
        &text->surface.reveal_clusters[index];
    if (cluster->width <= 0 || cluster->height <= 0) continue;
    const ArRenderRectF source = {
        (float)cluster->x, (float)cluster->y,
        (float)cluster->width, (float)cluster->height,
    };
    const ArRenderRectF target = {
        destination.x + source.x, destination.y + source.y,
        source.w, source.h,
    };
    if (!ArRenderDevice_DrawTexture(
            device, text->surface.texture, &source, &target))
      return false;
  }
  return true;
}

bool ArLocalizedTextPresenter_Draw(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared) {
  if (!device || !prepared) return false;
  for (uint8_t index = 0; index < prepared->text_count; ++index) {
    if (!DrawOne(device, &prepared->texts[index])) return false;
  }
  return true;
}

void ArLocalizedTextPresenter_Reset(ArRenderDevice *device) {
  if (s_presenter.cache_initialized) {
    const ArTextSurfaceCacheStats *stats =
        ArTextSurfaceCache_GetStats(&s_presenter.cache);
    if (stats && stats->lookups)
      fprintf(stderr,
              "[localized-text] cache lookups=%llu hits=%llu misses=%llu "
              "rasters=%llu uploads=%llu failures=%llu\n",
              (unsigned long long)stats->lookups,
              (unsigned long long)stats->hits,
              (unsigned long long)stats->misses,
              (unsigned long long)stats->rasterize_calls,
              (unsigned long long)stats->upload_calls,
              (unsigned long long)stats->failures);
  }
  DestroyResources(device);
  s_presenter.error_reported = false;
}
