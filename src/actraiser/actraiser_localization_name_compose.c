#include "actraiser/actraiser_localization_name_compose.h"

#include <stdio.h>
#include <string.h>

#include "actraiser/actraiser_localization_name_entry.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "localization/unicode_grapheme.h"

static bool CollectNameEntryLines(const char *utf8, size_t utf8_bytes,
                                  size_t *starts, size_t *ends,
                                  size_t capacity, size_t *line_count) {
  if (!utf8 || !utf8_bytes || !starts || !ends || !capacity || !line_count)
    return false;
  size_t count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= utf8_bytes; ++index) {
    if (index != utf8_bytes && utf8[index] != '\n') continue;
    if (count >= capacity) return false;
    starts[count] = start;
    ends[count] = index;
    ++count;
    start = index + 1u;
  }
  *line_count = count;
  return true;
}

/* The logical row immediately before the five keyboard rows is an entry-field
 * underline slot, not translatable wording. Remove the extraction-era dash
 * glyphs while retaining the hard line, then attach semantic underlines to
 * the eight shaped name graphemes independently of keyboard geometry. */
bool ActRaiserLocalizationNameCompose_ClearUnderlineRow(
    ActRaiserResolvedText *text) {
  if (!text || !text->utf8_bytes)
    return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(text->utf8, text->utf8_bytes, starts, ends,
                             sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows + 1u)
    return false;
  const size_t line =
      line_count - kActRaiserLocalizationNameEntryRows - 1u;
  const size_t remove_start = starts[line];
  const size_t remove_end = ends[line];
  if (remove_end < remove_start) return false;
  const size_t removed = remove_end - remove_start;
  for (uint8_t index = 0; index < text->inline_object_count; ++index) {
    if (text->inline_objects[index].end_utf8_byte > remove_start &&
        text->inline_objects[index].end_utf8_byte <= remove_end)
      return false;
    if (text->inline_objects[index].end_utf8_byte > remove_end)
      text->inline_objects[index].end_utf8_byte -= (uint32_t)removed;
  }
  memmove(text->utf8 + remove_start, text->utf8 + remove_end,
          text->utf8_bytes - remove_end + 1u);
  text->utf8_bytes -= removed;
  ArTextBidiSpans_Edit(&text->bidi, (uint32_t)remove_start, (uint32_t)removed,
                       0);
  ActRaiserTextStyle_Edit(&text->styles, (uint32_t)remove_start,
                          (uint32_t)removed, 0);
  return true;
}

static bool InsertNameEntryText(ActRaiserResolvedText *text, size_t offset,
                                const char *insertion, size_t insertion_bytes) {
  if (!text || !insertion || !insertion_bytes || offset > text->utf8_bytes ||
      insertion_bytes >= sizeof(text->utf8) - text->utf8_bytes)
    return false;
  memmove(text->utf8 + offset + insertion_bytes, text->utf8 + offset,
          text->utf8_bytes - offset + 1u);
  memcpy(text->utf8 + offset, insertion, insertion_bytes);
  text->utf8_bytes += insertion_bytes;
  ArTextBidiSpans_Edit(&text->bidi, (uint32_t)offset, 0,
                       (uint32_t)insertion_bytes);
  ActRaiserTextStyle_Edit(&text->styles, (uint32_t)offset, 0,
                          (uint32_t)insertion_bytes);
  for (uint8_t index = 0; index < text->inline_object_count; ++index) {
    /* An endpoint equal to the insertion point belongs to the preceding
     * cluster. Only objects attached to following text move. */
    if (text->inline_objects[index].end_utf8_byte > offset)
      text->inline_objects[index].end_utf8_byte += (uint32_t)insertion_bytes;
  }
  return true;
}

/* Enhanced glyphs are variable-width, but the retail name grid reserves a
 * blank tile before every key for its arrow. Retain that visual grammar by
 * expanding each normalized inter-key separator to a four-space gutter. The
 * glyphs remain shaped/VWF; only the navigation affordance has fixed room. */
bool ActRaiserLocalizationNameCompose_ExpandKeyGutters(
    ActRaiserResolvedText *text) {
  if (!text)
    return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(text->utf8, text->utf8_bytes, starts, ends,
                             sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  const size_t first = line_count - kActRaiserLocalizationNameEntryRows;
  for (size_t line = line_count; line-- > first;) {
    for (size_t offset = ends[line]; offset-- > starts[line];) {
      if (text->utf8[offset] != ' ' || offset == starts[line] ||
          offset + 1u >= ends[line] || text->utf8[offset - 1u] == ' ' ||
          text->utf8[offset + 1u] == ' ')
        continue;
      if (!InsertNameEntryText(text, offset, "   ", 3u))
        return false;
    }
  }
  return true;
}

bool ActRaiserLocalizationNameCompose_InsertPageIndicator(
    ActRaiserResolvedText *text, uint32_t page_index, uint32_t page_count) {
  if (!text)
    return false;
  if (page_count <= 1u) return true;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(text->utf8, text->utf8_bytes, starts, ends,
                             sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  char indicator[64];
  const int written = snprintf(
      indicator, sizeof(indicator), "< %u/%u >\n",
      (unsigned)(page_index + 1u), (unsigned)page_count);
  if (written <= 0 || (size_t)written >= sizeof(indicator)) return false;
  return InsertNameEntryText(
      text, starts[line_count - kActRaiserLocalizationNameEntryRows], indicator,
      (size_t)written);
}

bool ActRaiserLocalizationNameCompose_PrepareField(
    ActRaiserResolvedText *text) {
  if (!text || !text->utf8_bytes)
    return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= text->utf8_bytes; ++index) {
    if (index != text->utf8_bytes && text->utf8[index] != '\n')
      continue;
    if (line_count >= sizeof(starts) / sizeof(starts[0])) return false;
    starts[line_count] = start;
    ends[line_count] = index;
    ++line_count;
    start = index + 1u;
  }
  if (line_count < kActRaiserLocalizationNameEntryRows + 2u) return false;
  const size_t name_line =
      line_count - kActRaiserLocalizationNameEntryRows - 2u;
  size_t offset = starts[name_line];
  uint8_t graphemes = 0;
  while (offset < ends[name_line]) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(text->utf8, text->utf8_bytes, offset, NULL,
                                &next) ||
        next <= offset || next > ends[name_line] || next > UINT32_MAX ||
        !ActRaiserLocalizationText_InsertInlineObject(
            text->inline_objects, kArLocalizationFrameInlineObjectCapacity,
            &text->inline_object_count,
            (ArLocalizationInlineObjectSnapshot){
                kArLocalizationInlineObject_NameFieldUnderline,
                (uint32_t)next}))
      return false;
    offset = next;
    ++graphemes;
  }
  if (graphemes != kActRaiserLocalizationNameLength) return false;
  {
    text->live_field = (ArLocalizationTextField){
        .utf8_offset = starts[name_line],
        .utf8_bytes = ends[name_line] - starts[name_line],
        .cells = graphemes};
  }
  return true;
}
