#include "render/localized_text_presenter.h"

#include "render/localized_text_artwork.h"

#include <stdio.h>
#include <string.h>

#include "localization/enhanced_text_settings.h"
#include "localization/unicode_grapheme.h"
#include "render/localized_text_layout.h"
#include "render/text_cell_composite.h"
#include "deterministic_hash.h"

enum {
  kSurfaceCacheCapacity = 128, kFontSizeCacheCapacity = 8,
  /* Entry count alone is not a memory budget: one tall scrolling page at
   * HiDPI outweighs a hundred menu labels. This caps what the cache may own
   * in texture bytes; the prepared frame's own surfaces are never evicted. */
  kSurfaceCacheByteBudget = 96 << 20,
  kReportPlanCapacity = 8, kReportMeasureExtent = 4096,
};

_Static_assert(kSurfaceCacheCapacity >= kArLocalizedPreparedTextCapacity,
               "a prepared frame must fit without evicting its own text surfaces");

typedef struct ReportPlan {
  ArTextCacheKey key;
  /* Digest of the grid this plan was fitted for. The plan is only reused for
   * the identical geometry, without keeping a copy of it. */
  uint64_t grid_digest;
  ArTextCellRegion region;
  ArTextTableColumns columns;
  int font_pixels;
  bool valid, fits;
} ReportPlan;

typedef struct PendingFont {
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  char stack[kArLocalizationFrameFontStackCapacity];
  ArFontResourceId primary;
  ArFontResourceId fallbacks[kArTextPresentationMaximumFallbackFonts];
  size_t fallback_count;
  uint64_t revision, backend_revision;
} PendingFont;

typedef struct LocalizedTextPresenterState {
  ArTextBackend backend;
  ArFontResources resources;
  uint64_t backend_revision, active_backend_revision;
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  bool cache_initialized;
  char active_stack[kArLocalizationFrameFontStackCapacity];
  ArFontResourceId active_primary;
  ArFontResourceId active_fallbacks[kArTextPresentationMaximumFallbackFonts];
  size_t active_fallback_count;
  PendingFont pending;
  uint64_t active_revision;
  uint64_t reported_errors[32];
  unsigned next_reported_error;
  ReportPlan reports[kReportPlanCapacity];
  unsigned next_report;
  /* A page with its live line blanked, and that page's value spans (see
   * PrepareLiveLinePage). */
  char live_line_page[kArLocalizationFrameTextCapacity];
  ArTextBidiSpans live_line_spans;
} LocalizedTextPresenterState;

static LocalizedTextPresenterState s_presenter;

static void ReportOnce(const char *operation, const char *detail) {
  uint64_t key = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,
                                           operation, strlen(operation));
  if (detail)
    key = DeterministicHash_Fnv1a64(key, detail, strlen(detail));
  if (!key)
    key = 1;
  for (size_t i = 0; i < 32; ++i)
    if (s_presenter.reported_errors[i] == key)
      return;
  s_presenter.reported_errors[s_presenter.next_reported_error++ % 32] = key;
  fprintf(stderr, "[localized-text] %s%s%s; native text retained\n",
          operation, detail && detail[0] ? ": " : "",
          detail && detail[0] ? detail : "");
}

void ArLocalizedTextPresenter_SetBackend(const ArTextBackend *backend) {
  const ArTextBackend selected =
      ArTextBackend_IsReady(backend) ? *backend : (ArTextBackend){0};
  if (selected.ops != s_presenter.backend.ops ||
      selected.context != s_presenter.backend.context)
    ++s_presenter.backend_revision;
  s_presenter.backend = selected;
}

void ArLocalizedTextPresenter_SetFontResources(const ArFontResources *resources) {
  const ArFontResources selected = ArFontResources_IsReady(resources)
      ? *resources : (ArFontResources){0};
  if (selected.ops != s_presenter.resources.ops ||
      selected.context != s_presenter.resources.context)
    ++s_presenter.backend_revision;
  s_presenter.resources = selected;
}

static void DestroyResources(ArRenderDevice *device) {
  ArLocalizedTextArtwork_Reset(device);
  if (s_presenter.cache_initialized)
    ArTextSurfaceCache_Destroy(&s_presenter.cache, device);
  ArTextBackendInstance_Destroy(&s_presenter.instance);
  s_presenter.cache_initialized = false;
  s_presenter.active_stack[0] = 0;
  s_presenter.active_primary = 0;
  s_presenter.active_revision = 0;
  s_presenter.active_fallback_count = 0;
  memset(s_presenter.active_fallbacks, 0, sizeof(s_presenter.active_fallbacks));
  memset(s_presenter.reported_errors, 0, sizeof(s_presenter.reported_errors));
  s_presenter.next_reported_error = 0;
  memset(s_presenter.reports, 0, sizeof(s_presenter.reports));
  s_presenter.next_report = 0;
}

static void ReportRequestFailure(const ArLocalizationTextSnapshot *snapshot,
                                  const ArTextRasterRequest *request,
                                  const char *detail) {
  const ArTextCacheKey key = ArTextSurfaceCache_MakeKey(
      ArTextBackendInstance_Get(&s_presenter.instance), request);
  char diagnostic[kArTextRasterErrorCapacity];
  snprintf(diagnostic, sizeof(diagnostic),
           "surface=%u layout=%u request=%016llx/%016llx: %.128s",
           snapshot->surface_id, (unsigned)snapshot->layout,
           (unsigned long long)key.primary,
           (unsigned long long)key.secondary, detail);
  ReportOnce("text surface acquisition failed", diagnostic);
}

static bool FontError(char *error, size_t capacity, const char *message) {
  if (error && capacity)
    snprintf(error, capacity, "%s", message);
  return false;
}

static void DestroyPendingFont(ArRenderDevice *device) {
  ArTextSurfaceCache_Destroy(&s_presenter.pending.cache, device);
  ArTextBackendInstance_Destroy(&s_presenter.pending.instance);
  memset(&s_presenter.pending, 0, sizeof(s_presenter.pending));
}

void ArLocalizedTextPresenter_DiscardPreparedFont(ArRenderDevice *device) {
  DestroyPendingFont(device);
}

static bool ActiveFontMatches(ArRenderDevice *device,
                              const ArTextPresentationFont *font) {
  if (!s_presenter.cache_initialized || s_presenter.cache.device != device ||
      s_presenter.active_backend_revision != s_presenter.backend_revision ||
      s_presenter.active_revision != font->revision ||
      s_presenter.active_fallback_count != font->fallback_count ||
      strcmp(s_presenter.active_stack, font->stack_id) ||
      s_presenter.active_primary != font->primary)
    return false;
  for (size_t i = 0; i < font->fallback_count; ++i)
    if (font->fallbacks[i] != s_presenter.active_fallbacks[i])
      return false;
  return true;
}

bool ArLocalizedTextPresenter_PrepareFont(ArRenderDevice *device,
                                          const ArTextPresentationFont *font,
                                          char *error, size_t error_capacity) {
  if (error && error_capacity)
    error[0] = 0;
  if (!ArRenderDevice_IsReady(device) ||
      !ArTextBackend_IsReady(&s_presenter.backend) ||
      !ArFontResources_IsReady(&s_presenter.resources))
    return FontError(error, error_capacity,
                     "enhanced text backend is unavailable");
  if (!font ||
      font->struct_size < offsetof(ArTextPresentationFont, fallback_count) +
                              sizeof(font->fallback_count) ||
      font->abi_version != AR_TEXT_PRESENTATION_ABI_VERSION ||
      !font->revision || !font->stack_id || !font->stack_id[0] ||
      !font->primary ||
      strlen(font->stack_id) >= sizeof(s_presenter.active_stack) ||
      font->fallback_count > kArTextPresentationMaximumFallbackFonts ||
      (font->fallback_count && !font->fallbacks))
    return FontError(error, error_capacity, "invalid enhanced font selection");
  for (size_t i = 0; i < font->fallback_count; ++i) {
    if (!font->fallbacks[i])
      return FontError(error, error_capacity, "invalid fallback font resource");
  }
  if (ActiveFontMatches(device, font))
    return true;
  PendingFont *pending = &s_presenter.pending;
  bool same_pending =
      pending->cache.device == device &&
      pending->backend_revision == s_presenter.backend_revision &&
      pending->revision == font->revision &&
      pending->fallback_count == font->fallback_count &&
      !strcmp(pending->stack, font->stack_id) &&
      pending->primary == font->primary;
  for (size_t i = 0; same_pending && i < font->fallback_count; ++i)
    same_pending = pending->fallbacks[i] == font->fallbacks[i];
  if (same_pending)
    return true;
  const ArTextBackendConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = font->stack_id,
      .resources = s_presenter.resources,
      .primary_font = font->primary,
      .font_revision = font->revision,
      .cached_size_capacity = kFontSizeCacheCapacity,
      .fallback_fonts = font->fallbacks,
      .fallback_font_count = font->fallback_count,
  };
  ArTextBackendInstance instance = {0};
  if (!ArTextBackendInstance_Create(&instance, &s_presenter.backend, &config,
                                    error, error_capacity))
    return false;
  ArTextSurfaceCache cache = {0};
  if (!ArTextSurfaceCache_Init(&cache, kSurfaceCacheCapacity)) {
    ArTextBackendInstance_Destroy(&instance);
    return FontError(error, error_capacity, "text cache initialization failed");
  }
  ArTextSurfaceCache_SetByteBudget(&cache, kSurfaceCacheByteBudget);
  /* Backend construction may be lazy, opening fonts only for requested sizes.
   * Exercise shaping, raster allocation, texture creation and upload before
   * advertising readiness to the game. Keep the warmed font/cache on success.
   */
  const ArTextRasterRequest probe = {
      .struct_size = sizeof(probe),
      .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = "Ag",
      .utf8_bytes = 2,
      .font_stack_id = font->stack_id,
      .font_stack_id_bytes = strlen(font->stack_id),
      .source_revision = 1,
      .font_revision = font->revision,
      .style_id = kArTextStyle_RetailBlueWhiteBands,
      .flags = kArTextRasterFlag_IncludeRevealClusters,
      .font_pixels = 24,
      .minimum_font_pixels = 24,
      .maximum_width = 128,
      .maximum_height = 128,
      .filter = kArRenderFilter_Nearest,
  };
  ArTextSurface surface;
  if (!ArTextSurfaceCache_Acquire(&cache, device,
                                  ArTextBackendInstance_Get(&instance), &probe,
                                  &surface, error, error_capacity)) {
    ArTextSurfaceCache_Destroy(&cache, device);
    ArTextBackendInstance_Destroy(&instance);
    return false;
  }
  /* A source can still fail semantic validation after this readiness check.
   * Keep the active font and all borrowed surfaces intact until a frame with
   * the approved identity arrives. At most one candidate is retained. */
  DestroyPendingFont(device);
  pending->instance = instance;
  pending->cache = cache;
  snprintf(pending->stack, sizeof(pending->stack), "%s", font->stack_id);
  pending->primary = font->primary;
  pending->revision = font->revision;
  pending->backend_revision = s_presenter.backend_revision;
  pending->fallback_count = font->fallback_count;
  for (size_t i = 0; i < font->fallback_count; ++i)
    pending->fallbacks[i] = font->fallbacks[i];
  return true;
}

