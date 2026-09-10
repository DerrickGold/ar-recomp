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
  char locale_copy[kArLocalizationFrameLocaleCapacity];
  char stack_copy[kArLocalizationFrameFontStackCapacity];
  char path_copy[kArLocalizationFrameFontPathCapacity];
  if (!CopyString(locale_copy, sizeof(locale_copy), locale) ||
      !CopyString(stack_copy, sizeof(stack_copy), font_stack_id) ||
      !CopyString(path_copy, sizeof(path_copy), primary_font_path))
    return false;
  memcpy(frame->locale, locale_copy, sizeof(locale_copy));
  memcpy(frame->font_stack_id, stack_copy, sizeof(stack_copy));
  memcpy(frame->primary_font_path, path_copy, sizeof(path_copy));
  frame->font_revision = font_revision;
  frame->settings = *settings;
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
  if (!frame || frame->abi_version != AR_LOCALIZATION_FRAME_ABI_VERSION ||
      !frame->font_revision || !utf8 || !utf8_bytes || !source_revision ||
      !native_font_pixels || revealed_cluster_count > cluster_count ||
      native_preserve_count > kArLocalizationFrameNativePreserveCapacity ||
      (native_preserve_count && !native_preserves) ||
      direction < kArTextDirection_Auto ||
      direction > kArTextDirection_RightToLeft ||
      frame->snapshot_count >= kArTextCellRecordCapacity ||
      utf8_bytes >= kArLocalizationFrameTextCapacity - frame->text_bytes)
    return false;
  for (uint8_t index = 0; index < native_preserve_count; ++index) {
    if (!RegionContains(region, native_preserves[index])) return false;
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
      .native_font_pixels = native_font_pixels,
      .native_preserve_count = native_preserve_count,
  };
  if (native_preserve_count)
    memcpy(frame->snapshots[slot].native_preserves, native_preserves,
           (size_t)native_preserve_count * sizeof(native_preserves[0]));
  frame->snapshot_count++;
  frame->cells = cells;
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
