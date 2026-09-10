#include "localization/localization_frame.h"

#include <string.h>

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

void ArLocalizationFrame_Reset(ArLocalizationFrame *frame) {
  if (!frame) return;
  memset(frame, 0, sizeof(*frame));
  frame->struct_size = sizeof(*frame);
  frame->abi_version = AR_LOCALIZATION_FRAME_ABI_VERSION;
  ArTextCellRecordSet_Reset(&frame->cells);
  ArEnhancedTextSettings_Defaults(&frame->settings);
}

bool ArLocalizationFrame_SetFont(ArLocalizationFrame *frame,
                                 const char *locale,
                                 const char *font_stack_id,
                                 const char *primary_font_path,
                                 uint64_t font_revision,
                                 const ArEnhancedTextSettings *settings) {
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
      !font_revision || !ArEnhancedTextSettings_IsValid(settings))
    return false;
  char locale_copy[kArLocalizationFrameLocaleCapacity] = {0};
  char stack_copy[kArLocalizationFrameFontStackCapacity] = {0};
  char path_copy[kArLocalizationFrameFontPathCapacity] = {0};
  if (!CopyString(locale_copy, sizeof(locale_copy), locale) ||
      !CopyString(stack_copy, sizeof(stack_copy), font_stack_id) ||
      !CopyString(path_copy, sizeof(path_copy), primary_font_path))
    return false;
  memcpy(frame->locale, locale_copy, sizeof(locale_copy));
  memcpy(frame->font_stack_id, stack_copy, sizeof(stack_copy));
  memcpy(frame->primary_font_path, path_copy, sizeof(path_copy));
  frame->font_revision = font_revision;
  frame->settings = *settings;
  frame->fallback_font_count = 0;
  memset(frame->fallback_font_paths, 0, sizeof(frame->fallback_font_paths));
  return true;
}

bool ArLocalizationFrame_SetFallbackFonts(
    ArLocalizationFrame *frame, const char *const *paths, size_t count) {
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
      !frame->font_revision || count > kArTextPresentationMaximumFallbackFonts ||
      (count && !paths))
    return false;
  char copied[kArTextPresentationMaximumFallbackFonts]
             [kArLocalizationFrameFontPathCapacity] = {{0}};
  for (size_t i = 0; i < count; ++i)
    if (!CopyString(copied[i], sizeof(copied[i]), paths[i])) return false;
  memcpy(frame->fallback_font_paths, copied, sizeof(copied));
  frame->fallback_font_count = (uint8_t)count;
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
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
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
      layout > kArLocalizationTextLayout_SingleLineLabel ||
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
  const uint32_t offset = frame->text_bytes;
  memcpy(frame->text + offset, utf8, utf8_bytes);
  frame->text[offset + utf8_bytes] = 0;
  frame->text_bytes += (uint32_t)utf8_bytes + 1u;
  frame->snapshots[slot] = (ArLocalizationTextSnapshot){
      .surface_id = surface_id,
      .utf8_offset = offset,
      .utf8_bytes = (uint32_t)utf8_bytes,
      .revealed_cluster_count = revealed_cluster_count,
      .cluster_count = cluster_count,
      .source_revision = source_revision,
      .direction = direction,
      .layout = layout,
      .native_font_pixels = native_font_pixels,
      .native_preserve_count = native_preserve_count,
      .inline_object_offset = frame->inline_object_count,
      .inline_object_count = inline_object_count,
  };
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

bool ArLocalizationFrame_AddIndicator(
    ArLocalizationFrame *frame, uint32_t surface_id,
    ArLocalizationIndicatorKind kind, ArTextCellRegion region) {
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
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
  return true;
}

const char *ArLocalizationFrame_GetText(
    const ArLocalizationFrame *frame, uint8_t snapshot_index,
    size_t *utf8_bytes) {
  if (utf8_bytes) *utf8_bytes = 0;
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
      snapshot_index >= frame->snapshot_count)
    return NULL;
  const ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[snapshot_index];
  if (snapshot->utf8_offset >= frame->text_bytes ||
      snapshot->utf8_bytes >= frame->text_bytes - snapshot->utf8_offset ||
      frame->text[snapshot->utf8_offset + snapshot->utf8_bytes] != 0)
    return NULL;
  if (utf8_bytes) *utf8_bytes = snapshot->utf8_bytes;
  return frame->text + snapshot->utf8_offset;
}