static bool ActivateFont(ArRenderDevice *device,
                         const ArLocalizationFrame *frame) {
  if (frame->fallback_font_count > kArTextPresentationMaximumFallbackFonts)
    return false;
  const ArTextPresentationFont font = {
      .struct_size = sizeof(font),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .stack_id = frame->font_stack_id,
      .primary = frame->primary_font,
      .revision = frame->font_revision,
      .fallbacks = frame->fallback_fonts,
      .fallback_count = frame->fallback_font_count,
  };
  char error[kArTextRasterErrorCapacity] = {0};
  if (ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                           sizeof(error))) {
    if (!ActiveFontMatches(device, &font)) {
      PendingFont *pending = &s_presenter.pending;
      DestroyResources(device);
      s_presenter.instance = pending->instance;
      s_presenter.cache = pending->cache;
      s_presenter.cache_initialized = true;
      memcpy(s_presenter.active_stack, pending->stack, sizeof(pending->stack));
      s_presenter.active_primary = pending->primary;
      memcpy(s_presenter.active_fallbacks, pending->fallbacks,
             sizeof(pending->fallbacks));
      s_presenter.active_fallback_count = pending->fallback_count;
      s_presenter.active_revision = pending->revision;
      s_presenter.active_backend_revision = pending->backend_revision;
      memset(pending, 0,
             sizeof(*pending)); /* Ownership moved, not duplicated. */
    }
    return true;
  }
  ReportOnce("font initialization failed", error);
  return false;
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
  /* A tile claim can straddle a horizontal HUD-band boundary even though
   * its ink does not (the action PLAYER/ENEMY rows have a blank leading scan
   * line). Join only contiguous pieces with identical horizontal placement.
   * Independently anchored or separated groups must still fall back. */
  int next_y = screen_region.y;
  ArRenderRectI joined = {0};
  while (next_y < screen_region.y + screen_region.h) {
    bool found = false;
    for (size_t index = 0; index < chunk_count; ++index) {
      const HudPresentationChunk *chunk = &chunks[index];
      const ArRenderRectI source = chunk->screen_source;
      if (chunk->inspector_kind != kInspectorPresentation_HudBg ||
          source.x > screen_region.x || source.x + source.w < screen_region.x + screen_region.w ||
          source.y > next_y || source.y + source.h <= next_y)
        continue;
      const int end = source.y + source.h < screen_region.y + screen_region.h
          ? source.y + source.h : screen_region.y + screen_region.h;
      ArRenderRectI piece = {0};
      const bool visible = ArTextCellComposite_ProjectToOutput(chunk,
          (ArRenderRectI){screen_region.x, next_y, screen_region.w, end - next_y}, &piece);
      if (!visible) {
        /* At tiny HUD scales, a one-scanline sliver rounds to no pixels. */
        if (piece.w <= 0 || piece.h != 0) continue;
        next_y = end;
        found = true;
        break;
      }
      if (!joined.h) joined = piece;
      else {
        if (piece.x != joined.x || piece.w != joined.w || piece.y != joined.y + joined.h)
          continue;
        joined.h += piece.h;
      }
      next_y = end;
      found = true;
      break;
    }
    if (!found) return false;
  }
  if (joined.h) { *destination = joined; return true; }
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

static const ArTextRevealCluster *FindRevealCluster(
    const ArTextSurface *surface, uint32_t end_utf8_byte,
    size_t *cluster_index) {
  if (cluster_index) *cluster_index = 0;
  if (!surface || !end_utf8_byte) return NULL;
  for (size_t index = 0; index < surface->reveal_cluster_count; ++index) {
    if (surface->reveal_clusters[index].end_utf8_byte == end_utf8_byte) {
      if (cluster_index) *cluster_index = index;
      return &surface->reveal_clusters[index];
    }
  }
  return NULL;
}

static ArRenderRectI OutputInk(const ArTextSurface *surface,
                              ArRenderRectI destination) {
  ArRenderRectI ink = surface->ink_bounds;
  if (ink.w > 0 && ink.h > 0) {
    ink.x += destination.x;
    ink.y += destination.y;
  }
  return ink;
}


enum { kMaximumTableFields = 64 };

typedef struct TableField {
  size_t utf8_offset;
  size_t utf8_bytes;
  unsigned logical_line;
  unsigned field_index;
  unsigned field_count;
} TableField;

typedef struct TableRowBaseline {
  int desired, minimum, maximum;
  bool active;
} TableRowBaseline;

static void TrimAsciiSpaces(const char *utf8, size_t *offset, size_t *bytes) {
  while (*bytes && (utf8[*offset] == ' ' || utf8[*offset] == '\t')) {
    ++*offset;
    --*bytes;
  }
  while (*bytes && (utf8[*offset + *bytes - 1u] == ' ' ||
                    utf8[*offset + *bytes - 1u] == '\t'))
    --*bytes;
}

static bool AddTableField(TableField *fields, size_t capacity,
                          size_t *count, size_t offset, size_t bytes,
                          unsigned logical_line, unsigned field_index,
                          unsigned field_count) {
  if (!bytes) return true;
  if (!fields || !count || *count >= capacity || !field_count ||
      field_index >= field_count)
    return false;
  fields[(*count)++] = (TableField){
      offset, bytes, logical_line, field_index, field_count};
  return true;
}

/* Explicit authored boundaries keep translated multiword labels in one cell.
 * A pipe inside a resolved value is text, not a delimiter. Blank authored
 * lines retain native row anchors and never become raster work. */
static bool ParseTableFields(const ArLocalizationFrame *frame,
                             const ArLocalizationTextSnapshot *snapshot,
                             const char *utf8, size_t utf8_bytes,
                             TableField *fields, size_t capacity,
                             size_t *field_count,
                             unsigned *logical_line_count) {
  if (field_count) *field_count = 0;
  if (logical_line_count) *logical_line_count = 0;
  if (!utf8 || !utf8_bytes || !fields || !field_count ||
      !logical_line_count)
    return false;
  size_t line_start = 0;
  unsigned line_index = 0;
  while (line_start <= utf8_bytes) {
    size_t line_end = line_start;
    while (line_end < utf8_bytes &&
           !(utf8[line_end] == '\n' && ArTextBoundary_Get(
               frame->structural_boundaries, snapshot->utf8_offset + line_end)))
      ++line_end;
    bool explicit_columns = false;
    for (size_t index = line_start; index < line_end; ++index)
      explicit_columns |= utf8[index] == '|' && ArTextBoundary_Get(
          frame->structural_boundaries, snapshot->utf8_offset + index);
    if (explicit_columns) {
      unsigned columns = 1;
      for (size_t index = line_start; index < line_end; ++index)
        columns += utf8[index] == '|' && ArTextBoundary_Get(
            frame->structural_boundaries, snapshot->utf8_offset + index);
      size_t cell_start = line_start;
      unsigned cell = 0;
      for (size_t index = line_start; index <= line_end; ++index) {
        if (index != line_end &&
            !(utf8[index] == '|' && ArTextBoundary_Get(
                frame->structural_boundaries, snapshot->utf8_offset + index)))
          continue;
        size_t offset = cell_start;
        size_t bytes = index - cell_start;
        TrimAsciiSpaces(utf8, &offset, &bytes);
        if (!AddTableField(fields, capacity, field_count,
                           offset, bytes, line_index, cell, columns))
          return false;
        ++cell;
        cell_start = index + 1u;
      }
    } else {
      size_t start = line_start;
      size_t bytes = line_end - start;
      TrimAsciiSpaces(utf8, &start, &bytes);
      if (!AddTableField(fields, capacity, field_count,
                         start, bytes, line_index, 0, 1))
        return false;
    }
    ++line_index;
    if (line_end == utf8_bytes) break;
    line_start = line_end + 1u;
  }
  *logical_line_count = line_index;
  return *field_count != 0;
}

static bool FieldHasObject(const ArLocalizationFrame *frame,
                           const ArLocalizationTextSnapshot *snapshot,
                           const TableField *field) {
  for (uint8_t i = 0; i < snapshot->inline_object_count; ++i) {
    const uint32_t end = frame->inline_objects[snapshot->inline_object_offset + i].end_utf8_byte;
    if (end > field->utf8_offset && end <= field->utf8_offset + field->utf8_bytes)
      return true;
  }
  return false;
}

static bool TableRequest(const ArLocalizationFrame *frame,
                         const ArLocalizationTextSnapshot *snapshot,
                         const char *utf8, const TableField *field,
                         int base_pixels, int minimum_base_pixels,
                         int width, int height, bool crop_vertical,
                         ArTextRasterRequest *request) {
  const ArLocalizationTextRowRule *rule = ArLocalizationGrid_FindRow(
      ArLocalizationFrame_GetGrid(frame, snapshot), field->logical_line, field->field_count);
  const bool italic = snapshot->italic || (rule && field->field_index < rule->cell_count &&
      rule->cells[field->field_index].italic);
  *request = (ArTextRasterRequest){
      .struct_size = sizeof(*request), .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = utf8 + field->utf8_offset, .utf8_bytes = field->utf8_bytes,
      .font_stack_id = frame->font_stack_id,
      .font_stack_id_bytes = strlen(frame->font_stack_id),
      .source_revision = snapshot->source_revision, .font_revision = frame->font_revision,
      .style_id = snapshot->style_id,
      .band_rgb = snapshot->band_rgb, .body_rgb = snapshot->body_rgb,
      .shadow_rgb = snapshot->shadow_rgb,
      .shadow_enabled = snapshot->shadow_enabled,
      .shadow_shape = snapshot->shadow_shape,
      .flags = kArTextRasterFlag_IncludeRevealClusters |
          (snapshot->slant_ascii_numerals ? kArTextRasterFlag_SlantAsciiNumerals : 0u) |
          (italic ? kArTextRasterFlag_Italic : 0u) |
          (crop_vertical ? kArTextRasterFlag_CropVerticalWhitespace : 0u) |
          (FieldHasObject(frame, snapshot, field) ? 0u : kArTextRasterFlag_CropHorizontalWhitespace),
      .direction = snapshot->language.direction,
      .bidi_spans = frame->bidi.spans + snapshot->bidi_span_offset,
      .bidi_span_count = snapshot->bidi_span_count,
      .bidi_source_offset = (uint32_t)field->utf8_offset,
      .alignment = kArTextHorizontalAlignment_Leading,
      .font_pixels = base_pixels, .minimum_font_pixels = minimum_base_pixels,
      .maximum_width = width, .maximum_height = height,
      .filter = kArRenderFilter_Nearest,
      .language_bcp47 = snapshot->language.locale, .language_bcp47_bytes = strlen(snapshot->language.locale),
  };
  if (snapshot->accent_end_utf8_byte > field->utf8_offset &&
      snapshot->accent_end_utf8_byte <= field->utf8_offset + field->utf8_bytes) {
    request->accent_end_utf8_byte = snapshot->accent_end_utf8_byte - field->utf8_offset;
    request->accent_rgb = snapshot->accent_rgb;
  }
  return ArEnhancedTextSettings_Apply(
      &frame->settings, base_pixels, minimum_base_pixels, request);
}

static uint64_t GridDigest(const ArLocalizationTextGrid *grid) {
  uint64_t digest = DETERMINISTIC_HASH_FNV1A64_OFFSET;
  return grid ? DeterministicHash_Fnv1a64(digest, grid, sizeof(*grid)) : digest;
}

