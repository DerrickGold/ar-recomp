#include "localization/localization_frame.h"
#include "localization/text_appearance.h"

#include <stddef.h>
#include <string.h>

#define AR_MEMBER_END(type, member) \
  (offsetof(type, member) + sizeof(((type *)0)->member))

static bool FrameStorageValid(const ArLocalizationFrame *frame) {
  return frame &&
         frame->struct_size >=
             AR_MEMBER_END(ArLocalizationFrame, dialogue_surface_id) &&
         frame->abi_version == AR_LOCALIZATION_FRAME_ABI_VERSION &&
         frame->cells.count <= kArTextCellRecordCapacity &&
         frame->screen_text_count <= kArLocalizationFrameScreenTextCapacity &&
         frame->snapshot_count <= kArTextCellRecordCapacity &&
         frame->bidi.count <= kArTextMaximumBidiSpans &&
         frame->appearance_span_count <=
             kArLocalizationFrameAppearanceCapacity &&
         frame->grid_count <= kArLocalizationFrameGridCapacity &&
         frame->indicator_count <= kArLocalizationFrameIndicatorCapacity &&
         frame->inline_object_count <=
             kArLocalizationFrameInlineObjectCapacity &&
         frame->fallback_font_count <=
             kArTextPresentationMaximumFallbackFonts &&
         frame->font_role_count <= kArTextFontMaximumRoles &&
         frame->text_bytes <= kArLocalizationFrameTextCapacity;
}

static bool SnapshotTextValid(const ArLocalizationFrame *frame,
                              const ArLocalizationTextSnapshot *snapshot);

static bool CopyString(char *destination, size_t capacity,
                       const char *source) {
  const size_t length = source ? strlen(source) : 0;
  if (!source || !length || length >= capacity) return false;
  memcpy(destination, source, length + 1u);
  return true;
}

static bool RegionContains(ArTextCellRegion outer,
                           ArTextCellRegion inner) {
  return inner.columns && inner.rows &&
      inner.column >= outer.column && inner.row >= outer.row &&
      (unsigned)inner.column + inner.columns <=
          (unsigned)outer.column + outer.columns &&
      (unsigned)inner.row + inner.rows <=
          (unsigned)outer.row + outer.rows;
}

void ArLocalizationFrame_InitCleared(ArLocalizationFrame *frame) {
  if (!frame) return;
  frame->struct_size = sizeof(*frame);
  frame->abi_version = AR_LOCALIZATION_FRAME_ABI_VERSION;
  ArTextCellRecordSet_Reset(&frame->cells);
  ArEnhancedTextSettings_Defaults(&frame->settings);
}

void ArLocalizationFrame_Reset(ArLocalizationFrame *frame) {
  if (!frame) return;
  memset(frame, 0, sizeof(*frame));
  ArLocalizationFrame_InitCleared(frame);
}

void ArLocalizationFrame_ReleaseText(ArLocalizationFrame *frame,
                                     uint32_t surface_id) {
  if (!FrameStorageValid(frame))
    return;
  ArTextCellRecordSet_Release(&frame->cells, surface_id);
  uint8_t kept = 0;
  for (uint8_t i = 0; i < frame->screen_text_count; ++i)
    if (frame->screen_texts[i].surface_id != surface_id)
      frame->screen_texts[kept++] = frame->screen_texts[i];
  frame->screen_text_count = kept;
  kept = 0;
  for (uint8_t i = 0; i < frame->indicator_count; ++i)
    if (frame->indicators[i].surface_id != surface_id)
      frame->indicators[kept++] = frame->indicators[i];
  frame->indicator_count = kept;
  if (frame->dialogue_surface_id == surface_id) {
    frame->dialogue_surface_id = 0;
    frame->dialogue_ticket = 0;
  }
}

bool ArLocalizationFrame_SetFont(ArLocalizationFrame *frame,
                                 const char *locale,
                                 const char *font_stack_id,
                                 ArFontResourceId primary_font,
                                 uint64_t font_revision,
                                 const ArEnhancedTextSettings *settings) {
  if (!FrameStorageValid(frame) ||
      !primary_font || !font_revision || !ArEnhancedTextSettings_IsValid(settings))
    return false;
  char locale_copy[kArLocalizationFrameLocaleCapacity] = {0};
  char stack_copy[kArLocalizationFrameFontStackCapacity] = {0};
  if (!CopyString(locale_copy, sizeof(locale_copy), locale) ||
      !CopyString(stack_copy, sizeof(stack_copy), font_stack_id))
    return false;
  memcpy(frame->locale, locale_copy, sizeof(locale_copy));
  memcpy(frame->font_stack_id, stack_copy, sizeof(stack_copy));
  frame->primary_font = primary_font;
  frame->font_revision = font_revision;
  frame->settings = *settings;
  frame->fallback_font_count = 0;
  memset(frame->fallback_fonts, 0, sizeof(frame->fallback_fonts));
  frame->font_role_count = 0;
  memset(frame->font_roles, 0, sizeof(frame->font_roles));
  return true;
}

