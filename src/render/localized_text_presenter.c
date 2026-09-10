#include "render/localized_text_presenter.h"

#include <stdio.h>
#include <string.h>

#include "localization/enhanced_text_settings.h"
#include "render/localized_text_layout.h"
#include "render/text_cell_composite.h"
#include "deterministic_hash.h"

enum { kSurfaceCacheCapacity = 128, kFontSizeCacheCapacity = 8 };

typedef struct LocalizedTextPresenterState {
  ArTextBackend backend;
  ArTextBackendInstance instance;
  ArTextSurfaceCache cache;
  bool cache_initialized;
  char active_stack[kArLocalizationFrameFontStackCapacity];
  char active_path[kArLocalizationFrameFontPathCapacity];
  uint64_t active_revision;
  ArRenderTexture name_cursor;
  uint32_t name_cursor_argb[kArLocalizationFrameNameCursorPixels];
  ArRenderRectI name_cursor_ink;
  ArRenderTexture artwork_textures[kArLocalizationArtwork_Count];
  ArLocalizationArtwork artwork[kArLocalizationArtwork_Count];
  ArRenderRectI artwork_ink[kArLocalizationArtwork_Count];
  uint64_t reported_errors[32];
  unsigned next_reported_error;
} LocalizedTextPresenterState;

static LocalizedTextPresenterState s_presenter;

static void ReportOnce(const char *operation, const char *detail) {
  uint64_t key = DeterministicHash_Fnv1a64(
      DETERMINISTIC_HASH_FNV1A64_OFFSET, operation, strlen(operation));
  if (detail) key = DeterministicHash_Fnv1a64(key, detail, strlen(detail));
  if (!key) key = 1;
  for (size_t i = 0; i < 32; ++i)
    if (s_presenter.reported_errors[i] == key) return;
  s_presenter.reported_errors[s_presenter.next_reported_error++ % 32] = key;
  fprintf(stderr, "[localized-text] %s%s%s; native text retained\n",
          operation, detail && detail[0] ? ": " : "",
          detail && detail[0] ? detail : "");
}

void ArLocalizedTextPresenter_SetBackend(const ArTextBackend *backend) {
  s_presenter.backend = ArTextBackend_IsReady(backend)
      ? *backend : (ArTextBackend){0};
}