/* Cells of rows the grid marks as sharing its fitted columns. */
static bool IsSharedColumn(const ArLocalizationTextGrid *grid,
                           const TableField *field) {
  const ArLocalizationTextRowRule *rule = ArLocalizationGrid_FindRow(
      grid, field->logical_line, field->field_count);
  return rule && rule->shared_columns && !rule->native_reserved;
}

/* Measure at one shared size. Oversized cells are not individually shrunk:
 * first try sharing whitespace, then let the caller fit the whole table.
 * The same requests are reused for drawing, so a successful first pass does
 * not create a second set of textures. No cache-owned pointers are retained. */
static bool MeasureReport(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot, const ArTextCellRecord *record,
    const char *utf8, const TableField *fields, size_t field_count,
    ArRenderRectI bounds, int font_pixels, ReportPlan *plan, bool *fits) {
  const ArLocalizationTextGrid *grid =
      ArLocalizationFrame_GetGrid(frame, snapshot);
  const unsigned columns = grid ? grid->shared_column_count : 0u;
  int widths[kArTextTableMaximumColumns] = {1, 1, 1, 1, 1};
  int preferred[kArTextTableMaximumColumns] = {0};
  *fits = false;
  bool height_fits = true;
  for (size_t i = 0; i < field_count; ++i) {
    const TableField *field = &fields[i];
    if (!IsSharedColumn(grid, field)) continue;
    if (field->logical_line >= record->region.rows) return false;
    ArTextRasterRequest request;
    if (!TableRequest(frame, snapshot, utf8, field, 1, 1,
                      kReportMeasureExtent, kReportMeasureExtent, false, &request))
      return false;
    request.font_pixels = request.minimum_font_pixels = font_pixels;
    ArTextSurface surface;
    char error[kArTextRasterErrorCapacity] = {0};
    if (!ArTextSurfaceCache_Acquire(&s_presenter.cache, device,
            ArTextBackendInstance_Get(&s_presenter.instance),
            &request, &surface, error, sizeof(error))) {
      ReportRequestFailure(snapshot, &request, error);
      return false;
    }
    int width = surface.width;
    if (FieldHasObject(frame, snapshot, field)) {
      const int art_width = 2 * (bounds.w / record->region.columns);
      if (art_width > width) width = art_width;
    }
    if (width > widths[field->field_index]) widths[field->field_index] = width;
    const unsigned end_row = field->logical_line + 2 < record->region.rows
        ? field->logical_line + 2 : record->region.rows;
    const int height = bounds.h * (int)end_row / record->region.rows -
        bounds.h * (int)field->logical_line / record->region.rows;
    if (surface.height > height) height_fits = false;
  }
  if (!height_fits) return true;
  const ArLocalizationTextRowRule *template_row = ArLocalizationGrid_FindRow(
      grid, grid ? grid->shared_template_line : 0u, columns);
  if (!template_row || template_row->cell_count != columns) return false;
  for (unsigned i = 0; i < columns; ++i) {
    const ArLocalizationTextCellRule *cell = &template_row->cells[i];
    preferred[i] = bounds.w * (int)cell->end / record->region.columns -
        bounds.w * (int)cell->start / record->region.columns;
  }
  int unit = bounds.w / record->region.columns;
  if (unit < 1) unit = 1;
  int minimum_gap = unit / 4;
  if (minimum_gap < 1) minimum_gap = 1;
  *fits = ArLocalizedTextLayout_FitColumns(
      widths, preferred, columns, bounds.w, unit, minimum_gap, &plan->columns);
  if (*fits) plan->font_pixels = font_pixels;
  return true;
}

static bool ResolveReportPlan(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot, const ArTextCellRecord *record,
    const char *utf8, size_t utf8_bytes, const TableField *fields, size_t field_count,
    ArRenderRectI bounds, int base_pixels, int minimum_base_pixels, ReportPlan *out) {
  const TableField whole = {.utf8_bytes = utf8_bytes};
  ArTextRasterRequest request;
  if (!TableRequest(frame, snapshot, utf8, &whole, base_pixels, minimum_base_pixels,
                    bounds.w, bounds.h, false, &request))
    return false;
  const ArTextCacheKey key = ArTextSurfaceCache_MakeKey(
      ArTextBackendInstance_Get(&s_presenter.instance), &request);
  uint64_t digest =
      GridDigest(ArLocalizationFrame_GetGrid(frame, snapshot));
  /* Identical resolved bytes can have different authored column boundaries.
   * Include structure, not just text/font/geometry, in the fitting cache key. */
  for (size_t i = 0; i < field_count; ++i) {
    const uint32_t shape[] = {(uint32_t)fields[i].utf8_offset,
        (uint32_t)fields[i].utf8_bytes, fields[i].logical_line,
        fields[i].field_index, fields[i].field_count};
    digest = DeterministicHash_Fnv1a64(digest, shape, sizeof(shape));
  }
  for (unsigned i = 0; i < kReportPlanCapacity; ++i) {
    const ReportPlan *cached = &s_presenter.reports[i];
    if (cached->valid && cached->grid_digest == digest &&
        cached->region.columns == record->region.columns &&
        cached->region.rows == record->region.rows &&
        ArTextCacheKey_Equals(cached->key, key)) {
      *out = *cached;
      return cached->fits;
    }
  }
  ReportPlan result = {.key = key, .grid_digest = digest,
                       .region = record->region, .valid = true};
  bool fits;
  if (!MeasureReport(device, frame, snapshot, record, utf8, fields, field_count,
                      bounds, request.font_pixels, &result, &fits))
    return false; /* Backend/upload errors remain retryable. */
  result.fits = fits;
  if (!fits) {
    int low = request.minimum_font_pixels, high = request.font_pixels - 1;
    while (low <= high) {
      const int candidate = low + (high - low) / 2;
      ReportPlan attempt = result;
      if (!MeasureReport(device, frame, snapshot, record, utf8, fields, field_count,
                          bounds, candidate, &attempt, &fits))
        return false;
      if (fits) {
        result = attempt;
        result.fits = true;
        low = candidate + 1;
      } else high = candidate - 1;
    }
  }
  s_presenter.reports[s_presenter.next_report++ % kReportPlanCapacity] = result;
  *out = result;
  if (!result.fits)
    ReportRequestFailure(snapshot, &request, "report columns cannot fit at minimum font size");
  return result.fits;
}