bool ArLocalizationFrame_SetFontRoles(ArLocalizationFrame *frame,
                                      const ArTextFontRole *roles,
                                      size_t count) {
  if (!FrameStorageValid(frame) || !frame->font_revision ||
      !ArTextFontRoles_Valid(roles, count))
    return false;
  ArTextFontRole copied[kArTextFontMaximumRoles] = {0};
  if (count)
    memcpy(copied, roles, count * sizeof(*roles));
  memcpy(frame->font_roles, copied, sizeof(copied));
  frame->font_role_count = (uint8_t)count;
  return true;
}

bool ArLocalizationFrame_SetFallbackFonts(
    ArLocalizationFrame *frame, const ArFontResourceId *fonts, size_t count) {
  if (!FrameStorageValid(frame) ||
      !frame->font_revision || count > kArTextPresentationMaximumFallbackFonts ||
      (count && !fonts))
    return false;
  ArFontResourceId copied[kArTextPresentationMaximumFallbackFonts] = {0};
  for (size_t i = 0; i < count; ++i)
    if (!(copied[i] = fonts[i])) return false;
  memcpy(frame->fallback_fonts, copied, sizeof(copied));
  frame->fallback_font_count = (uint8_t)count;
  return true;
}

bool ArLocalizationTextLanguage_IsValid(const ArLocalizationTextLanguage *language) {
  return language && language->locale[0] &&
      memchr(language->locale, 0, sizeof(language->locale)) &&
      language->direction >= kArTextDirection_Auto &&
      language->direction <= kArTextDirection_RightToLeft;
}

bool ArLocalizationFrame_SetTextLanguage(
    ArLocalizationFrame *frame, const ArLocalizationTextLanguage *language) {
  if (!FrameStorageValid(frame) ||
      !frame->snapshot_count || frame->snapshot_count > kArTextCellRecordCapacity ||
      !ArLocalizationTextLanguage_IsValid(language))
    return false;
  frame->snapshots[frame->snapshot_count - 1u].language = *language;
  return true;
}

bool ArLocalizationFrame_SetTextBidiSpans(
    ArLocalizationFrame *frame, const ArTextBidiSpans *spans) {
  if (!FrameStorageValid(frame) ||
      !frame->snapshot_count || frame->snapshot_count > kArTextCellRecordCapacity ||
      !spans || frame->bidi.count > kArTextMaximumBidiSpans ||
      spans->count > kArTextMaximumBidiSpans - frame->bidi.count) return false;
  ArLocalizationTextSnapshot *snapshot = &frame->snapshots[frame->snapshot_count - 1];
  if (!SnapshotTextValid(frame, snapshot) || snapshot->bidi_span_count ||
      (spans->count && spans->spans[spans->count - 1].end > snapshot->utf8_bytes) ||
      !ArTextBidiSpans_Valid(spans->spans, spans->count,
          frame->text + snapshot->utf8_offset, snapshot->utf8_bytes, 0)) return false;
  snapshot->bidi_span_offset = frame->bidi.count;
  snapshot->bidi_span_count = spans->count;
  memcpy(frame->bidi.spans + frame->bidi.count, spans->spans,
         spans->count * sizeof(spans->spans[0]));
  frame->bidi.count += spans->count;
  return true;
}

bool ArLocalizationFrame_SetTextOrigin(ArLocalizationFrame *frame,
                                       const ArTextSourceOrigin *origin) {
  if (!FrameStorageValid(frame) || !frame->snapshot_count ||
      !ArTextSourceOrigin_IsValid(origin))
    return false;
  frame->snapshots[frame->snapshot_count - 1].origin = *origin;
  return true;
}

bool ArLocalizationFrame_SetTextAppearance(
    ArLocalizationFrame *frame, const ArTextRunAppearance *appearance,
    const ArTextAppearanceSpan *spans, size_t count) {
  if (!FrameStorageValid(frame) || !frame->snapshot_count ||
      !ArTextAppearance_IsValid(appearance) ||
      count >
          kArLocalizationFrameAppearanceCapacity - frame->appearance_span_count)
    return false;
  ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1];
  if (!SnapshotTextValid(frame, snapshot) || snapshot->has_appearance ||
      !ArTextAppearanceSpans_IsValid(spans, count,
                                     frame->text + snapshot->utf8_offset,
                                     snapshot->utf8_bytes, 0) ||
      (count && spans[count - 1].end > snapshot->utf8_bytes))
    return false;
  snapshot->appearance = *appearance;
  snapshot->has_appearance = true;
  snapshot->appearance_span_offset = frame->appearance_span_count;
  snapshot->appearance_span_count = (uint16_t)count;
  if (count)
    memcpy(frame->appearance_spans + frame->appearance_span_count, spans,
           count * sizeof(*spans));
  frame->appearance_span_count += (uint16_t)count;
  return true;
}