static void DestroyResources(ArRenderDevice *device) {
  for (unsigned i = 0; i < kArLocalizationArtwork_Count; ++i) {
    ArRenderDevice_DestroyTexture(device, s_presenter.artwork_textures[i]);
    s_presenter.artwork_textures[i] = ArRenderTexture_Invalid();
    memset(&s_presenter.artwork[i], 0, sizeof(s_presenter.artwork[i]));
    s_presenter.artwork_ink[i] = (ArRenderRectI){0};
  }
  ArRenderDevice_DestroyTexture(device, s_presenter.name_cursor);
  s_presenter.name_cursor = ArRenderTexture_Invalid();
  s_presenter.name_cursor_ink = (ArRenderRectI){0};
  if (s_presenter.cache_initialized)
    ArTextSurfaceCache_Destroy(&s_presenter.cache, device);
  ArTextBackendInstance_Destroy(&s_presenter.instance);
  s_presenter.cache_initialized = false;
  s_presenter.active_stack[0] = 0;
  s_presenter.active_path[0] = 0;
  s_presenter.active_revision = 0;
  memset(s_presenter.reported_errors, 0, sizeof(s_presenter.reported_errors));
  s_presenter.next_reported_error = 0;
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

static bool PrepareNameCursor(ArRenderDevice *device,
                              const ArLocalizationFrame *frame,
                              ArRenderTexture *texture) {
  if (!frame->name_cursor_valid) return false;
  if (!ArRenderTexture_IsValid(s_presenter.name_cursor) ||
      memcmp(s_presenter.name_cursor_argb, frame->name_cursor_argb,
             sizeof(s_presenter.name_cursor_argb))) {
    if (!ArRenderTexture_IsValid(s_presenter.name_cursor)) {
      const ArRenderTextureDesc desc = {
          .width = kArLocalizationFrameNameCursorExtent,
          .height = kArLocalizationFrameNameCursorExtent,
          .format = kArRenderPixelFormat_Argb8888,
          .usage = kArRenderTextureUsage_Static,
          .filter = kArRenderFilter_Nearest,
          .blend = kArRenderBlendMode_Alpha,
      };
      if (!ArRenderDevice_CreateTexture(device, &desc,
                                        &s_presenter.name_cursor))
        return false;
    }
    if (!ArRenderDevice_UpdateTexture(
            device, s_presenter.name_cursor, NULL, frame->name_cursor_argb,
            kArLocalizationFrameNameCursorExtent * (int)sizeof(uint32_t))) {
      ArRenderDevice_DestroyTexture(device, s_presenter.name_cursor);
      s_presenter.name_cursor = ArRenderTexture_Invalid();
      return false;
    }
    memcpy(s_presenter.name_cursor_argb, frame->name_cursor_argb,
           sizeof(s_presenter.name_cursor_argb));
    const ArTextBitmap bitmap = {
        .pixels = frame->name_cursor_argb,
        .width = kArLocalizationFrameNameCursorExtent,
        .height = kArLocalizationFrameNameCursorExtent,
        .pitch_bytes = kArLocalizationFrameNameCursorExtent * (int)sizeof(uint32_t),
        .format = kArRenderPixelFormat_Argb8888,
    };
    s_presenter.name_cursor_ink = ArTextBitmap_InkBounds(&bitmap,
        (ArRenderRectI){0, 0, bitmap.width, bitmap.height});
  }
  *texture = s_presenter.name_cursor;
  return true;
}

static bool PrepareArtwork(ArRenderDevice *device,
                            const ArLocalizationFrame *frame,
                            ArLocalizationArtworkKind kind,
                            ArRenderTexture *texture) {
  if ((unsigned)kind >= kArLocalizationArtwork_Count) return false;
  const ArLocalizationArtwork *art = &frame->artwork[kind];
  if (!art->valid || !art->width || !art->height ||
      art->width * art->height > kArLocalizationArtworkPixels)
    return false;
  ArRenderTexture *cached = &s_presenter.artwork_textures[kind];
  ArLocalizationArtwork *previous = &s_presenter.artwork[kind];
  if (ArRenderTexture_IsValid(*cached) && previous->width == art->width &&
      previous->height == art->height &&
      !memcmp(previous->argb, art->argb, sizeof(art->argb))) {
    *texture = *cached;
    return true;
  }
  if (previous->width != art->width || previous->height != art->height) {
    ArRenderDevice_DestroyTexture(device, *cached);
    *cached = ArRenderTexture_Invalid();
  }
  if (!ArRenderTexture_IsValid(*cached)) {
    const ArRenderTextureDesc desc = {
        .width = art->width, .height = art->height,
        .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Static,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, cached)) return false;
  }
  if (!ArRenderDevice_UpdateTexture(device, *cached, NULL, art->argb,
                                    art->width * (int)sizeof(uint32_t))) {
    ArRenderDevice_DestroyTexture(device, *cached);
    *cached = ArRenderTexture_Invalid();
    return false;
  }
  *previous = *art;
  const ArTextBitmap bitmap = {
      .pixels = art->argb, .width = art->width, .height = art->height,
      .pitch_bytes = art->width * (int)sizeof(uint32_t),
      .format = kArRenderPixelFormat_Argb8888,
  };
  s_presenter.artwork_ink[kind] = ArTextBitmap_InkBounds(&bitmap,
      (ArRenderRectI){0, 0, art->width, art->height});
  *texture = *cached;
  return true;
}

/* The adapter reserves repeated ASCII spaces between keyboard keys. Measure
 * that shaped room rather than assuming a space is a fraction of the line
 * height: a full line-height arrow can otherwise cover the previous key.
 * Use the narrowest gutter for stable sizing, including the first column and
 * narrow glyphs such as I; font fallback can change individual advances. */
