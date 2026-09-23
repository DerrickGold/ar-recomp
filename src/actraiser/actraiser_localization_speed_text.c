#include "actraiser_localization_speed_text.h"
#include "localization/unicode_grapheme.h"
#include <string.h>

bool ActRaiserLocalizationSpeedText_Project(ActRaiserResolvedText *text, unsigned maximum) {
  if (!text || (maximum != 7 && maximum != 9) ||
      text->utf8_bytes >= sizeof(text->utf8) || text->utf8[text->utf8_bytes] ||
      text->live_field.utf8_bytes || text->live_field.cells ||
      text->inline_object_count > kArLocalizationFrameInlineObjectCapacity ||
      text->styles.span_count > kActRaiserTextStyleSpanCapacity ||
      !ArTextBidiSpans_FitSource(&text->bidi, text->utf8, text->utf8_bytes)) return false;
  if (!text->utf8_bytes) return !text->inline_object_count && !text->cluster_count;
  size_t end = 0, eighth_separator = 0;
  unsigned fields = 1;
  for (; end < text->utf8_bytes; ++end) {
    if (!ArTextBoundary_Get(text->structural_boundaries, end)) continue;
    if (text->utf8[end] == '\n') break;
    if (text->utf8[end] == '|') {
      if (++fields == 9) eighth_separator = end;
    }
  }
  if (fields != 8 && fields != 10) return false;
  if (fields == maximum + 1) return true;
  const size_t offset = maximum == 7 ? eighth_separator : end;
  const size_t removed = end - offset;
  const char *insertion = maximum == 9 ? " | 8 | 9" : "";
  const size_t inserted = strlen(insertion);
  const size_t new_size = text->utf8_bytes - removed + inserted;
  if (new_size >= sizeof(text->utf8)) return false;
  for (unsigned i = 0; i < text->inline_object_count; ++i) {
    const uint32_t object_end = text->inline_objects[i].end_utf8_byte;
    if (object_end > offset && object_end <= end) return false;
  }
  /* Keep byte-indexed metadata in lockstep. Boundaries inside inserted ASCII
   * separators are structural; translated label/artwork boundaries shift. */
  uint8_t boundaries[sizeof(text->structural_boundaries)] = {0};
  for (size_t i = 0; i < text->utf8_bytes; ++i) {
    if (i >= offset && i < end) continue;
    if (ArTextBoundary_Get(text->structural_boundaries, i))
      ArTextBoundary_Set(boundaries, i < offset ? i : i - removed + inserted, true);
  }
  for (size_t i = 0; i < inserted; ++i)
    if (insertion[i] == '|') ArTextBoundary_Set(boundaries, offset + i, true);
  memmove(text->utf8 + offset + inserted, text->utf8 + end,
          text->utf8_bytes - end + 1);
  memcpy(text->utf8 + offset, insertion, inserted);
  memcpy(text->structural_boundaries, boundaries, sizeof(boundaries));
  ArTextBidiSpans_Edit(&text->bidi, (uint32_t)offset, (uint32_t)removed, (uint32_t)inserted);
  ActRaiserTextStyle_Edit(&text->styles, (uint32_t)offset, (uint32_t)removed, (uint32_t)inserted);
  for (unsigned i = 0; i < text->inline_object_count; ++i)
    if (text->inline_objects[i].end_utf8_byte > offset)
      text->inline_objects[i].end_utf8_byte =
          (uint32_t)(text->inline_objects[i].end_utf8_byte - removed + inserted);
  text->utf8_bytes = new_size;
  text->cluster_count = 0;
  for (size_t offset = 0, next; offset < new_size; offset = next) {
    if (!ArUnicodeGrapheme_Next(text->utf8, new_size, offset, NULL, &next) || next <= offset)
      return false;
    ++text->cluster_count;
  }
  return ArTextBidiSpans_FitSource(&text->bidi, text->utf8, text->utf8_bytes);
}