bool ArLocalizationFrame_AddText(ArLocalizationFrame *frame,
                                 uint32_t surface_id,
                                 ArTextCellDestination destination,
                                 ArTextCellRegion region,
                                 const char *utf8, size_t utf8_bytes,
                                 uint32_t revealed_cluster_count,
                                 uint32_t cluster_count,
                                 uint64_t source_revision,
                                 ArTextDirection direction,
                                 uint8_t native_font_pixels,
                                 const ArTextCellRegion *native_preserves,
                                 uint8_t native_preserve_count) {
  return ArLocalizationFrame_AddTextWithObjects(
      frame, surface_id, destination, region, utf8, utf8_bytes,
      revealed_cluster_count, cluster_count, source_revision, direction,
      native_font_pixels, native_preserves, native_preserve_count, NULL, 0);
}

bool ArLocalizationFrame_AddTextWithObjects(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count) {
  return ArLocalizationFrame_AddTextWithObjectsAndLayout(
      frame, surface_id, destination, region, utf8, utf8_bytes,
      revealed_cluster_count, cluster_count, source_revision, direction,
      native_font_pixels, kArLocalizationTextLayout_Flow,
      native_preserves, native_preserve_count,
      inline_objects, inline_object_count);
}

bool ArLocalizationFrame_AddScreenText(
    ArLocalizationFrame *frame, uint32_t surface_id,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, ArLocalizationTextLayoutKind layout) {
  if (!FrameStorageValid(frame) || !frame->font_revision || !surface_id ||
      !width || !height || !utf8 || !source_revision ||
      (!utf8_bytes && cluster_count) || (utf8_bytes && !cluster_count) ||
      revealed_cluster_count > cluster_count || !native_font_pixels ||
      direction < kArTextDirection_Auto ||
      direction > kArTextDirection_RightToLeft ||
      (layout != kArLocalizationTextLayout_SingleLineLabel &&
       layout != kArLocalizationTextLayout_CenteredLabel &&
       layout != kArLocalizationTextLayout_RightAlignedLabel &&
       layout != kArLocalizationTextLayout_LeftAlignedLabel &&
       layout != kArLocalizationTextLayout_DialogueWindow) ||
      frame->screen_text_count >= kArLocalizationFrameScreenTextCapacity ||
      frame->snapshot_count >= kArTextCellRecordCapacity ||
      utf8_bytes >= kArLocalizationFrameTextCapacity - frame->text_bytes ||
      ArTextCellRecordSet_Find(&frame->cells, surface_id) ||
      ArLocalizationFrame_FindScreenText(frame, surface_id))
    return false;

  const uint8_t slot = frame->snapshot_count;
  const uint32_t offset = frame->text_bytes;
  memcpy(frame->text + offset, utf8, utf8_bytes);
  frame->text[offset + utf8_bytes] = 0;
  frame->text_bytes += (uint32_t)utf8_bytes + 1u;
  frame->snapshots[slot] = (ArLocalizationTextSnapshot){
      .style_id = kArTextStyle_RetailBlueWhiteBands,
      .surface_id = surface_id,
      .utf8_offset = offset,
      .utf8_bytes = (uint32_t)utf8_bytes,
      .revealed_cluster_count = revealed_cluster_count,
      .cluster_count = cluster_count,
      .source_revision = source_revision,
      .language.direction = direction,
      .layout = layout,
      .native_font_pixels = native_font_pixels,
  };
  memcpy(frame->snapshots[slot].language.locale, frame->locale,
         sizeof(frame->locale));
  frame->screen_texts[frame->screen_text_count++] =
      (ArLocalizationScreenTextRecord){
          .surface_id = surface_id,
          .x = x,
          .y = y,
          .width = width,
          .height = height,
          .snapshot_slot = slot,
      };
  frame->snapshot_count++;
  return true;
}

/* Grids are compared and hashed as bytes, so the description must pack without
 * padding: a byte no caller wrote would make two identical grids differ. */
_Static_assert(sizeof(ArLocalizationTextCellRule) == 7,
               "cell rule gained padding");
_Static_assert(sizeof(ArLocalizationTextRowRule) ==
                   6 + 7 * kArLocalizationGridMaximumCells,
               "row rule gained padding");