static int NameCursorExtent(const ArTextSurface *surface, const char *utf8,
                             size_t utf8_bytes, int maximum_extent) {
  for (size_t index = 0; index < surface->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *first = &surface->reveal_clusters[index];
    const size_t end = first->end_utf8_byte;
    if (end >= utf8_bytes || !end || utf8[end - 1u] != ' ' || utf8[end] != ' ')
      continue;
    int right = first->x + first->width;
    size_t last_end = end;
    while (index + 1u < surface->reveal_cluster_count) {
      const ArTextRevealCluster *next = &surface->reveal_clusters[index + 1u];
      if (next->end_utf8_byte != last_end + 1u ||
          next->end_utf8_byte > utf8_bytes ||
          utf8[next->end_utf8_byte - 1u] != ' ' ||
          next->line_index != first->line_index)
        break;
      if (next->x + next->width > right) right = next->x + next->width;
      last_end = next->end_utf8_byte;
      ++index;
    }
    const int width = right - first->x;
    if (width > 0 && width < maximum_extent) maximum_extent = width;
  }
  return maximum_extent;
}

static ArRenderRectI UnionInk(ArRenderRectI a, ArRenderRectI b);

static bool PrepareInlineObject(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    ArLocalizationInlineObjectKind kind, const ArTextSurface *surface,
    const char *utf8, size_t utf8_bytes,
    const ArTextRevealCluster *cluster, ArRenderRectI text_destination,
    ArLocalizedPreparedInlineObject *prepared) {
  if (!surface || !cluster || !prepared) return false;
  if (kind == kArLocalizationInlineObject_NameFieldUnderline) {
    *prepared = (ArLocalizedPreparedInlineObject){.kind = kind};
    return ArLocalizedTextLayout_NameUnderline(
        surface, cluster, text_destination, &prepared->destination);
  }
  int extent = cluster->width;
  if (surface->line_advance > 0 && extent > surface->line_advance)
    extent = surface->line_advance;
  if (extent > cluster->height) extent = cluster->height;
  if (extent <= 0) return false;

  const bool name_cursor =
      kind == kArLocalizationInlineObject_NameCursor;
  ArRenderTexture texture = ArRenderTexture_Invalid();
  int object_height = extent;
  if (name_cursor) {
    object_height = cluster->height;
    if (surface->line_advance > 0 && object_height > surface->line_advance)
      object_height = surface->line_advance;
    object_height = NameCursorExtent(surface, utf8, utf8_bytes, object_height);
    if (object_height <= 0) return false;
    if (!PrepareNameCursor(device, frame, &texture)) return false;
  } else if (kind == kArLocalizationInlineObject_StatusLife ||
             kind == kArLocalizationInlineObject_StatusPopulation ||
             kind == kArLocalizationInlineObject_SpeedDirection) {
    const ArLocalizationArtworkKind art_kind =
        kind == kArLocalizationInlineObject_StatusLife ? kArLocalizationArtwork_Life :
        kind == kArLocalizationInlineObject_StatusPopulation ? kArLocalizationArtwork_Population :
        kArLocalizationArtwork_SpeedDirection;
    if (!PrepareArtwork(device, frame, art_kind, &texture)) return false;
  }
  *prepared = (ArLocalizedPreparedInlineObject){
      .kind = kind,
      .destination = {text_destination.x + cluster->x +
           (name_cursor ? -object_height : (cluster->width - extent) / 2),
       text_destination.y + cluster->y +
           (cluster->height - object_height) / 2,
       name_cursor ? object_height : extent, object_height},
      .texture = texture,
  };
  if ((name_cursor || kind == kArLocalizationInlineObject_NameBackspace ||
       kind == kArLocalizationInlineObject_NameFinish ||
       kind == kArLocalizationInlineObject_SelectionPointer) &&
      surface->cluster_ink_bounds) {
    /* Use the whole keyboard row's ink so accents/descenders on individual
     * keys never make the cursor jump vertically as selection changes. */
    ArRenderRectI ink = {0};
    for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
      if (surface->reveal_clusters[i].line_index == cluster->line_index)
        ink = UnionInk(ink, surface->cluster_ink_bounds[i]);
    }
    if (ink.h > 0) {
      ink.y += text_destination.y;
      return ArLocalizedTextLayout_CenterInkVertically(
          ink, name_cursor ? s_presenter.name_cursor_ink
                           : (ArRenderRectI){0, 0, extent, object_height},
          name_cursor ? kArLocalizationFrameNameCursorExtent : object_height,
          &prepared->destination);
    }
  }
  return true;
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