static bool PrepareTable(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot,
    const ArTextCellRecord *record, const char *utf8, size_t utf8_bytes,
    ArRenderRectI bounds, const ReportPlan *report, ArLocalizedPreparedFrame *prepared) {
  TableField fields[kMaximumTableFields];
  size_t field_count = 0;
  unsigned logical_lines = 0;
  if (!ParseTableFields(frame, snapshot, utf8, utf8_bytes, fields,
                        kMaximumTableFields, &field_count, &logical_lines) ||
      !logical_lines ||
      logical_lines > kMaximumTableFields ||
      snapshot->inline_object_offset > frame->inline_object_count ||
      snapshot->inline_object_count >
          frame->inline_object_count - snapshot->inline_object_offset ||
      field_count > kArLocalizedPreparedTextCapacity - prepared->text_count)
    return false;

  const int base_pixels =
      (snapshot->native_font_pixels * bounds.h +
       (int)record->region.rows * 4) /
      ((int)record->region.rows * 8);
  int minimum_base_pixels = base_pixels * 2 / 3;
  if (minimum_base_pixels < 1) minimum_base_pixels = 1;
  const ArLocalizationTextGrid *grid =
      ArLocalizationFrame_GetGrid(frame, snapshot);
  if (!grid) return false;
  if (grid->shared_column_count && (!report || !report->fits)) return false;
  const uint8_t initial_text_count = prepared->text_count;
  const uint8_t initial_object_count = prepared->inline_object_count;
  /* A row whose outer cells reserve a gutter brackets an object between them;
   * the object is centred in the gap the fitted labels actually leave. */
  ArRenderRectI bracket_labels[2] = {{0}, {0}};
  int bracket_texts[2] = {-1, -1};
  int bracketed_object = -1;
  ArRenderRectI row_ink[kMaximumTableFields] = {{0}};
  TableRowBaseline baselines[kMaximumTableFields] = {{0}};
  uint8_t text_lines[kArLocalizedPreparedTextCapacity] = {0};
  int text_dy[kArLocalizedPreparedTextCapacity] = {0};
  unsigned object_lines[kArLocalizationFrameInlineObjectCapacity] = {0};
  uint8_t object_texts[kArLocalizationFrameInlineObjectCapacity] = {0};
  for (size_t field_index = 0; field_index < field_count; ++field_index) {
    const TableField *field = &fields[field_index];
    const ArLocalizationTextRowRule *rule = ArLocalizationGrid_FindRow(
        grid, field->logical_line, field->field_count);
    if (!rule || field->logical_line >= record->region.rows) goto fail;
    if (rule->native_reserved) {
      /* The adapter draws this row itself, keeping its gaps and palette. It
       * is neither localizable text nor a font decoration. */
      continue;
    }
    if (field->field_index >= rule->cell_count) goto fail;
    const ArLocalizationTextCellRule *cell =
        &rule->cells[field->field_index];
    ArTextHorizontalAlignment alignment = cell->alignment;
    int left = bounds.x +
        bounds.w * (int)cell->start / (int)record->region.columns;
    int right = bounds.x +
        bounds.w * (int)cell->end / (int)record->region.columns;
    const bool report_column = rule->shared_columns;
    if (report_column) {
      left = bounds.x + report->columns.left[field->field_index];
      right = bounds.x + report->columns.right[field->field_index];
    }
    if (cell->gutter_leading || cell->gutter_trailing) {
      int gutter = bounds.w / ((int)record->region.columns * 8);
      if (gutter < 1) gutter = 1;
      if (cell->gutter_trailing) right -= gutter;
      if (cell->gutter_leading) left += gutter;
    }
    const int top = bounds.y +
        bounds.h * (int)field->logical_line / (int)record->region.rows;
    int bottom = bounds.y +
        bounds.h * (int)(field->logical_line + grid->row_height) /
            (int)record->region.rows;
    if (bottom > bounds.y + bounds.h) bottom = bounds.y + bounds.h;
    if (right <= left || bottom <= top) goto fail;

    ArTextRasterRequest request;
    if (!TableRequest(frame, snapshot, utf8, field, base_pixels, minimum_base_pixels,
          report_column ? kReportMeasureExtent : right - left,
          report_column ? kReportMeasureExtent : bottom - top,
          grid->crop_rows, &request))
      goto fail;
    if (report_column)
      request.font_pixels = request.minimum_font_pixels = report->font_pixels;
    ArTextSurface surface;
    char error[kArTextRasterErrorCapacity] = {0};
    if (!ArTextSurfaceCache_Acquire(
            &s_presenter.cache, device,
            ArTextBackendInstance_Get(&s_presenter.instance),
            &request, &surface, error, sizeof(error))) {
      char diagnostic[kArTextRasterErrorCapacity] = {0};
      snprintf(diagnostic, sizeof(diagnostic),
               "grid line=%u cell=%u/%u max=%dx%d: %.128s",
               field->logical_line,
               field->field_index + 1u, field->field_count,
               request.maximum_width, request.maximum_height, error);
      ReportRequestFailure(snapshot, &request, diagnostic);
      goto fail;
    }
    if (surface.width > right - left || surface.height > bottom - top) goto fail;
    const ArTextDirection effective_direction = snapshot->language.direction == kArTextDirection_Auto
        ? surface.paragraph_direction : snapshot->language.direction;
    if (cell->follows_direction && alignment == kArTextHorizontalAlignment_Leading &&
        effective_direction == kArTextDirection_RightToLeft)
      alignment = kArTextHorizontalAlignment_Trailing;
    const int x = alignment == kArTextHorizontalAlignment_Center
        ? left + (right - left - surface.width) / 2
        : alignment == kArTextHorizontalAlignment_Trailing
            ? right - surface.width : left;
    const ArRenderRectI destination =
        {x, top, surface.width, surface.height};
    const ArRenderRectI ink = OutputInk(&surface, destination);
    text_lines[prepared->text_count] = (uint8_t)field->logical_line;
    if (ink.h > 0) {
      /* Independently fitted fields still share one typographic baseline.
       * Intersect their legal baseline ranges to keep every ink pixel inside
       * its own cell, including accents and descenders from fallback fonts. */
      TableRowBaseline *row = &baselines[field->logical_line];
      const int baseline = top + surface.ascent;
      const int desired = grid->center_rows
          ? baseline + top + (bottom - top - ink.h) / 2 - ink.y : baseline;
      const int minimum = baseline + top - ink.y;
      const int maximum = baseline + bottom - ink.y - ink.h;
      if (!row->active) {
        *row = (TableRowBaseline){desired, minimum, maximum, true};
      } else {
        if (desired > row->desired) row->desired = desired;
        if (minimum > row->minimum) row->minimum = minimum;
        if (maximum < row->maximum) row->maximum = maximum;
      }
    }
    if (cell->gutter_trailing) bracket_texts[0] = prepared->text_count;
    if (cell->gutter_leading) bracket_texts[1] = prepared->text_count;
    prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
        .surface = surface,
        .destination = destination,
        .revealed_cluster_count = (uint32_t)surface.reveal_cluster_count,
        .cluster_count = (uint32_t)surface.reveal_cluster_count,
        .cluster_shift_offset = -1,
    };

    for (uint8_t object_index = 0;
         object_index < snapshot->inline_object_count; ++object_index) {
      const ArLocalizationInlineObjectSnapshot *object =
          &frame->inline_objects[
              snapshot->inline_object_offset + object_index];
      if (object->end_utf8_byte <= field->utf8_offset ||
          object->end_utf8_byte > field->utf8_offset + field->utf8_bytes)
        continue;
      if (prepared->inline_object_count >=
          kArLocalizationFrameInlineObjectCapacity)
        goto fail;
      const uint32_t local_end =
          object->end_utf8_byte - (uint32_t)field->utf8_offset;
      const ArTextRevealCluster *cluster =
          FindRevealCluster(&surface, local_end, NULL);
      if (!cluster || !ArLocalizedTextArtwork_PrepareInlineObject(
              device, frame, snapshot, object->kind, &surface,
              utf8 + field->utf8_offset, field->utf8_bytes,
              cluster, destination, 0,
              &prepared->inline_objects[prepared->inline_object_count]))
        goto fail;
      ArLocalizedPreparedInlineObject *placed =
          &prepared->inline_objects[prepared->inline_object_count];
      object_lines[prepared->inline_object_count] = field->logical_line;
      object_texts[prepared->inline_object_count] = prepared->text_count - 1u;
      if (bracketed_object < 0 && bracket_texts[0] >= 0 &&
          field->field_index + 1u < rule->cell_count &&
          rule->cells[field->field_index + 1u].gutter_leading)
        bracketed_object = prepared->inline_object_count;
      if (object->kind == kArLocalizationInlineObject_StatusLife ||
          object->kind == kArLocalizationInlineObject_StatusPopulation ||
          object->kind == kArLocalizationInlineObject_SpeedDirection) {
        const int unit = bounds.w / (int)record->region.columns;
        const bool life = object->kind == kArLocalizationInlineObject_StatusLife;
        const int width = unit * (life ? 1 : 2);
        placed->destination = (ArRenderRectI){
            life ? destination.x + cluster->x + (cluster->width - width) / 2 :
                left + (right - left - width) / 2,
            top, width, unit};
      }
      ++prepared->inline_object_count;
    }
  }
  for (unsigned line = 0; line < logical_lines; ++line) {
    TableRowBaseline *row = &baselines[line];
    if (!row->active) continue;
    if (row->minimum > row->maximum) goto fail;
    if (row->desired > row->maximum) row->desired = row->maximum;
    if (row->desired < row->minimum) row->desired = row->minimum;
  }
  for (uint8_t i = initial_text_count; i < prepared->text_count; ++i) {
    ArLocalizedPreparedText *text = &prepared->texts[i];
    const unsigned line = text_lines[i];
    if (baselines[line].active) {
      text_dy[i] = baselines[line].desired - text->destination.y - text->surface.ascent;
      text->destination.y += text_dy[i];
    }
    row_ink[line] = ArLocalizedTextLayout_UnionInk(row_ink[line], OutputInk(&text->surface, text->destination));
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (bracket_texts[i] >= 0) {
      const ArLocalizedPreparedText *text = &prepared->texts[bracket_texts[i]];
      bracket_labels[i] = OutputInk(&text->surface, text->destination);
    }
  }
  if (bracketed_object >= 0 && !ArLocalizedTextLayout_CenterBetween(
          bracket_labels[0], bracket_labels[1],
          &prepared->inline_objects[bracketed_object].destination))
    goto fail;
  for (uint8_t i = initial_object_count; i < prepared->inline_object_count; ++i) {
    ArLocalizedPreparedInlineObject *object = &prepared->inline_objects[i];
    object->destination.y += text_dy[object_texts[i]];
    ArLocalizationArtworkKind art_kind;
    if (object->kind == kArLocalizationInlineObject_StatusLife)
      art_kind = kArLocalizationArtwork_Life;
    else if (object->kind == kArLocalizationInlineObject_StatusPopulation)
      art_kind = kArLocalizationArtwork_Population;
    else if (object->kind == kArLocalizationInlineObject_SpeedDirection)
      art_kind = kArLocalizationArtwork_SpeedDirection;
    else continue;
    const ArRenderRectI reference = row_ink[object_lines[i]];
    if (reference.h > 0 && !ArLocalizedTextLayout_CenterInkVertically(
            reference, ArLocalizedTextArtwork_Ink(art_kind),
            frame->artwork[art_kind].height, &object->destination))
      goto fail;
    if (!RectangleContains(bounds, object->destination)) goto fail;
  }
  return true;

fail:
  prepared->text_count = initial_text_count;
  prepared->inline_object_count = initial_object_count;
  return false;
}

static bool PrepareIndicators(
    ArRenderDevice *device, const ArLocalizationFrame *frame, uint32_t surface_id,
    unsigned map_width, unsigned map_height, uint16_t hscroll, uint16_t vscroll,
    unsigned visible_width, unsigned visible_height,
    const HudPresentationChunk *chunks, size_t chunk_count,
    ArLocalizedPreparedFrame *prepared) {
  const uint8_t first = prepared->indicator_count;
  for (uint8_t index = 0; index < frame->indicator_count; ++index) {
    const ArLocalizationIndicatorSnapshot *indicator = &frame->indicators[index];
    if (indicator->surface_id != surface_id) continue;
    if (prepared->indicator_count >= kArLocalizationFrameIndicatorCapacity)
      goto failed;
    ArRenderRectI projected[kArTextCellMaximumProjectedRegions];
    const size_t count = ArTextCellComposite_ProjectRegion(
        indicator->region, map_width, map_height, hscroll, vscroll,
        visible_width, visible_height, projected);
    ArRenderRectI destination;
    if (count != 1u ||
        !ProjectTextDestination(chunks, chunk_count, projected[0], &destination))
      goto failed;
    ArRenderTexture texture;
    if (!ArLocalizedTextArtwork_PrepareTexture(device, frame, kArLocalizationArtwork_Continue, &texture)) goto failed;
    prepared->indicators[prepared->indicator_count++] =
        (ArLocalizedPreparedIndicator){indicator->kind, destination, texture};
  }
  return true;
failed:
  prepared->indicator_count = first;
  return false;
}

static bool PrepareLabelFrame(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    ArTextCellRegion region, ArRenderRectI cells, ArRenderRectI *text_bounds,
    ArLocalizedPreparedDecoration ends[2]) {
  const ArLocalizationArtworkKind kinds[] = {
    kArLocalizationArtwork_LabelFrameLeft, kArLocalizationArtwork_LabelFrameRight,
  };
  for (unsigned i = 0; i < 2; ++i) {
    if (!ArLocalizedTextArtwork_PrepareTexture(device, frame, kinds[i], &ends[i].texture)) return false;
    const ArLocalizationArtwork *art = &frame->artwork[kinds[i]];
    const int width = (art->width * cells.w + region.columns * 4) / (region.columns * 8);
    const int height = (art->height * cells.h + region.rows * 4) / (region.rows * 8);
    if (width <= 0 || height <= 0 || height > cells.h) return false;
    ends[i].destination = (ArRenderRectI){0, cells.y + (cells.h - height) / 2, width, height};
  }
  const int available = text_bounds->w - ends[0].destination.w - ends[1].destination.w;
  if (available <= 0) return false;
  text_bounds->x += ends[0].destination.w;
  text_bounds->w = available;
  return true;
}

static void AppendLabelFrame(ArLocalizedPreparedFrame *prepared,
                             ArLocalizedPreparedDecoration ends[2], ArRenderRectI text) {
  ends[0].destination.x = text.x - ends[0].destination.w;
  ends[1].destination.x = text.x + text.w;
  memcpy(&prepared->decorations[prepared->decoration_count], ends, sizeof(*ends) * 2);
  prepared->decoration_count += 2;
}

/* A keyboard's native rows were a fixed cell grid. Proportional glyphs give
 * every row its own width, so without this the columns drift apart from row
 * to row and the last key of the widest row ends up against the window frame.
 * Only placement is regularized: the surface was shaped in one pass, so every
 * key keeps the same glyph size. Returns the offset of the surface's shifts in
 * the frame's pool, or -1 when its text flows. */
static int32_t PrepareKeyShifts(
    const ArLocalizationTextSnapshot *snapshot, const ArTextCellRecord *record,
    ArRenderRectI bounds, ArRenderRectI destination,
    const ArTextSurface *surface, const char *utf8, size_t utf8_bytes,
    ArLocalizedPreparedFrame *prepared, int *key_pitch_out) {
  /* Native pitch, not the claim divided by the key count: the claim can be
   * wider than the keyboard, and dividing it would spread the keys past where
   * the game put them and crowd the last one against the frame. */
  const int key_pitch = snapshot->key_cell_columns && record->region.columns
      ? bounds.w * (int)snapshot->key_cell_columns /
            (int)record->region.columns
      : 0;
  *key_pitch_out = key_pitch;
  /* The glyph occupies the last native cell of the key and the cells before
   * it are the selector's room, so its centre sits half a cell in from the
   * key's trailing edge. Derived from the stated cell count rather than
   * written as three quarters, which is only the same thing while a key is two
   * cells wide. */
  const int cells = snapshot->key_cell_columns;
  const int first_key_center = (bounds.x - destination.x) +
      (cells ? key_pitch * (2 * cells - 1) / (2 * cells) : 0);
  if (!snapshot->key_columns || !snapshot->key_trailing_lines ||
      key_pitch <= 0 ||
      surface->reveal_cluster_count >
          kArLocalizedPreparedClusterShiftCapacity -
              prepared->cluster_shift_count ||
      !ArLocalizedTextLayout_KeyCellShifts(
          surface, utf8, utf8_bytes, snapshot->key_separator,
          snapshot->key_separator_bytes, snapshot->key_columns,
          snapshot->key_trailing_lines, first_key_center, key_pitch,
          &prepared->cluster_shifts[prepared->cluster_shift_count],
          kArLocalizedPreparedClusterShiftCapacity -
              prepared->cluster_shift_count))
    return -1;
  const int32_t offset = (int32_t)prepared->cluster_shift_count;
  prepared->cluster_shift_count += surface->reveal_cluster_count;
  return offset;
}