_Static_assert(sizeof(ArLocalizationTextGrid) ==
                   6 + (6 + 7 * kArLocalizationGridMaximumCells) *
                           kArLocalizationGridMaximumRules,
               "grid gained padding");

/* Rejects geometry the renderer could not act on, so a malformed grid fails
 * where it is published rather than while a frame is being prepared. */
static bool GridValid(const ArLocalizationTextGrid *grid,
                      ArTextCellRegion region) {
  if (!grid || !grid->rule_count ||
      grid->rule_count > kArLocalizationGridMaximumRules || !grid->row_height ||
      grid->shared_column_count > kArLocalizationGridMaximumCells)
    return false;
  for (uint8_t index = 0; index < grid->rule_count; ++index) {
    const ArLocalizationTextRowRule *rule = &grid->rules[index];
    if (rule->first_line > rule->last_line ||
        rule->field_count > kArLocalizationGridMaximumCells ||
        rule->cell_count > kArLocalizationGridMaximumCells)
      return false;
    if (rule->native_reserved) {
      if (rule->cell_count || rule->shared_columns) return false;
      continue;
    }
    if (!rule->cell_count ||
        (rule->field_count && rule->cell_count != rule->field_count))
      return false;
    if (rule->shared_columns &&
        (!grid->shared_column_count ||
         rule->cell_count != grid->shared_column_count))
      return false;
    for (uint8_t cell = 0; cell < rule->cell_count; ++cell) {
      const ArLocalizationTextCellRule *entry = &rule->cells[cell];
      if (entry->start >= entry->end || entry->end > region.columns ||
          entry->italic > 1 ||
          entry->alignment > (uint8_t)kArTextHorizontalAlignment_Trailing)
        return false;
    }
  }
  return true;
}

static bool SnapshotTextValid(const ArLocalizationFrame *frame,
                              const ArLocalizationTextSnapshot *snapshot) {
  return snapshot->utf8_offset < frame->text_bytes &&
      snapshot->utf8_bytes < frame->text_bytes - snapshot->utf8_offset &&
      frame->text[snapshot->utf8_offset + snapshot->utf8_bytes] == 0;
}

/* A live line is a whole hard line: bounded by the text edges or line feeds,
 * with none inside. No live line at all is valid too. */
static bool LiveLineValid(const char *utf8, size_t utf8_bytes,
                          size_t offset, size_t bytes) {
  if (!bytes) return !offset;
  return offset <= utf8_bytes && bytes <= utf8_bytes - offset &&
      bytes <= UINT16_MAX && offset <= UINT16_MAX &&
      (!offset || utf8[offset - 1u] == '\n') &&
      (offset + bytes == utf8_bytes || utf8[offset + bytes] == '\n') &&
      !memchr(utf8 + offset, '\n', bytes);
}

bool ArLocalizationTextField_IsValid(const ArLocalizationTextField *field,
                                     const char *utf8, size_t utf8_bytes) {
  return field && utf8 &&
      field->cells <= kArLocalizationFrameLiveLineMaximumCells &&
      (field->utf8_bytes || !field->cells) &&
      LiveLineValid(utf8, utf8_bytes, field->utf8_offset, field->utf8_bytes);
}