static ArRenderRectI UnionInk(ArRenderRectI a, ArRenderRectI b) {
  if (!a.w || !a.h) return b;
  if (!b.w || !b.h) return a;
  const int left = a.x < b.x ? a.x : b.x;
  const int top = a.y < b.y ? a.y : b.y;
  const int right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  const int bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return (ArRenderRectI){left, top, right - left, bottom - top};
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

/* Explicit `|` separators keep translated multiword labels in one cell.
 * Blank lines retain native row anchors and never become raster work. */
static bool ParseTableFields(const char *utf8, size_t utf8_bytes,
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
    while (line_end < utf8_bytes && utf8[line_end] != '\n') ++line_end;
    bool explicit_columns = false;
    for (size_t index = line_start; index < line_end; ++index)
      explicit_columns |= utf8[index] == '|';
    if (explicit_columns) {
      unsigned columns = 1;
      for (size_t index = line_start; index < line_end; ++index)
        columns += utf8[index] == '|';
      size_t cell_start = line_start;
      unsigned cell = 0;
      for (size_t index = line_start; index <= line_end; ++index) {
        if (index != line_end && utf8[index] != '|') continue;
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


static bool PrepareTable(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot,
    const ArTextCellRecord *record, const char *utf8, size_t utf8_bytes,
    ArRenderRectI bounds, ArLocalizedPreparedFrame *prepared) {
  TableField fields[kMaximumTableFields];
  size_t field_count = 0;
  unsigned logical_lines = 0;
  if (!ParseTableFields(utf8, utf8_bytes, fields,
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
  const uint8_t initial_text_count = prepared->text_count;
  const uint8_t initial_object_count = prepared->inline_object_count;
  ArRenderRectI speed_labels[2] = {{0}, {0}};
  int speed_texts[2] = {-1, -1};
  int speed_direction_object = -1;
  ArRenderRectI row_ink[kMaximumTableFields] = {{0}};
  TableRowBaseline baselines[kMaximumTableFields] = {{0}};
  uint8_t text_lines[kArLocalizedPreparedTextCapacity] = {0};
  int text_dy[kArLocalizedPreparedTextCapacity] = {0};
  unsigned object_lines[kArLocalizationFrameInlineObjectCapacity] = {0};
  uint8_t object_texts[kArLocalizationFrameInlineObjectCapacity] = {0};
  for (size_t field_index = 0; field_index < field_count; ++field_index) {
    const TableField *field = &fields[field_index];
    unsigned column = 0;
    unsigned next_column = 0;
    if (!ArLocalizedTextLayout_TableColumns(snapshot->layout, field->logical_line,
                     field->field_index, field->field_count,
                     &column, &next_column) ||
        next_column <= column ||
        field->logical_line >= record->region.rows)
      goto fail;
    if (next_column > record->region.columns)
      next_column = record->region.columns;
    const ArTextHorizontalAlignment alignment = ArLocalizedTextLayout_TableAlignment(
        snapshot->layout, field->logical_line,
        field->field_index, field->field_count, snapshot->direction);
    int left = bounds.x +
        bounds.w * (int)column / (int)record->region.columns;
    int right = bounds.x +
        bounds.w * (int)next_column / (int)record->region.columns;
    const bool speed_label_row =
        snapshot->layout == kArLocalizationTextLayout_MessageSpeed &&
        field->logical_line == 2 && field->field_count == 3;
    if (speed_label_row) {
      /* Leave one native pixel of breathing room on each side of the art,
       * even when a long translated label uses all of its available width. */
      int gutter = bounds.w / ((int)record->region.columns * 8);
      if (gutter < 1) gutter = 1;
      if (field->field_index == 0) right -= gutter;
      if (field->field_index == 2) left += gutter;
    }
    const int top = bounds.y +
        bounds.h * (int)field->logical_line / (int)record->region.rows;
    const unsigned row_height =
        snapshot->layout == kArLocalizationTextLayout_FixedRows &&
        record->region.rows <= 2 ? 1 : 2;
    int bottom = bounds.y +
        bounds.h * (int)(field->logical_line + row_height) /
            (int)record->region.rows;
    if (bottom > bounds.y + bounds.h) bottom = bounds.y + bounds.h;
    if (right <= left || bottom <= top) goto fail;

    if ((snapshot->layout == kArLocalizationTextLayout_StatusCities ||
         snapshot->layout == kArLocalizationTextLayout_StatusScore) &&
        field->logical_line == 5) {
      /* The adapter preserves this native graphics row, including its gaps
       * and palette. It is neither localizable text nor a font decoration. */
      continue;
    }

    bool has_inline_object = false;
    for (uint8_t object_index = 0;
         object_index < snapshot->inline_object_count; ++object_index) {
      const ArLocalizationInlineObjectSnapshot *object =
          &frame->inline_objects[
              snapshot->inline_object_offset + object_index];
      if (object->end_utf8_byte > field->utf8_offset &&
          object->end_utf8_byte <= field->utf8_offset + field->utf8_bytes) {
        has_inline_object = true;
        break;
      }
    }

    ArTextRasterRequest request = {
        .struct_size = sizeof(request),
        .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
        .utf8 = utf8 + field->utf8_offset,
        .utf8_bytes = field->utf8_bytes,
        .font_stack_id = frame->font_stack_id,
        .font_stack_id_bytes = strlen(frame->font_stack_id),
        .source_revision = snapshot->source_revision,
        .font_revision = frame->font_revision,
        .style_id = kArTextStyle_RetailBlueWhiteBands,
        .flags = kArTextRasterFlag_IncludeRevealClusters |
            ((snapshot->layout == kArLocalizationTextLayout_FixedRows &&
              record->region.rows <= 2)
                 ? kArTextRasterFlag_CropVerticalWhitespace : 0u) |
            (has_inline_object
                 ? 0u : kArTextRasterFlag_CropHorizontalWhitespace),
        .direction = snapshot->direction,
        .alignment = snapshot->direction == kArTextDirection_RightToLeft
            ? kArTextHorizontalAlignment_Trailing
            : kArTextHorizontalAlignment_Leading,
        .font_pixels = base_pixels,
        .minimum_font_pixels = minimum_base_pixels,
        .maximum_width = right - left,
        .maximum_height = bottom - top,
        .filter = kArRenderFilter_Nearest,
        .language_bcp47 = frame->locale,
        .language_bcp47_bytes = strlen(frame->locale),
    };
    if (!ArEnhancedTextSettings_Apply(
            &frame->settings, base_pixels, minimum_base_pixels, &request))
      goto fail;
    ArTextSurface surface;
    char error[kArTextRasterErrorCapacity] = {0};
    if (!ArTextSurfaceCache_Acquire(
            &s_presenter.cache, device,
            ArTextBackendInstance_Get(&s_presenter.instance),
            &request, &surface, error, sizeof(error))) {
      char diagnostic[kArTextRasterErrorCapacity] = {0};
      snprintf(diagnostic, sizeof(diagnostic),
               "layout=%u line=%u cell=%u/%u max=%dx%d: %.128s",
               (unsigned)snapshot->layout, field->logical_line,
               field->field_index + 1u, field->field_count,
               request.maximum_width, request.maximum_height, error);
      ReportRequestFailure(snapshot, &request, diagnostic);
      goto fail;
    }
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
      const int desired = top + surface.ascent;
      const int minimum = desired + top - ink.y;
      const int maximum = desired + bottom - ink.y - ink.h;
      if (!row->active) {
        *row = (TableRowBaseline){desired, minimum, maximum, true};
      } else {
        if (desired > row->desired) row->desired = desired;
        if (minimum > row->minimum) row->minimum = minimum;
        if (maximum < row->maximum) row->maximum = maximum;
      }
    }
    if (speed_label_row && field->field_index != 1)
      speed_texts[field->field_index / 2] = prepared->text_count;
    prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
        .surface = surface,
        .destination = destination,
        .revealed_cluster_count = (uint32_t)surface.reveal_cluster_count,
        .cluster_count = (uint32_t)surface.reveal_cluster_count,
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
      if (!cluster || !PrepareInlineObject(
              device, frame, object->kind, &surface,
              utf8 + field->utf8_offset, field->utf8_bytes,
              cluster, destination,
              &prepared->inline_objects[prepared->inline_object_count]))
        goto fail;
      ArLocalizedPreparedInlineObject *placed =
          &prepared->inline_objects[prepared->inline_object_count];
      object_lines[prepared->inline_object_count] = field->logical_line;
      object_texts[prepared->inline_object_count] = prepared->text_count - 1u;
      if (speed_label_row && object->kind == kArLocalizationInlineObject_SpeedDirection)
        speed_direction_object = prepared->inline_object_count;
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
    row_ink[line] = UnionInk(row_ink[line], OutputInk(&text->surface, text->destination));
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (speed_texts[i] >= 0) {
      const ArLocalizedPreparedText *text = &prepared->texts[speed_texts[i]];
      speed_labels[i] = OutputInk(&text->surface, text->destination);
    }
  }
  if (speed_direction_object >= 0 && !ArLocalizedTextLayout_CenterBetween(
          speed_labels[0], speed_labels[1],
          &prepared->inline_objects[speed_direction_object].destination))
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
            reference, s_presenter.artwork_ink[art_kind],
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
        prepared->text_count >= kArLocalizedPreparedTextCapacity)
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

    if (snapshot->layout != kArLocalizationTextLayout_Flow &&
        snapshot->layout != kArLocalizationTextLayout_DialogueWindow) {
      if (!PrepareTable(device, frame, snapshot, record, utf8, utf8_bytes,
                        bounds, prepared))
        continue;
      memcpy(&prepared->masks[prepared->mask_count], masks,
             mask_count * sizeof(masks[0]));
      prepared->mask_count += mask_count;
      continue;
    }

    const bool scrolling =
        snapshot->layout == kArLocalizationTextLayout_DialogueWindow;
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
        .maximum_height = scrolling ? 4096 : bounds.h,
        .filter = kArRenderFilter_Nearest,
        .language_bcp47 = frame->locale,
        .language_bcp47_bytes = strlen(frame->locale),
  };
    if (!ArEnhancedTextSettings_Apply(
            &frame->settings, base_pixels, minimum_base_pixels, &request))
      continue;
    if (scrolling) {
      /* Appending a continuation must not change the font size or horizontal
       * origin of earlier lines. Height overflow is handled by the viewport. */
      request.minimum_font_pixels = request.font_pixels;
      request.flags &= ~kArTextRasterFlag_CropHorizontalWhitespace;
    }
    ArTextSurface surface;
    char error[kArTextRasterErrorCapacity] = {0};
    if (!ArTextSurfaceCache_Acquire(
            &s_presenter.cache, device,
            ArTextBackendInstance_Get(&s_presenter.instance),
            &request, &surface, error, sizeof(error))) {
      ReportRequestFailure(snapshot, &request, error);
      continue;
    }
    uint32_t revealed_clusters = snapshot->revealed_cluster_count;
    const int scroll_y = scrolling ? ArLocalizedTextLayout_ScrollOffset(
        &surface, snapshot->revealed_utf8_bytes, viewport.h, &revealed_clusters) : 0;
    const ArRenderRectI destination = {
        bounds.x + (scrolling
            ? (snapshot->direction == kArTextDirection_RightToLeft
                ? bounds.w - surface.width : 0)
            : (bounds.w - surface.width) / 2),
        bounds.y - scroll_y,
        surface.width,
        surface.height,
    };
    ArLocalizedPreparedInlineObject prepared_objects[
        kArLocalizationFrameInlineObjectCapacity];
    uint8_t prepared_object_count = 0;
    bool objects_valid =
        snapshot->inline_object_offset <= frame->inline_object_count &&
        snapshot->inline_object_count <=
            frame->inline_object_count - snapshot->inline_object_offset &&
        snapshot->inline_object_count <=
            kArLocalizationFrameInlineObjectCapacity -
                prepared->inline_object_count;
    for (uint8_t object_index = 0;
         objects_valid && object_index < snapshot->inline_object_count;
         ++object_index) {
      const ArLocalizationInlineObjectSnapshot *object =
          &frame->inline_objects[
              snapshot->inline_object_offset + object_index];
      size_t reveal_index = 0;
      const ArTextRevealCluster *cluster = FindRevealCluster(
          &surface, object->end_utf8_byte, &reveal_index);
      if (!cluster) {
        objects_valid = false;
        break;
      }
      if (reveal_index >= snapshot->revealed_cluster_count) continue;
      if (!PrepareInlineObject(
              device, frame, object->kind, &surface, utf8, utf8_bytes,
              cluster, destination,
              &prepared_objects[prepared_object_count])) {
        objects_valid = false;
        break;
      }
      ++prepared_object_count;
    }
    if (!objects_valid) continue;
    prepared->texts[prepared->text_count++] = (ArLocalizedPreparedText){
        .surface = surface,
        .destination = destination,
        .revealed_cluster_count = revealed_clusters,
        .cluster_count = snapshot->cluster_count,
        .viewport = scrolling ? viewport : (ArRenderRectI){0},
    };
    if (prepared_object_count) {
      memcpy(&prepared->inline_objects[prepared->inline_object_count],
             prepared_objects,
             (size_t)prepared_object_count * sizeof(prepared_objects[0]));
      prepared->inline_object_count += prepared_object_count;
    }
    for (uint8_t indicator_index = 0;
         indicator_index < frame->indicator_count; ++indicator_index) {
      const ArLocalizationIndicatorSnapshot *indicator =
          &frame->indicators[indicator_index];
      if (indicator->surface_id != record->surface_id ||
          prepared->indicator_count >= kArLocalizationFrameIndicatorCapacity)
        continue;
      ArRenderRectI projected_indicator[
          kArTextCellMaximumProjectedRegions];
      const size_t projected_indicator_count =
          ArTextCellComposite_ProjectRegion(
              indicator->region, bg3_map_width_tiles,
              bg3_map_height_tiles, bg3_hscroll, bg3_vscroll,
              visible_width, visible_height, projected_indicator);
      ArRenderRectI indicator_destination;
      if (projected_indicator_count != 1u ||
          !ProjectTextDestination(chunks, chunk_count,
                                  projected_indicator[0],
                                  &indicator_destination))
        continue;
      ArRenderTexture texture;
      if (!PrepareArtwork(device, frame, kArLocalizationArtwork_Continue,
                           &texture))
        continue;
      prepared->indicators[prepared->indicator_count++] =
          (ArLocalizedPreparedIndicator){
              indicator->kind, indicator_destination, texture};
    }
    memcpy(&prepared->masks[prepared->mask_count], masks,
           mask_count * sizeof(masks[0]));
    prepared->mask_count += mask_count;
  }
}

static bool DrawContinueIndicator(
    ArRenderDevice *device, const ArLocalizedPreparedIndicator *indicator) {
  const ArRenderRectI bounds = indicator->destination;
  const ArRenderRectF destination = {bounds.x, bounds.y, bounds.w, bounds.h};
  return ArRenderTexture_IsValid(indicator->texture) &&
      ArRenderDevice_DrawTexture(device, indicator->texture, NULL, &destination);
}

static bool DrawTriangle(ArRenderDevice *device,
                         ArRenderPointF first,
                         ArRenderPointF second,
                         ArRenderPointF third,
                         ArRenderColorF color) {
  const ArRenderVertex2D vertices[] = {
      {first, color, {0, 0}}, {second, color, {0, 0}},
      {third, color, {0, 0}},
  };
  const int32_t indices[] = {0, 1, 2};
  return ArRenderDevice_DrawGeometry(
      device, ArRenderTexture_Invalid(), vertices, 3, indices, 3);
}

static bool DrawInlineObject(
    ArRenderDevice *device, const ArLocalizedPreparedInlineObject *object) {
  if (!device || !object || object->destination.w <= 0 ||
      object->destination.h <= 0)
    return false;
  const ArRenderRectI bounds = object->destination;
  const float x = (float)bounds.x;
  const float y = (float)bounds.y;
  const float w = (float)bounds.w;
  const float h = (float)bounds.h;
  const float thickness = w > 12.0f ? w * 0.12f : 1.0f;
  const ArRenderColorF blue = {0.12f, 0.29f, 0.68f, 1.0f};
  const ArRenderColorF white = {0.96f, 0.98f, 1.0f, 1.0f};
  switch (object->kind) {
    case kArLocalizationInlineObject_SelectionPointer:
      return DrawTriangle(
                 device,
                 (ArRenderPointF){x + w * 0.14f, y + h * 0.12f},
                 (ArRenderPointF){x + w * 0.14f, y + h * 0.88f},
                 (ArRenderPointF){x + w * 0.90f, y + h * 0.50f}, blue) &&
          DrawTriangle(
                 device,
                 (ArRenderPointF){x + w * 0.25f, y + h * 0.28f},
                 (ArRenderPointF){x + w * 0.25f, y + h * 0.72f},
                 (ArRenderPointF){x + w * 0.70f, y + h * 0.50f}, white);
    case kArLocalizationInlineObject_NameBackspace: {
      const ArRenderRectF stem =
          {x + w * 0.30f, y + h * 0.42f, w * 0.55f, h * 0.18f};
      return ArRenderDevice_DrawSolidRect(
                 device, &stem, white, kArRenderBlendMode_Alpha) &&
          DrawTriangle(
              device, (ArRenderPointF){x + w * 0.12f, y + h * 0.50f},
              (ArRenderPointF){x + w * 0.44f, y + h * 0.18f},
              (ArRenderPointF){x + w * 0.44f, y + h * 0.82f}, white);
    }
    case kArLocalizationInlineObject_NameFinish:
      return ArRenderDevice_DrawLine(
                 device,
                 (ArRenderPointF){x + w * 0.16f, y + h * 0.54f},
                 (ArRenderPointF){x + w * 0.40f, y + h * 0.78f},
                 thickness, blue, kArRenderBlendMode_Alpha) &&
          ArRenderDevice_DrawLine(
                 device,
                 (ArRenderPointF){x + w * 0.40f, y + h * 0.78f},
                 (ArRenderPointF){x + w * 0.86f, y + h * 0.20f},
                 thickness, white, kArRenderBlendMode_Alpha);
    case kArLocalizationInlineObject_StatusLife:
    case kArLocalizationInlineObject_StatusPopulation:
    case kArLocalizationInlineObject_SpeedDirection:
    case kArLocalizationInlineObject_NameCursor: {
      const ArRenderRectF destination = {x, y, w, h};
      return ArRenderTexture_IsValid(object->texture) &&
          ArRenderDevice_DrawTexture(
              device, object->texture, NULL, &destination);
    }
    case kArLocalizationInlineObject_NameFieldUnderline: {
      const ArRenderRectF lower = {x, y, w, h};
      const ArRenderRectF upper = {x, y, w, h * 0.5f};
      return ArRenderDevice_DrawSolidRect(
                 device, &lower, blue, kArRenderBlendMode_Alpha) &&
          ArRenderDevice_DrawSolidRect(
                 device, &upper, white, kArRenderBlendMode_Alpha);
    }
    default:
      return false;
  }
}

static bool DrawTextPiece(ArRenderDevice *device,
                          const ArLocalizedPreparedText *text,
                          ArRenderRectI source, ArRenderRectI target) {
  if (text->viewport.w > 0 &&
      !ArLocalizedTextLayout_Clip(text->viewport, &source, &target))
    return true;
  const ArRenderRectF source_f = {
      (float)source.x, (float)source.y, (float)source.w, (float)source.h};
  const ArRenderRectF target_f = {
      (float)target.x, (float)target.y, (float)target.w, (float)target.h};
  return ArRenderDevice_DrawTexture(
      device, text->surface.texture, &source_f, &target_f);
}

static bool DrawOne(ArRenderDevice *device,
                    const ArLocalizedPreparedText *text) {
  const ArRenderRectI destination = text->destination;
  size_t revealed = text->revealed_cluster_count;
  if (revealed > text->surface.reveal_cluster_count)
    revealed = text->surface.reveal_cluster_count;
  if (revealed >= text->surface.reveal_cluster_count)
    return DrawTextPiece(device, text,
        (ArRenderRectI){0, 0, destination.w, destination.h}, destination);
  for (size_t index = 0; index < revealed; ++index) {
    const ArTextRevealCluster *cluster =
        &text->surface.reveal_clusters[index];
    if (cluster->width <= 0 || cluster->height <= 0) continue;
    const ArRenderRectI source = {
        cluster->x, cluster->y, cluster->width, cluster->height,
    };
    const ArRenderRectI target = {
        destination.x + source.x, destination.y + source.y,
        source.w, source.h,
    };
    if (!DrawTextPiece(device, text, source, target))
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
  for (uint8_t index = 0; index < prepared->indicator_count; ++index) {
    const ArLocalizedPreparedIndicator *indicator =
        &prepared->indicators[index];
    if (indicator->kind == kArLocalizationIndicator_DialogueContinue &&
        !DrawContinueIndicator(device, indicator))
      return false;
  }
  for (uint8_t index = 0; index < prepared->inline_object_count; ++index) {
    if (!DrawInlineObject(device, &prepared->inline_objects[index]))
      return false;
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
}