/* The page line a hard line starting at `offset` was laid out on. Each
 * cluster records its line, and every line feed after the last cluster before
 * `offset` starts exactly one more; line feeds are never inside a cluster. */
static int LineIndexAt(const ArTextSurface *surface, const char *utf8,
                       size_t offset) {
  size_t from = 0;
  int line = 0;
  for (size_t index = 0; index < surface->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &surface->reveal_clusters[index];
    if (cluster->end_utf8_byte > offset) break;
    from = cluster->end_utf8_byte;
    line = cluster->line_index;
  }
  for (size_t at = from; at < offset; ++at) line += utf8[at] == '\n';
  return line;
}

/* Whether a grapheme is white space only: an empty field cell, which puts no
 * ink on screen and needs no surface of its own. */
static bool BlankGrapheme(const char *utf8, size_t bytes) {
  for (size_t offset = 0; offset < bytes;) {
    uint32_t scalar = 0;
    if (!ArUnicode_DecodeScalar(utf8, bytes, offset, &scalar, &offset))
      return false;
    if (scalar != ' ' && scalar != 0xa0 && scalar != 0x1680 &&
        !(scalar >= 0x2000 && scalar <= 0x200a) && scalar != 0x202f &&
        scalar != 0x205f && scalar != 0x3000)
      return false;
  }
  return true;
}

/* `value` moved to the nearest multiple of `quantum`: a surface placed at a
 * multiple of the page's low-resolution scale or mosaic block from the page's
 * own origin keeps its pixels on the page's grid. */
static int NearestMultiple(int value, int quantum) {
  if (quantum <= 1) return value;
  const int64_t shifted = (int64_t)value + quantum / 2;
  int64_t multiple = shifted / quantum;
  if (shifted % quantum < 0) --multiple;
  return (int)(multiple * quantum);
}

/* The width of one entry-field cell: an em of the page's fitted type, widened
 * when the widest key below (shading included) would not leave neighbours a
 * shadow's breadth apart, and rounded up to whole grid steps so every cell
 * starts on the page's pixel grid. */
static int FieldCellPitch(const ArLocalizationTextSnapshot *snapshot,
                          const ArTextSurface *page, int quantum) {
  const int em = page->raster_font_pixels * page->raster_scale;
  int widest = 0;
  if (snapshot->key_trailing_lines && page->cluster_ink_bounds) {
    int last_line = -1;
    for (size_t index = 0; index < page->reveal_cluster_count; ++index)
      if (page->reveal_clusters[index].line_index > last_line)
        last_line = page->reveal_clusters[index].line_index;
    const int first_keyed = last_line + 1 - (int)snapshot->key_trailing_lines;
    for (size_t index = 0; index < page->reveal_cluster_count; ++index)
      if (page->reveal_clusters[index].line_index >= first_keyed &&
          page->cluster_ink_bounds[index].w > widest)
        widest = page->cluster_ink_bounds[index].w;
  }
  int pitch = widest + (em + 4) / 8;
  if (pitch < em) pitch = em;
  if (quantum > 1) pitch = (pitch + quantum - 1) / quantum * quantum;
  return pitch;
}

/* How a live line was laid out: one surface for an ordinary line, or one per
 * inked cell of an entry field. */
typedef struct LiveLineLayout {
  ArLocalizedPreparedText texts[kArLocalizationFrameLiveLineMaximumCells];
  uint8_t text_count;
  /* Entry fields only: each cell's grapheme end within the line, the cell
   * width and the field's left edge. */
  size_t grapheme_ends[kArLocalizationFrameLiveLineMaximumCells];
  int pitch;
  int field_x;
} LiveLineLayout;

static bool SurfaceMatchesPage(const ArTextSurface *surface,
                               const ArTextSurface *page) {
  return surface->raster_font_pixels == page->raster_font_pixels &&
      surface->raster_scale == page->raster_scale &&
      surface->mosaic_block == page->mosaic_block &&
      surface->line_advance == page->line_advance &&
      surface->descent == page->descent &&
      surface->paragraph_direction != kArTextDirection_RightToLeft;
}

/* An entry field (see ArLocalizationTextSnapshot.live_line_cells): every
 * grapheme is rasterized alone -- so nothing joins or kerns -- and centred in
 * its own cell. Each surface is keyed by its grapheme alone, so a letter
 * typed anywhere in the field before costs no rasterization when it returns. */
static bool PrepareFieldCells(
    ArRenderDevice *device, const ArLocalizationTextSnapshot *snapshot,
    const char *line, size_t line_bytes, size_t line_start,
    const ArTextRasterRequest *request, const ArTextSurface *page,
    ArRenderRectI page_destination, int line_top, LiveLineLayout *layout) {
  enum { kCellSpanCapacity = 8 };
  const int quantum =
      page->mosaic_block > 1 ? page->mosaic_block : page->raster_scale;
  layout->pitch = FieldCellPitch(snapshot, page, quantum);
  layout->field_x = page_destination.x + page->origin_x;
  const int line_y = page_destination.y + page->origin_y + line_top;
  size_t offset = 0;
  uint8_t cell = 0;
  while (offset < line_bytes) {
    size_t next = 0;
    if (cell >= snapshot->live_line_cells ||
        !ArUnicodeGrapheme_Next(line, line_bytes, offset, NULL, &next) ||
        next <= offset || next > line_bytes)
      return false;
    layout->grapheme_ends[cell] = next;
    if (!BlankGrapheme(line + offset, next - offset)) {
      /* The value spans over this grapheme, as its own text sees them. */
      ArTextBidiSpan spans[kCellSpanCapacity];
      size_t span_count = 0;
      const size_t first = line_start + offset, last = line_start + next;
      for (size_t index = 0; index < request->bidi_span_count; ++index) {
        const ArTextBidiSpan span = request->bidi_spans[index];
        if (span.end <= first || span.start >= last) continue;
        if (span_count == kCellSpanCapacity) return false;
        spans[span_count++] = (ArTextBidiSpan){
            (span.start > first ? span.start : (uint32_t)first) - (uint32_t)first,
            (span.end < last ? span.end : (uint32_t)last) - (uint32_t)first,
            span.direction};
      }
      ArTextRasterRequest cell_request = *request;
      cell_request.utf8 = line + offset;
      cell_request.utf8_bytes = next - offset;
      cell_request.bidi_spans = span_count ? spans : NULL;
      cell_request.bidi_span_count = span_count;
      /* One grapheme never wraps, and a wrapping layout is as wide as its
       * box: without wrapping the surface is the letter's own size. Cropping
       * would move its origin by the letter's blank edge. */
      cell_request.flags &= ~(kArTextRasterFlag_CropHorizontalWhitespace |
                              kArTextRasterFlag_WrapWords |
                              kArTextRasterFlag_PreserveHardBreaks);
      cell_request.raster_font_pixels = page->raster_font_pixels;
      if (page->mosaic_block > 1) {
        /* Rows follow the page's grid; the cell's column is snapped to it
         * once the letter's width is known. */
        cell_request.align_pixelation_grid = true;
        cell_request.pixelation_grid_y = -page->origin_y - line_top;
      }
      ArTextSurface surface;
      char error[kArTextRasterErrorCapacity] = {0};
      if (!ArTextSurfaceCache_Acquire(
              &s_presenter.cache, device,
              ArTextBackendInstance_Get(&s_presenter.instance), &cell_request,
              &surface, error, sizeof(error)) ||
          !SurfaceMatchesPage(&surface, page) || !surface.reveal_cluster_count)
        return false;
      /* Centre the letterform itself, not its shading or advance. */
      int left = surface.reveal_clusters[0].x;
      int right = left + surface.reveal_clusters[0].width;
      for (size_t index = 0; index < surface.reveal_cluster_count; ++index) {
        const ArTextRevealCluster *cluster = &surface.reveal_clusters[index];
        if (cluster->line_index != 0) return false;
        if (cluster->x < left) left = cluster->x;
        if (cluster->x + cluster->width > right) right = cluster->x + cluster->width;
      }
      const int cell_left = layout->field_x + (int)cell * layout->pitch;
      const int centred = cell_left + (layout->pitch - (right - left)) / 2 - left;
      /* A glyph taller than the font's ascent pushes its own text's line
       * down; lift it back so every cell shares one baseline. */
      const int drop = surface.reveal_clusters[0].y - surface.origin_y;
      layout->texts[layout->text_count++] = (ArLocalizedPreparedText){
          .surface = surface,
          .destination = {
              page_destination.x +
                  NearestMultiple(centred - page_destination.x, quantum),
              line_y - surface.origin_y - NearestMultiple(drop, quantum),
              surface.width, surface.height},
          .revealed_cluster_count = (uint32_t)surface.reveal_cluster_count,
          .cluster_count = (uint32_t)surface.reveal_cluster_count,
          .cluster_shift_offset = -1,
      };
    }
    offset = next;
    ++cell;
  }
  return cell == snapshot->live_line_cells;
}

/* An ordinary live line: the line alone, drawn exactly where the whole page
 * would have drawn it. It lays out as it did inside the page -- each hard line
 * is its own bidi paragraph, shaped and placed from its own glyphs -- once it
 * is rasterized at the page's fitted size, starts at the page's layout origin
 * and, under mosaic, samples the page's blocks. */
static bool PrepareLiveLineAlone(
    ArRenderDevice *device, const char *line, size_t line_bytes,
    size_t line_start, const ArTextRasterRequest *request,
    const ArTextSurface *page, ArRenderRectI page_destination, int line_top,
    LiveLineLayout *layout) {
  ArTextRasterRequest line_request = *request;
  line_request.utf8 = line;
  line_request.utf8_bytes = line_bytes;
  /* The page's own spans, viewed from the line's first byte. */
  line_request.bidi_source_offset = (uint32_t)line_start;
  /* Cropping would drop a blank line, which has no ink to keep. Wrapping stays
   * on, so a line too long for the page still breaks and is refused below. */
  line_request.flags &= ~kArTextRasterFlag_CropHorizontalWhitespace;
  line_request.raster_font_pixels = page->raster_font_pixels;
  if (page->mosaic_block > 1) {
    /* The page's block boundaries, in the line's own layout coordinates. */
    line_request.align_pixelation_grid = true;
    line_request.pixelation_grid_x = -page->origin_x;
    line_request.pixelation_grid_y = -page->origin_y - line_top;
  }
  ArTextSurface surface;
  char error[kArTextRasterErrorCapacity] = {0};
  if (!ArTextSurfaceCache_Acquire(
          &s_presenter.cache, device,
          ArTextBackendInstance_Get(&s_presenter.instance), &line_request,
          &surface, error, sizeof(error)) ||
      !SurfaceMatchesPage(&surface, page))
    return false;
  for (size_t index = 0; index < surface.reveal_cluster_count; ++index)
    if (surface.reveal_clusters[index].line_index != 0) return false;
  /* Both layouts start from the same origin; the line sits `line_top` down. */
  layout->texts[layout->text_count++] = (ArLocalizedPreparedText){
      .surface = surface,
      .destination = {
          page_destination.x + page->origin_x - surface.origin_x,
          page_destination.y + page->origin_y + line_top - surface.origin_y,
          surface.width, surface.height},
      .revealed_cluster_count = (uint32_t)surface.reveal_cluster_count,
      .cluster_count = (uint32_t)surface.reveal_cluster_count,
      .cluster_shift_offset = -1,
  };
  return true;
}