bool ArLocalizationFrame_IsValid(const ArLocalizationFrame *frame) {
  if (!FrameStorageValid(frame)) return false;
  if (!frame->snapshot_count) {
    return !frame->cells.count && !frame->screen_text_count &&
           !frame->bidi.count && !frame->appearance_span_count &&
           !frame->grid_count && !frame->indicator_count &&
           !frame->inline_object_count && !frame->text_bytes;
  }
  if (!frame->primary_font || !frame->font_revision ||
      !frame->locale[0] || !memchr(frame->locale, 0, sizeof(frame->locale)) ||
      !frame->font_stack_id[0] ||
      !memchr(frame->font_stack_id, 0, sizeof(frame->font_stack_id)) ||
      !ArEnhancedTextSettings_IsValid(&frame->settings))
    return false;
  for (uint8_t i = 0; i < frame->fallback_font_count; ++i)
    if (!frame->fallback_fonts[i]) return false;
  if (!ArTextFontRoles_Valid(frame->font_roles, frame->font_role_count))
    return false;

  ArTextCellRecordSet rebuilt;
  ArTextCellRecordSet_Reset(&rebuilt);
  for (uint8_t i = 0; i < frame->cells.count; ++i) {
    const ArTextCellRecord *record = &frame->cells.records[i];
    if (!record->surface_id || record->snapshot_slot < 0 ||
        (uint8_t)record->snapshot_slot >= frame->snapshot_count ||
        frame->snapshots[record->snapshot_slot].surface_id != record->surface_id ||
        !ArTextCellRecordSet_Claim(&rebuilt, record->surface_id,
            record->destination, record->region, record->snapshot_slot) ||
        rebuilt.count != i + 1u)
      return false;
  }
  for (uint8_t i = 0; i < frame->screen_text_count; ++i) {
    const ArLocalizationScreenTextRecord *record = &frame->screen_texts[i];
    if (!record->surface_id || !record->width || !record->height ||
        record->snapshot_slot >= frame->snapshot_count ||
        frame->snapshots[record->snapshot_slot].surface_id !=
            record->surface_id)
      return false;
    for (uint8_t previous = 0; previous < i; ++previous)
      if (frame->screen_texts[previous].surface_id == record->surface_id)
        return false;
    if (ArTextCellRecordSet_Find(&frame->cells, record->surface_id))
      return false;
  }

  for (uint8_t i = 0; i < frame->snapshot_count; ++i) {
    const ArLocalizationTextSnapshot *snapshot = &frame->snapshots[i];
    const ArTextCellRecord *owner = NULL;
    for (uint8_t cell = 0; cell < frame->cells.count; ++cell)
      if (frame->cells.records[cell].snapshot_slot == (int8_t)i) {
        owner = &frame->cells.records[cell];
        break;
      }
    if (!SnapshotTextValid(frame, snapshot) ||
        !ArTextSourceOrigin_IsValid(&snapshot->origin) ||
        !snapshot->source_revision || !snapshot->native_font_pixels ||
        snapshot->revealed_cluster_count > snapshot->cluster_count ||
        (!snapshot->utf8_bytes && snapshot->cluster_count) ||
        (snapshot->utf8_bytes && !snapshot->cluster_count) ||
        snapshot->revealed_utf8_bytes > snapshot->utf8_bytes ||
        (snapshot->revealed_utf8_bytes < snapshot->utf8_bytes &&
         ((uint8_t)frame
              ->text[snapshot->utf8_offset + snapshot->revealed_utf8_bytes] &
          0xc0u) == 0x80u) ||
        !ArLocalizationTextLanguage_IsValid(&snapshot->language) ||
        snapshot->layout < kArLocalizationTextLayout_Flow ||
        snapshot->layout > kArLocalizationTextLayout_CenteredBlock ||
        snapshot->native_preserve_count >
            kArLocalizationFrameNativePreserveCapacity ||
        snapshot->inline_object_offset > frame->inline_object_count ||
        snapshot->inline_object_count >
            frame->inline_object_count - snapshot->inline_object_offset ||
        snapshot->bidi_span_offset > frame->bidi.count ||
        snapshot->bidi_span_count >
            frame->bidi.count - snapshot->bidi_span_offset ||
        snapshot->key_separator_bytes >
            kArLocalizationFrameKeySeparatorCapacity ||
        !LiveLineValid(frame->text + snapshot->utf8_offset,
                       snapshot->utf8_bytes, snapshot->live_line_utf8_offset,
                       snapshot->live_line_utf8_bytes) ||
        snapshot->live_line_cells > kArLocalizationFrameLiveLineMaximumCells ||
        (snapshot->live_line_cells && !snapshot->live_line_utf8_bytes))
      return false;
    if (snapshot->appearance_span_offset > frame->appearance_span_count ||
        snapshot->appearance_span_count >
            frame->appearance_span_count - snapshot->appearance_span_offset)
      return false;
    if (snapshot->has_appearance) {
      const ArTextAppearanceSpan *spans =
          frame->appearance_spans + snapshot->appearance_span_offset;
      const size_t count = snapshot->appearance_span_count;
      if (!ArTextAppearance_IsValid(&snapshot->appearance) ||
          !ArTextAppearanceSpans_IsValid(spans, count,
                                         frame->text + snapshot->utf8_offset,
                                         snapshot->utf8_bytes, 0) ||
          (count && spans[count - 1].end > snapshot->utf8_bytes))
        return false;
    } else if (snapshot->appearance_span_count ||
               snapshot->appearance_span_offset) {
      return false;
    }
    if ((snapshot->layout == kArLocalizationTextLayout_Grid) !=
        (snapshot->grid_index != 0) || snapshot->grid_index > frame->grid_count)
      return false;
    const ArTextCellRegion grid_region = owner
        ? owner->region : (ArTextCellRegion){0, 0, 64, 64};
    if (snapshot->grid_index &&
        !GridValid(&frame->grids[snapshot->grid_index - 1u], grid_region))
      return false;
    for (uint8_t preserve = 0;
         preserve < snapshot->native_preserve_count; ++preserve)
      if (owner &&
          !RegionContains(owner->region, snapshot->native_preserves[preserve]))
        return false;
    uint32_t previous_end = 0;
    for (uint8_t object = 0; object < snapshot->inline_object_count; ++object) {
      const ArLocalizationInlineObjectSnapshot *entry =
          &frame->inline_objects[snapshot->inline_object_offset + object];
      if (entry->kind <= kArLocalizationInlineObject_None ||
          entry->kind > kArLocalizationInlineObject_NameFieldUnderline ||
          entry->end_utf8_byte < previous_end ||
          entry->end_utf8_byte > snapshot->utf8_bytes)
        return false;
      previous_end = entry->end_utf8_byte;
    }
    if (!ArTextBidiSpans_Valid(
            frame->bidi.spans + snapshot->bidi_span_offset,
            snapshot->bidi_span_count,
            frame->text + snapshot->utf8_offset, snapshot->utf8_bytes, 0))
      return false;
  }
  for (uint8_t i = 0; i < frame->indicator_count; ++i) {
    const ArLocalizationIndicatorSnapshot *indicator = &frame->indicators[i];
    const ArTextCellRecord *owner =
        ArTextCellRecordSet_Find(&frame->cells, indicator->surface_id);
    if (indicator->kind <= kArLocalizationIndicator_None ||
        indicator->kind > kArLocalizationIndicator_DialogueContinue ||
        !owner || !RegionContains(owner->region, indicator->region))
      return false;
  }
  return !frame->dialogue_ticket ||
      (frame->dialogue_surface_id &&
       ArTextCellRecordSet_Find(&frame->cells, frame->dialogue_surface_id));
}

const ArLocalizationTextRowRule *ArLocalizationGrid_FindRow(
    const ArLocalizationTextGrid *grid, unsigned line, unsigned field_count) {
  if (!grid || line > kArLocalizationGridAnyLine) return NULL;
  for (uint8_t index = 0; index < grid->rule_count; ++index) {
    const ArLocalizationTextRowRule *rule = &grid->rules[index];
    if (line < rule->first_line || line > rule->last_line) continue;
    if (rule->field_count != kArLocalizationGridAnyFieldCount &&
        rule->field_count != field_count)
      continue;
    return rule;
  }
  return NULL;
}

const ArLocalizationTextGrid *ArLocalizationFrame_GetGrid(
    const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot) {
  if (!FrameStorageValid(frame) || !snapshot || !snapshot->grid_index ||
      snapshot->grid_index > frame->grid_count)
    return NULL;
  return &frame->grids[snapshot->grid_index - 1u];
}

const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface_id) {
  if (!FrameStorageValid(frame) || !surface_id) return NULL;
  for (uint8_t index = 0; index < frame->screen_text_count; ++index)
    if (frame->screen_texts[index].surface_id == surface_id)
      return &frame->screen_texts[index];
  return NULL;
}

/* Interns one grid, so a report drawn by several surfaces is published once.
 * Returns a one-based index, or zero when the table is full. */
static uint8_t InternGrid(ArLocalizationFrame *frame,
                          const ArLocalizationTextGrid *grid) {
  for (uint8_t index = 0; index < frame->grid_count; ++index) {
    if (!memcmp(&frame->grids[index], grid, sizeof(*grid)))
      return index + 1u;
  }
  if (frame->grid_count >= kArLocalizationFrameGridCapacity) return 0;
  frame->grids[frame->grid_count] = *grid;
  return ++frame->grid_count;
}