/* Rasterizes a page whose live line (a name being typed) changes on its own as
 * the page with that line blanked, which stays cached while only the line
 * changes, plus the line: alone, or as an entry field of fixed cells. Every
 * other line keeps its placement, key shifts and objects.
 *
 * The blank is an inkless figure space inside the line's own value spans, so
 * the page keeps the layout engine and line count the whole text had, and an
 * ordinary line is drawn exactly as the whole text would have drawn it while
 * its ink stays within the other lines' extent, which a name above its
 * keyboard does.
 *
 * False leaves `prepared` as it was, for the caller to rasterize the page
 * whole: a layout this cannot reproduce (right-to-left paragraphs, a value
 * span crossing the line's edge or, for an ordinary line, no value span at
 * all; partial reveal, an accent, a live last line), or any failure. */
static bool PrepareLiveLinePage(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot, const ArTextCellRecord *record,
    const char *utf8, size_t utf8_bytes, const ArTextRasterRequest *request,
    ArRenderRectI bounds, uint32_t surface_id, unsigned bg3_map_width_tiles,
    unsigned bg3_map_height_tiles, uint16_t bg3_hscroll, uint16_t bg3_vscroll,
    unsigned visible_width, unsigned visible_height,
    const HudPresentationChunk *chunks, size_t chunk_count,
    ArLocalizedPreparedFrame *prepared) {
  static const char kBlank[] = "\xE2\x80\x87"; /* U+2007 FIGURE SPACE */
  const size_t blank_bytes = sizeof(kBlank) - 1u;
  const size_t line_start = snapshot->live_line_utf8_offset;
  const size_t line_bytes = snapshot->live_line_utf8_bytes;
  const size_t line_end = line_start + line_bytes;
  const uint8_t cells = snapshot->live_line_cells;
  if (!line_bytes || line_end >= utf8_bytes ||
      utf8_bytes - line_bytes + blank_bytes >
          sizeof(s_presenter.live_line_page) ||
      cells > kArLocalizationFrameLiveLineMaximumCells ||
      request->bidi_source_offset ||
      request->bidi_span_count > kArTextMaximumBidiSpans ||
      snapshot->language.direction == kArTextDirection_RightToLeft ||
      request->alignment != kArTextHorizontalAlignment_Leading ||
      snapshot->revealed_cluster_count < snapshot->cluster_count ||
      request->accent_end_utf8_byte ||
      snapshot->inline_object_offset > frame->inline_object_count ||
      snapshot->inline_object_count >
          frame->inline_object_count - snapshot->inline_object_offset ||
      snapshot->inline_object_count >
          kArLocalizationFrameInlineObjectCapacity -
              prepared->inline_object_count ||
      prepared->text_count + 1u + (cells ? cells : 1u) >
          kArLocalizedPreparedTextCapacity)
    return false;
  for (size_t index = 0; index < request->bidi_span_count; ++index) {
    const ArTextBidiSpan span = request->bidi_spans[index];
    if (span.start < line_end && span.end > line_start &&
        (span.start < line_start || span.end > line_end))
      return false;
  }

  /* The blank keeps the line and its line feed, so every later line keeps its
   * index, and so its place. */
  char *const page = s_presenter.live_line_page;
  memcpy(page, utf8, line_start);
  memcpy(page + line_start, kBlank, blank_bytes);
  memcpy(page + line_start + blank_bytes, utf8 + line_end,
         utf8_bytes - line_end);
  const size_t page_bytes = utf8_bytes - line_bytes + blank_bytes;
  ArTextBidiSpans *const spans = &s_presenter.live_line_spans;
  spans->count = (uint16_t)request->bidi_span_count;
  if (spans->count)
    memcpy(spans->spans, request->bidi_spans,
           spans->count * sizeof(spans->spans[0]));
  /* A value span covering the whole line (the typed name) now covers the
   * blank. A backend chooses its layout engine from the text itself when no
   * span remains, and the whole text may have chosen differently, so an
   * ordinary line is only split while a span keeps both on the same one. An
   * entry field never matched the whole text, so it has nothing to keep. */
  ArTextBidiSpans_Edit(spans, (uint32_t)line_start, (uint32_t)line_bytes,
                       (uint32_t)blank_bytes);
  if (!spans->count && !cells) return false;
  ArTextRasterRequest page_request = *request;
  page_request.utf8 = page;
  page_request.utf8_bytes = page_bytes;
  page_request.bidi_spans = spans->count ? spans->spans : NULL;
  page_request.bidi_span_count = spans->count;
  ArTextSurface page_surface;
  char error[kArTextRasterErrorCapacity] = {0};
  if (!ArTextSurfaceCache_Acquire(
          &s_presenter.cache, device,
          ArTextBackendInstance_Get(&s_presenter.instance), &page_request,
          &page_surface, error, sizeof(error)) ||
      !page_surface.raster_font_pixels ||
      page_surface.paragraph_direction == kArTextDirection_RightToLeft)
    return false;
  const int line_index = LineIndexAt(&page_surface, page, line_start);
  const int line_top = line_index * page_surface.line_advance;
  const ArRenderRectI page_destination = {
      bounds.x + (bounds.w - page_surface.width) / 2, bounds.y,
      page_surface.width, page_surface.height,
  };
  LiveLineLayout layout;
  layout.text_count = 0;
  if (cells ? !PrepareFieldCells(device, snapshot, utf8 + line_start,
                                 line_bytes, line_start, request,
                                 &page_surface, page_destination, line_top,
                                 &layout)
            : !PrepareLiveLineAlone(device, utf8 + line_start, line_bytes,
                                    line_start, request, &page_surface,
                                    page_destination, line_top, &layout))
    return false;

  const size_t first_shift = prepared->cluster_shift_count;
  int key_pitch = 0;
  const int32_t cluster_shift_offset = PrepareKeyShifts(
      snapshot, record, bounds, page_destination, &page_surface, page,
      page_bytes, prepared, &key_pitch);

  ArLocalizedPreparedInlineObject objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t object_count = 0;
  for (uint8_t index = 0; index < snapshot->inline_object_count; ++index) {
    const ArLocalizationInlineObjectSnapshot *object =
        &frame->inline_objects[snapshot->inline_object_offset + index];
    const uint32_t end = object->end_utf8_byte;
    bool placed = false;
    if (end > line_start && end <= line_end && cells) {
      /* A field's underlines mark its cells, not the letters in them. */
      uint8_t cell = 0;
      while (cell < cells && layout.grapheme_ends[cell] != end - line_start)
        ++cell;
      if (cell < cells &&
          object->kind == kArLocalizationInlineObject_NameFieldUnderline) {
        const ArTextRevealCluster slot = {
            .end_utf8_byte = end, .line_index = line_index,
            .x = layout.field_x + (int)cell * layout.pitch - page_destination.x,
            .width = layout.pitch, .height = page_surface.line_advance};
        objects[object_count] = (ArLocalizedPreparedInlineObject){
            .kind = object->kind};
        placed = ArLocalizedTextLayout_NameUnderline(
            &page_surface, &slot, page_destination,
            &objects[object_count].destination);
      }
    } else if (end > line_start && end <= line_end) {
      const ArLocalizedPreparedText *line = &layout.texts[0];
      const ArTextRevealCluster *cluster = FindRevealCluster(
          &line->surface, end - (uint32_t)line_start, NULL);
      placed = cluster && ArLocalizedTextArtwork_PrepareInlineObject(
          device, frame, snapshot, object->kind, &line->surface,
          utf8 + line_start, line_bytes, cluster, line->destination, 0,
          &objects[object_count]);
    } else {
      size_t reveal_index = 0;
      const ArTextRevealCluster *cluster = FindRevealCluster(
          &page_surface,
          end > line_end ? end - (uint32_t)line_bytes + (uint32_t)blank_bytes
                         : end,
          &reveal_index);
      ArRenderRectI object_destination = page_destination;
      if (cluster && cluster_shift_offset >= 0)
        object_destination.x += prepared->cluster_shifts[
            (size_t)cluster_shift_offset + reveal_index];
      placed = cluster && ArLocalizedTextArtwork_PrepareInlineObject(
          device, frame, snapshot, object->kind, &page_surface, page,
          page_bytes, cluster, object_destination,
          cluster_shift_offset >= 0 ? key_pitch / 2 : 0,
          &objects[object_count]);
    }
    if (!placed) {
      prepared->cluster_shift_count = first_shift;
      return false;
    }
    ++object_count;
  }
  if (!PrepareIndicators(device, frame, surface_id, bg3_map_width_tiles,
                         bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
                         visible_width, visible_height, chunks, chunk_count,
                         prepared)) {
    prepared->cluster_shift_count = first_shift;
    return false;
  }
  prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
      .surface = page_surface,
      .destination = page_destination,
      .revealed_cluster_count = snapshot->revealed_cluster_count,
      .cluster_count = snapshot->cluster_count,
      .cluster_shift_offset = cluster_shift_offset,
  };
  memcpy(&prepared->texts[prepared->text_count], layout.texts,
         (size_t)layout.text_count * sizeof(layout.texts[0]));
  prepared->text_count += layout.text_count;
  memcpy(&prepared->inline_objects[prepared->inline_object_count], objects,
         (size_t)object_count * sizeof(objects[0]));
  prepared->inline_object_count += object_count;
  return true;
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
  if (!device || !ArLocalizationFrame_IsValid(frame) ||
      !frame->snapshot_count ||
      !bg3_state_valid || !chunks || !chunk_count)
    return;
  /* Empty replacements claim cells without manufacturing a space glyph or
   * loading a font. Decide font activation before preparing any artwork,
   * since changing font stacks invalidates the presenter's texture cache. */
  bool font_ready = false;
  for (uint8_t index = 0; index < frame->snapshot_count; ++index) {
    if (frame->snapshots[index].utf8_bytes) {
      font_ready = ActivateFont(device, frame);
      break;
    }
  }

  /* Resolve all cold report fits before retaining any frame-owned surface
   * references. Font-size probes can evict LRU entries; they must never evict
   * a HUD/text surface already published into this prepared frame. */
  ArTextSurfaceCache_EndFrame(&s_presenter.cache);
  ReportPlan reports[kArTextCellRecordCapacity] = {0};
  for (unsigned pass = 0; pass < 2; ++pass) {
    /* Fitting probes retain no surfaces. Only the second pass publishes
     * handles that must survive until this prepared frame has been drawn. */
    if (pass == 1) ArTextSurfaceCache_BeginFrame(&s_presenter.cache);
    for (uint8_t record_index = 0; record_index < frame->cells.count; ++record_index) {
      const ArTextCellRecord *record = &frame->cells.records[record_index];
      if (record->destination.background != 3u ||
          record->destination.screen != kArTextCellScreen_Composited ||
          record->destination.tilemap_base_words != bg3_tilemap_base_words ||
          record->snapshot_slot < 0 || (uint8_t)record->snapshot_slot >= frame->snapshot_count)
        continue;
      if (pass == 0) {
        const ArLocalizationTextGrid *shared = ArLocalizationFrame_GetGrid(
            frame, &frame->snapshots[record->snapshot_slot]);
        if (!shared || !shared->shared_column_count) continue;
      }
      ArRenderRectI projected[kArTextCellMaximumProjectedRegions];
      const size_t projected_count = ArTextCellComposite_ProjectRegion(
          record->region, bg3_map_width_tiles, bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
          visible_width, visible_height, projected);
      if (projected_count != 1u || prepared->text_count >= kArLocalizedPreparedTextCapacity)
        continue;
      ArRenderRectI bounds;
      if (!ProjectTextDestination(chunks, chunk_count, projected[0], &bounds) || bounds.w <= 0 ||
          bounds.h <= 0)
        continue;
      const uint8_t snapshot_index = (uint8_t)record->snapshot_slot;
      const ArLocalizationTextSnapshot *snapshot = &frame->snapshots[snapshot_index];
      size_t utf8_bytes = 0;
      const char *utf8 = ArLocalizationFrame_GetText(frame, snapshot_index, &utf8_bytes);
      if (!utf8 || snapshot->surface_id != record->surface_id ||
          !ArLocalizationTextLanguage_IsValid(&snapshot->language) ||
          frame->bidi.count > kArTextMaximumBidiSpans ||
          snapshot->bidi_span_offset > frame->bidi.count ||
          snapshot->bidi_span_count > frame->bidi.count - snapshot->bidi_span_offset) continue;
      if ((utf8_bytes && !font_ready) ||
          (!utf8_bytes && (snapshot->cluster_count || snapshot->revealed_cluster_count ||
                           snapshot->revealed_utf8_bytes || snapshot->inline_object_count)))
        continue;
      if (pass == 0) {
        const ArLocalizationTextGrid *grid =
            ArLocalizationFrame_GetGrid(frame, snapshot);
        if (!utf8_bytes || !grid || !grid->shared_column_count) continue;
        TableField fields[kMaximumTableFields];
        size_t field_count = 0;
        unsigned lines = 0;
        if (snapshot->inline_object_offset > frame->inline_object_count ||
            snapshot->inline_object_count >
                frame->inline_object_count - snapshot->inline_object_offset ||
            !ParseTableFields(frame, snapshot, utf8, utf8_bytes, fields, kMaximumTableFields, &field_count,
                              &lines) ||
            lines > kMaximumTableFields)
          continue;
        const int pixels = (snapshot->native_font_pixels * bounds.h + record->region.rows * 4) /
                           (record->region.rows * 8);
        const int minimum = pixels * 2 / 3 > 0 ? pixels * 2 / 3 : 1;
        (void)ResolveReportPlan(device, frame, snapshot, record, utf8, utf8_bytes, fields,
                                field_count, bounds, pixels, minimum, &reports[record_index]);
        continue;
      }
      ArRenderRectI masks[kArTextCellMaximumChunkPieces];
      const size_t mask_count =
          BuildReplacementMasks(snapshot, projected[0], bg3_map_width_tiles, bg3_map_height_tiles,
                                bg3_hscroll, bg3_vscroll, visible_width, visible_height, masks);
      if (!mask_count || mask_count == SIZE_MAX ||
          prepared->mask_count + mask_count > sizeof(prepared->masks) / sizeof(prepared->masks[0]))
        continue;

      const ArRenderRectI cell_bounds = bounds;
      if (snapshot->left_inset_pixels || snapshot->right_inset_pixels) {
        const unsigned logical_width = record->region.columns * 8u;
        if (snapshot->left_inset_pixels + snapshot->right_inset_pixels >= logical_width)
          continue;
        const int left = (snapshot->left_inset_pixels * bounds.w + logical_width / 2) /
                         logical_width;
        const int right = (snapshot->right_inset_pixels * bounds.w + logical_width / 2) /
                          logical_width;
        bounds.x += left;
        bounds.w -= left + right;
        if (bounds.w <= 0) continue;
      }
      if (snapshot->top_inset_pixels) {
        if (snapshot->top_inset_pixels >= record->region.rows * 8u) continue;
        const int inset = (snapshot->top_inset_pixels * bounds.h + record->region.rows * 4) /
                         (record->region.rows * 8);
        bounds.y += inset;
        bounds.h -= inset;
        if (bounds.h <= 0) continue;
      }

      const bool framed_label = snapshot->layout == kArLocalizationTextLayout_FramedLabel;
      ArLocalizedPreparedDecoration frame_ends[2] = {0};
      if (framed_label &&
          (prepared->decoration_count + 2 > kArTextCellRecordCapacity * 2 ||
           !PrepareLabelFrame(device, frame, record->region, cell_bounds, &bounds, frame_ends)))
        continue;

      if (!utf8_bytes) {
        if (!PrepareIndicators(device, frame, record->surface_id, bg3_map_width_tiles,
                          bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
                          visible_width, visible_height, chunks, chunk_count, prepared))
          continue;
        memcpy(&prepared->masks[prepared->mask_count], masks, mask_count * sizeof(masks[0]));
        prepared->mask_count += mask_count;
        if (framed_label)
          AppendLabelFrame(prepared, frame_ends,
              (ArRenderRectI){bounds.x + bounds.w / 2, bounds.y, 0, 0});
        if (snapshot->layout == kArLocalizationTextLayout_DialogueWindow &&
            snapshot->surface_id == frame->dialogue_surface_id)
          prepared->ready_dialogue_ticket = frame->dialogue_ticket;
        continue;
      }

      const bool centered_label = framed_label ||
          snapshot->layout == kArLocalizationTextLayout_CenteredLabel;
      const bool right_label = snapshot->layout == kArLocalizationTextLayout_RightAlignedLabel;
      const bool left_label = snapshot->layout == kArLocalizationTextLayout_LeftAlignedLabel;
      const bool single_line = centered_label || right_label || left_label ||
          snapshot->layout == kArLocalizationTextLayout_SingleLineLabel;
      if (snapshot->layout == kArLocalizationTextLayout_Grid) {
        if (!PrepareTable(device, frame, snapshot, record, utf8, utf8_bytes, bounds,
                          &reports[record_index], prepared))
          continue;
        memcpy(&prepared->masks[prepared->mask_count], masks, mask_count * sizeof(masks[0]));
        prepared->mask_count += mask_count;
        continue;
      }

      const bool scrolling = snapshot->layout == kArLocalizationTextLayout_DialogueWindow;
      ArRenderRectI viewport = bounds;
      if (scrolling) {
        /* The claim includes a footer cell for the continuation marker. Reserve
         * it even while typing so appearing/disappearing arrows never reflow text. */
        int footer = bounds.h / record->region.rows;
        if (footer < 1) footer = 1;
        viewport.h -= footer;
        if (viewport.h <= 0) continue;
      }
      const int base_pixels =
          (snapshot->native_font_pixels * bounds.h + (int)record->region.rows * 4) /
          ((int)record->region.rows * 8);
      /* A one-tile label has a hard vertical limit, including accents and
       * low-resolution raster quantization. Permit extra fitting there. */
      int minimum_base_pixels = single_line ? base_pixels / 2 : base_pixels * 2 / 3;
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
          .style_id = snapshot->style_id,
          .band_rgb = snapshot->band_rgb,
          .body_rgb = snapshot->body_rgb,
          .accent_end_utf8_byte = snapshot->accent_end_utf8_byte,
          .accent_rgb = snapshot->accent_rgb,
          .shadow_rgb = snapshot->shadow_rgb,
          .shadow_enabled = snapshot->shadow_enabled,
          .shadow_shape = snapshot->shadow_shape,
          .flags = kArTextRasterFlag_WrapWords | kArTextRasterFlag_PreserveHardBreaks |
                   (snapshot->slant_ascii_numerals ? kArTextRasterFlag_SlantAsciiNumerals : 0u) |
                   kArTextRasterFlag_CropHorizontalWhitespace |
                   kArTextRasterFlag_IncludeRevealClusters,
          .direction = snapshot->language.direction,
          .bidi_spans = frame->bidi.spans + snapshot->bidi_span_offset,
          .bidi_span_count = snapshot->bidi_span_count,
          .preferred_line_breaks =
              scrolling ? frame->structural_boundaries : NULL,
          .preferred_line_break_capacity =
              scrolling ? kArLocalizationFrameTextCapacity : 0,
          .preferred_line_break_source_offset =
              scrolling ? snapshot->utf8_offset : 0,
          .alignment = right_label ? kArTextHorizontalAlignment_Right
              : left_label ? kArTextHorizontalAlignment_Left
                           : kArTextHorizontalAlignment_Leading,
          .font_pixels = base_pixels,
          .minimum_font_pixels = minimum_base_pixels,
          .maximum_width = bounds.w,
          .maximum_height = scrolling ? 4096 : bounds.h,
          .filter = kArRenderFilter_Nearest,
          .language_bcp47 = snapshot->language.locale,
          .language_bcp47_bytes = strlen(snapshot->language.locale),
      };
      if (!ArEnhancedTextSettings_Apply(&frame->settings, base_pixels, minimum_base_pixels,
                                        &request))
        continue;
      if (single_line) {
        request.flags &= ~(kArTextRasterFlag_WrapWords | kArTextRasterFlag_PreserveHardBreaks);
        request.flags |= kArTextRasterFlag_CropVerticalWhitespace;
        /* HUD scale and font scale are independent. Enlarging the preferred
         * font must not enlarge its fitting floor past a tiny physical row.
         * Keep the requested size; relax only the minimum when necessary. */
        const int minimum_for_row = bounds.h / 2 > 0 ? bounds.h / 2 : 1;
        if (request.minimum_font_pixels > minimum_for_row)
          request.minimum_font_pixels = minimum_for_row;
      }
      if (snapshot->italic) request.flags |= kArTextRasterFlag_Italic;
      if (scrolling) {
        /* Appending a continuation must not change the font size or horizontal
         * origin of earlier lines. Height overflow is handled by the viewport. */
        request.minimum_font_pixels = request.font_pixels;
        request.flags &= ~kArTextRasterFlag_CropHorizontalWhitespace;
      }
      if (!scrolling && !single_line && snapshot->live_line_utf8_bytes &&
          PrepareLiveLinePage(device, frame, snapshot, record, utf8, utf8_bytes,
                              &request, bounds, record->surface_id,
                              bg3_map_width_tiles, bg3_map_height_tiles,
                              bg3_hscroll, bg3_vscroll, visible_width,
                              visible_height, chunks, chunk_count, prepared)) {
        memcpy(&prepared->masks[prepared->mask_count], masks, mask_count * sizeof(masks[0]));
        prepared->mask_count += mask_count;
        continue;
      }
      ArTextSurface surface;
      char error[kArTextRasterErrorCapacity] = {0};
      if (!ArTextSurfaceCache_Acquire(&s_presenter.cache, device,
                                      ArTextBackendInstance_Get(&s_presenter.instance), &request,
                                      &surface, error, sizeof(error))) {
        ReportRequestFailure(snapshot, &request, error);
        continue;
      }
      const ArTextDirection effective_direction = snapshot->language.direction == kArTextDirection_Auto
          ? surface.paragraph_direction : snapshot->language.direction;
      const bool trailing = right_label ||
          (!left_label && effective_direction == kArTextDirection_RightToLeft);
      uint32_t revealed_clusters = snapshot->revealed_cluster_count;
      const int scroll_y =
          scrolling ? ArLocalizedTextLayout_ScrollOffset(&surface, snapshot->revealed_utf8_bytes,
                                                         viewport.h, &revealed_clusters)
                    : 0;
      const ArRenderRectI destination = {
          bounds.x +
              (scrolling || (single_line && !centered_label)
                   ? (trailing ? bounds.w - surface.width : 0)
                   : (bounds.w - surface.width) / 2),
          bounds.y - scroll_y + (single_line ? (bounds.h - surface.height) / 2 : 0),
          surface.width,
          surface.height,
      };
      int key_pitch = 0;
      const int32_t cluster_shift_offset = PrepareKeyShifts(
          snapshot, record, bounds, destination, &surface, utf8, utf8_bytes,
          prepared, &key_pitch);

      ArLocalizedPreparedInlineObject prepared_objects[kArLocalizationFrameInlineObjectCapacity];
      uint8_t prepared_object_count = 0;
      bool objects_valid =
          snapshot->inline_object_offset <= frame->inline_object_count &&
          snapshot->inline_object_count <=
              frame->inline_object_count - snapshot->inline_object_offset &&
          snapshot->inline_object_count <=
              kArLocalizationFrameInlineObjectCapacity - prepared->inline_object_count;
      for (uint8_t object_index = 0; objects_valid && object_index < snapshot->inline_object_count;
           ++object_index) {
        const ArLocalizationInlineObjectSnapshot *object =
            &frame->inline_objects[snapshot->inline_object_offset + object_index];
        size_t reveal_index = 0;
        const ArTextRevealCluster *cluster =
            FindRevealCluster(&surface, object->end_utf8_byte, &reveal_index);
        if (!cluster) {
          objects_valid = false;
          break;
        }
        if (reveal_index >= snapshot->revealed_cluster_count) continue;
        /* Objects hang off a cluster, so one that sits on a key follows it
         * onto its column instead of staying where the key was shaped. */
        ArRenderRectI object_destination = destination;
        if (cluster_shift_offset >= 0)
          object_destination.x += prepared->cluster_shifts[
              (size_t)cluster_shift_offset + reveal_index];
        if (!ArLocalizedTextArtwork_PrepareInlineObject(device, frame, snapshot, object->kind,
                                 &surface, utf8,
                                 utf8_bytes, cluster, object_destination,
                                 cluster_shift_offset >= 0 ? key_pitch / 2 : 0,
                                 &prepared_objects[prepared_object_count])) {
          objects_valid = false;
          break;
        }
        ++prepared_object_count;
      }
      if (!objects_valid) continue;
      if (!PrepareIndicators(device, frame, record->surface_id, bg3_map_width_tiles,
                              bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
                              visible_width, visible_height, chunks, chunk_count, prepared))
        continue;
      prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
          .surface = surface,
          .destination = destination,
          .revealed_cluster_count = revealed_clusters,
          .cluster_count = snapshot->cluster_count,
          .viewport = scrolling ? viewport : (ArRenderRectI){0},
          .cluster_shift_offset = cluster_shift_offset,
      };
      if (framed_label) AppendLabelFrame(prepared, frame_ends, destination);
      if (prepared_object_count) {
        memcpy(&prepared->inline_objects[prepared->inline_object_count], prepared_objects,
               (size_t)prepared_object_count * sizeof(prepared_objects[0]));
        prepared->inline_object_count += prepared_object_count;
      }
      memcpy(&prepared->masks[prepared->mask_count], masks, mask_count * sizeof(masks[0]));
      prepared->mask_count += mask_count;
      if (scrolling && snapshot->surface_id == frame->dialogue_surface_id)
        prepared->ready_dialogue_ticket = frame->dialogue_ticket;
    }
  }
}