static bool AddTextInternal(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, ArLocalizationTextLayoutKind layout,
    const ArLocalizationTextGrid *grid,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count) {
  if (!FrameStorageValid(frame) ||
      !frame->font_revision || !utf8 || !source_revision ||
      (!utf8_bytes && (cluster_count || inline_object_count)) ||
      (utf8_bytes && !cluster_count) ||
      !native_font_pixels || revealed_cluster_count > cluster_count ||
      native_preserve_count > kArLocalizationFrameNativePreserveCapacity ||
      (native_preserve_count && !native_preserves) ||
      (inline_object_count && !inline_objects) ||
      inline_object_count >
          kArLocalizationFrameInlineObjectCapacity -
              frame->inline_object_count ||
      direction < kArTextDirection_Auto ||
      direction > kArTextDirection_RightToLeft ||
      layout < kArLocalizationTextLayout_Flow ||
      layout > kArLocalizationTextLayout_CenteredBlock ||
      (layout == kArLocalizationTextLayout_Grid) != (grid != NULL) ||
      (grid && !GridValid(grid, region)) ||
      frame->snapshot_count >= kArTextCellRecordCapacity ||
      utf8_bytes >= kArLocalizationFrameTextCapacity - frame->text_bytes)
    return false;
  for (uint8_t index = 0; index < native_preserve_count; ++index) {
    if (!RegionContains(region, native_preserves[index])) return false;
  }
  uint32_t previous_end = 0;
  for (uint8_t index = 0; index < inline_object_count; ++index) {
    const ArLocalizationInlineObjectSnapshot *object =
        &inline_objects[index];
    if (object->kind <= kArLocalizationInlineObject_None ||
        object->kind > kArLocalizationInlineObject_NameFieldUnderline ||
        object->end_utf8_byte < previous_end ||
        object->end_utf8_byte > utf8_bytes)
      return false;
    previous_end = object->end_utf8_byte;
  }
  const uint8_t slot = frame->snapshot_count;
  ArTextCellRecordSet cells = frame->cells;
  if (!ArTextCellRecordSet_Claim(&cells, surface_id, destination, region,
                                 (int8_t)slot))
    return false;
  /* Intern before publishing anything: a full grid table must not leave a
   * half-added snapshot behind. */
  const uint8_t grid_index = grid ? InternGrid(frame, grid) : 0u;
  if (grid && !grid_index) return false;
  const uint32_t offset = frame->text_bytes;
  memcpy(frame->text + offset, utf8, utf8_bytes);
  frame->text[offset + utf8_bytes] = 0;
  frame->text_bytes += (uint32_t)utf8_bytes + 1u;
  frame->snapshots[slot] = (ArLocalizationTextSnapshot){
      .style_id = kArTextStyle_RetailBlueWhiteBands,
      .surface_id = surface_id,
      .utf8_offset = offset,
      .utf8_bytes = (uint32_t)utf8_bytes,
      .revealed_cluster_count = revealed_cluster_count,
      .cluster_count = cluster_count,
      .source_revision = source_revision,
      .language.direction = direction,
      .layout = layout,
      .grid_index = grid_index,
      .native_font_pixels = native_font_pixels,
      .native_preserve_count = native_preserve_count,
      .inline_object_offset = frame->inline_object_count,
      .inline_object_count = inline_object_count,
  };
  memcpy(frame->snapshots[slot].language.locale, frame->locale, sizeof(frame->locale));
  if (native_preserve_count)
    memcpy(frame->snapshots[slot].native_preserves, native_preserves,
           (size_t)native_preserve_count * sizeof(native_preserves[0]));
  if (inline_object_count) {
    memcpy(&frame->inline_objects[frame->inline_object_count],
           inline_objects,
           (size_t)inline_object_count * sizeof(inline_objects[0]));
    frame->inline_object_count += inline_object_count;
  }
  frame->snapshot_count++;
  frame->cells = cells;
  return true;
}

bool ArLocalizationFrame_AddTextWithObjectsAndLayout(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, ArLocalizationTextLayoutKind layout,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count) {
  return AddTextInternal(frame, surface_id, destination, region, utf8,
                         utf8_bytes, revealed_cluster_count, cluster_count,
                         source_revision, direction, native_font_pixels,
                         layout, NULL, native_preserves, native_preserve_count,
                         inline_objects, inline_object_count);
}

bool ArLocalizationFrame_AddTextWithGrid(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes,
    uint32_t revealed_cluster_count, uint32_t cluster_count,
    uint64_t source_revision, ArTextDirection direction,
    uint8_t native_font_pixels, const ArLocalizationTextGrid *grid,
    const uint8_t *structural_boundaries,
    const ArTextCellRegion *native_preserves,
    uint8_t native_preserve_count,
    const ArLocalizationInlineObjectSnapshot *inline_objects,
    uint8_t inline_object_count) {
  if (!AddTextInternal(frame, surface_id, destination, region, utf8,
                         utf8_bytes, revealed_cluster_count, cluster_count,
                         source_revision, direction, native_font_pixels,
                         kArLocalizationTextLayout_Grid, grid,
                         native_preserves, native_preserve_count,
                         inline_objects, inline_object_count))
    return false;
  const size_t offset = frame->snapshots[frame->snapshot_count - 1u].utf8_offset;
  for (size_t i = 0; i < utf8_bytes; ++i)
    ArTextBoundary_Set(frame->structural_boundaries, offset + i,
        (utf8[i] == '|' || utf8[i] == '\n') &&
        (!structural_boundaries || ArTextBoundary_Get(structural_boundaries, i)));
  return true;
}

bool ArLocalizationFrame_SetKeySeparator(ArLocalizationFrame *frame,
                                         const char *utf8, size_t utf8_bytes) {
  if (!FrameStorageValid(frame) ||
      !frame->snapshot_count || !utf8 || !utf8_bytes ||
      utf8_bytes > kArLocalizationFrameKeySeparatorCapacity)
    return false;
  ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1u];
  memcpy(snapshot->key_separator, utf8, utf8_bytes);
  snapshot->key_separator_bytes = (uint8_t)utf8_bytes;
  return true;
}

bool ArLocalizationFrame_SetKeyGrid(ArLocalizationFrame *frame,
                                    uint8_t columns, uint8_t trailing_lines,
                                    uint8_t cell_columns) {
  if (!FrameStorageValid(frame) ||
      !frame->snapshot_count || !columns || !trailing_lines || !cell_columns)
    return false;
  ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1u];
  snapshot->key_columns = columns;
  snapshot->key_trailing_lines = trailing_lines;
  snapshot->key_cell_columns = cell_columns;
  return true;
}

bool ArLocalizationFrame_SetLiveLine(ArLocalizationFrame *frame,
                                     size_t utf8_offset, size_t utf8_bytes,
                                     uint8_t cells) {
  if (!FrameStorageValid(frame) || !frame->snapshot_count || !utf8_bytes ||
      cells > kArLocalizationFrameLiveLineMaximumCells)
    return false;
  ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1u];
  if (!SnapshotTextValid(frame, snapshot) ||
      !LiveLineValid(frame->text + snapshot->utf8_offset,
                     snapshot->utf8_bytes, utf8_offset, utf8_bytes))
    return false;
  snapshot->live_line_utf8_offset = (uint16_t)utf8_offset;
  snapshot->live_line_utf8_bytes = (uint16_t)utf8_bytes;
  snapshot->live_line_cells = cells;
  return true;
}

bool ArLocalizationFrame_AddIndicator(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArLocalizationIndicatorKind kind, ArTextCellRegion region) {
  if (!FrameStorageValid(frame) ||
      kind <= kArLocalizationIndicator_None ||
      kind > kArLocalizationIndicator_DialogueContinue ||
      frame->indicator_count >= kArLocalizationFrameIndicatorCapacity)
    return false;
  const ArTextCellRecord *owner = NULL;
  for (uint8_t index = 0; index < frame->cells.count; ++index) {
    if (frame->cells.records[index].surface_id == surface_id) {
      owner = &frame->cells.records[index];
      break;
    }
  }
  if (!owner || !RegionContains(owner->region, region)) return false;
  frame->indicators[frame->indicator_count++] =
      (ArLocalizationIndicatorSnapshot){surface_id, kind, region};
  return true;
}

bool ArLocalizationFrame_AddDialogueWindow(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes, uint32_t revealed_utf8_bytes,
    uint32_t cluster_count, uint64_t source_revision,
    ArTextDirection direction, uint8_t native_font_pixels) {
  return ArLocalizationFrame_AddStructuredDialogueWindow(
      frame, surface_id, destination, region, utf8, utf8_bytes,
      revealed_utf8_bytes, cluster_count, source_revision, direction,
      native_font_pixels, NULL);
}

bool ArLocalizationFrame_AddStructuredDialogueWindow(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArTextCellDestination destination, ArTextCellRegion region,
    const char *utf8, size_t utf8_bytes, uint32_t revealed_utf8_bytes,
    uint32_t cluster_count, uint64_t source_revision,
    ArTextDirection direction, uint8_t native_font_pixels,
    const uint8_t *structural_boundaries) {
  if (!utf8 || revealed_utf8_bytes > utf8_bytes ||
      (revealed_utf8_bytes < utf8_bytes &&
       ((uint8_t)utf8[revealed_utf8_bytes] & 0xc0u) == 0x80u))
    return false;
  if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
          frame, surface_id, destination, region, utf8, utf8_bytes,
          cluster_count, cluster_count, source_revision, direction,
          native_font_pixels, kArLocalizationTextLayout_DialogueWindow,
          NULL, 0, NULL, 0))
    return false;
  frame->snapshots[frame->snapshot_count - 1u].revealed_utf8_bytes =
      revealed_utf8_bytes;
  if (structural_boundaries) {
    const size_t offset = frame->snapshots[frame->snapshot_count - 1u].utf8_offset;
    for (size_t i = 0; i < utf8_bytes; ++i)
      ArTextBoundary_Set(frame->structural_boundaries, offset + i,
                         ArTextBoundary_Get(structural_boundaries, i));
  }
  return true;
}

const char *ArLocalizationFrame_GetText(
    const ArLocalizationFrame *frame, uint8_t snapshot_index,
    size_t *utf8_bytes) {
  if (utf8_bytes) *utf8_bytes = 0;
  if (!FrameStorageValid(frame) ||
      snapshot_index >= frame->snapshot_count)
    return NULL;
  const ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[snapshot_index];
  if (!SnapshotTextValid(frame, snapshot))
    return NULL;
  if (utf8_bytes) *utf8_bytes = snapshot->utf8_bytes;
  return frame->text + snapshot->utf8_offset;
}