bool ArLocalizedTextPresenter_PrepareScreenText(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    uint32_t surface_id, ArRenderRectI bounds,
    ArLocalizedPreparedFrame *prepared) {
  if (!prepared) return false;
  memset(prepared, 0, sizeof(*prepared));
  if (!device || bounds.w <= 0 || bounds.h <= 0 ||
      !ArLocalizationFrame_IsValid(frame))
    return false;
  const ArLocalizationScreenTextRecord *record =
      ArLocalizationFrame_FindScreenText(frame, surface_id);
  if (!record || record->snapshot_slot >= frame->snapshot_count)
    return false;
  const ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[record->snapshot_slot];
  size_t utf8_bytes = 0;
  const char *utf8 = ArLocalizationFrame_GetText(
      frame, record->snapshot_slot, &utf8_bytes);
  if (!utf8 || snapshot->surface_id != surface_id ||
      !ArLocalizationTextLanguage_IsValid(&snapshot->language) ||
      snapshot->bidi_span_offset > frame->bidi.count ||
      snapshot->bidi_span_count >
          frame->bidi.count - snapshot->bidi_span_offset)
    return false;
  if (!utf8_bytes) {
    /* A blank still constitutes the current prepared frame: release a prior
     * label's pin even though this one needs no font or texture. */
    if (s_presenter.cache_initialized)
      ArTextSurfaceCache_EndFrame(&s_presenter.cache);
    return !snapshot->cluster_count && !snapshot->revealed_cluster_count;
  }
  if (!ActivateFont(device, frame)) return false;

  const unsigned logical_width = record->width;
  if (snapshot->left_inset_pixels || snapshot->right_inset_pixels) {
    if (snapshot->left_inset_pixels + snapshot->right_inset_pixels >=
        logical_width)
      return false;
    const int left =
        (snapshot->left_inset_pixels * bounds.w + logical_width / 2) /
        logical_width;
    const int right =
        (snapshot->right_inset_pixels * bounds.w + logical_width / 2) /
        logical_width;
    bounds.x += left;
    bounds.w -= left + right;
    if (bounds.w <= 0) return false;
  }
  if (snapshot->top_inset_pixels) {
    if (snapshot->top_inset_pixels >= record->height) return false;
    const int top =
        (snapshot->top_inset_pixels * bounds.h + record->height / 2) /
        record->height;
    bounds.y += top;
    bounds.h -= top;
    if (bounds.h <= 0) return false;
  }

  int base_pixels =
      (snapshot->native_font_pixels * bounds.h + record->height / 2) /
      record->height;
  if (base_pixels < 1) base_pixels = 1;
  int minimum_base_pixels = base_pixels / 2;
  if (minimum_base_pixels < 1) minimum_base_pixels = 1;
  const bool centered =
      snapshot->layout == kArLocalizationTextLayout_CenteredLabel;
  const bool physical_right =
      snapshot->layout == kArLocalizationTextLayout_RightAlignedLabel;
  const bool physical_left =
      snapshot->layout == kArLocalizationTextLayout_LeftAlignedLabel;
  if (!centered && !physical_right && !physical_left &&
      snapshot->layout != kArLocalizationTextLayout_SingleLineLabel)
    return false;

  ArTextRasterRequest request = {
      .struct_size = sizeof(request),
      .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = utf8,
      .utf8_bytes = utf8_bytes,
      .font_stack_id = frame->font_stack_id,
      .font_stack_id_bytes = strlen(frame->font_stack_id),
      .source_revision = snapshot->source_revision,
      .font_revision = frame->font_revision,
      .style_id = snapshot->style_id,
      .band_rgb = snapshot->band_rgb,
      .body_rgb = snapshot->body_rgb,
      .accent_end_utf8_byte = snapshot->accent_end_utf8_byte,
      .accent_rgb = snapshot->accent_rgb,
      .shadow_rgb = snapshot->shadow_rgb,
      .shadow_enabled = snapshot->shadow_enabled,
      .shadow_shape = snapshot->shadow_shape,
      .flags = kArTextRasterFlag_CropHorizontalWhitespace |
               kArTextRasterFlag_CropVerticalWhitespace |
               kArTextRasterFlag_IncludeRevealClusters |
               (snapshot->slant_ascii_numerals
                    ? kArTextRasterFlag_SlantAsciiNumerals
                    : 0u),
      .direction = snapshot->language.direction,
      .bidi_spans = frame->bidi.spans + snapshot->bidi_span_offset,
      .bidi_span_count = snapshot->bidi_span_count,
      .alignment = physical_right ? kArTextHorizontalAlignment_Right
          : physical_left ? kArTextHorizontalAlignment_Left
                          : kArTextHorizontalAlignment_Leading,
      .font_pixels = base_pixels,
      .minimum_font_pixels = minimum_base_pixels,
      .maximum_width = bounds.w,
      .maximum_height = bounds.h,
      .filter = kArRenderFilter_Nearest,
      .language_bcp47 = snapshot->language.locale,
      .language_bcp47_bytes = strlen(snapshot->language.locale),
  };
  if (!ArEnhancedTextSettings_Apply(
          &frame->settings, base_pixels, minimum_base_pixels, &request))
    return false;
  const int minimum_for_row = bounds.h / 2 > 0 ? bounds.h / 2 : 1;
  if (request.minimum_font_pixels > minimum_for_row)
    request.minimum_font_pixels = minimum_for_row;
  if (snapshot->italic) request.flags |= kArTextRasterFlag_Italic;

  /* No other prepared frame is live on the mutually exclusive navigation
   * branch. Rotate retained cache references before acquiring this label. */
  ArTextSurfaceCache_EndFrame(&s_presenter.cache);
  ArTextSurfaceCache_BeginFrame(&s_presenter.cache);
  ArTextSurface surface;
  char error[kArTextRasterErrorCapacity] = {0};
  if (!ArTextSurfaceCache_Acquire(
          &s_presenter.cache, device,
          ArTextBackendInstance_Get(&s_presenter.instance), &request,
          &surface, error, sizeof(error))) {
    ReportRequestFailure(snapshot, &request, error);
    return false;
  }
  const ArTextDirection effective_direction =
      snapshot->language.direction == kArTextDirection_Auto
          ? surface.paragraph_direction
          : snapshot->language.direction;
  const bool trailing = physical_right ||
      (!physical_left && effective_direction == kArTextDirection_RightToLeft);
  const int x = centered ? bounds.x + (bounds.w - surface.width) / 2
      : trailing ? bounds.x + bounds.w - surface.width
                 : bounds.x;
  prepared->texts[0] = (ArLocalizedPreparedText){
      .surface = surface,
      .destination = {
          x, bounds.y + (bounds.h - surface.height) / 2,
          surface.width, surface.height},
      .revealed_cluster_count = snapshot->revealed_cluster_count,
      .cluster_count = snapshot->cluster_count,
      .cluster_shift_offset = -1,
  };
  prepared->text_count = 1;
  return true;
}

void ArLocalizedTextPresenter_Reset(ArRenderDevice *device) {
  DestroyPendingFont(device);
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
}
