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
    char *utf8, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count, ArTextBidiSpans *bidi) {
  if (!utf8 || !utf8_bytes || !*utf8_bytes) return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(utf8, *utf8_bytes, starts, ends,
                             sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows + 1u)
    return false;
  const size_t line =
      line_count - kActRaiserLocalizationNameEntryRows - 1u;
  const size_t remove_start = starts[line];
  const size_t remove_end = ends[line];
  if (remove_end < remove_start) return false;
  const size_t removed = remove_end - remove_start;
  for (uint8_t index = 0; index < object_count; ++index) {
    if (objects[index].end_utf8_byte > remove_start &&
        objects[index].end_utf8_byte <= remove_end)
      return false;
    if (objects[index].end_utf8_byte > remove_end)
      objects[index].end_utf8_byte -= (uint32_t)removed;
  }
  memmove(utf8 + remove_start, utf8 + remove_end,
          *utf8_bytes - remove_end + 1u);
  *utf8_bytes -= removed;
  ArTextBidiSpans_Edit(bidi, (uint32_t)remove_start, (uint32_t)removed, 0);
  return true;
}

static bool InsertNameEntryText(
    char *utf8, size_t capacity, size_t *utf8_bytes, size_t offset,
    const char *insertion, size_t insertion_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count, ArTextBidiSpans *bidi) {
  if (!utf8 || !capacity || !utf8_bytes || !insertion || !insertion_bytes ||
      offset > *utf8_bytes || insertion_bytes >= capacity - *utf8_bytes)
    return false;
  memmove(utf8 + offset + insertion_bytes, utf8 + offset,
          *utf8_bytes - offset + 1u);
  memcpy(utf8 + offset, insertion, insertion_bytes);
  *utf8_bytes += insertion_bytes;
  ArTextBidiSpans_Edit(bidi, (uint32_t)offset, 0, (uint32_t)insertion_bytes);
  for (uint8_t index = 0; index < object_count; ++index) {
    /* An endpoint equal to the insertion point belongs to the preceding
     * cluster. Only objects attached to following text move. */
    if (objects[index].end_utf8_byte > offset)
      objects[index].end_utf8_byte += (uint32_t)insertion_bytes;
  }
  return true;
}


/* Enhanced glyphs are variable-width, but the retail name grid reserves a
 * blank tile before every key for its arrow. Retain that visual grammar by
 * expanding each normalized inter-key separator to a four-space gutter. The
 * glyphs remain shaped/VWF; only the navigation affordance has fixed room. */
bool ActRaiserLocalizationNameCompose_ExpandKeyGutters(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count, ArTextBidiSpans *bidi) {
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(
          utf8, *utf8_bytes, starts, ends,
          sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  const size_t first = line_count - kActRaiserLocalizationNameEntryRows;
  for (size_t line = line_count; line-- > first;) {
    for (size_t offset = ends[line]; offset-- > starts[line];) {
      if (utf8[offset] != ' ' || offset == starts[line] ||
          offset + 1u >= ends[line] || utf8[offset - 1u] == ' ' ||
          utf8[offset + 1u] == ' ')
        continue;
      if (!InsertNameEntryText(
              utf8, capacity, utf8_bytes, offset, "   ", 3u,
              objects, object_count, bidi))
        return false;
    }
  }
  return true;
}

bool ActRaiserLocalizationNameCompose_InsertPageIndicator(
    char *utf8, size_t capacity, size_t *utf8_bytes,
    uint32_t page_index, uint32_t page_count,
    ArLocalizationInlineObjectSnapshot *objects, uint8_t object_count, ArTextBidiSpans *bidi) {
  if (page_count <= 1u) return true;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  if (!CollectNameEntryLines(
          utf8, *utf8_bytes, starts, ends,
          sizeof(starts) / sizeof(starts[0]), &line_count) ||
      line_count < kActRaiserLocalizationNameEntryRows)
    return false;
  char indicator[64];
  const int written = snprintf(
      indicator, sizeof(indicator), "< %u/%u >\n",
      (unsigned)(page_index + 1u), (unsigned)page_count);
  if (written <= 0 || (size_t)written >= sizeof(indicator)) return false;
  return InsertNameEntryText(
      utf8, capacity, utf8_bytes,
      starts[line_count - kActRaiserLocalizationNameEntryRows],
      indicator, (size_t)written, objects, object_count, bidi);
}

bool ActRaiserLocalizationNameCompose_InsertFieldUnderlines(
    const char *utf8, size_t utf8_bytes,
    ArLocalizationInlineObjectSnapshot *objects, size_t capacity,
    uint8_t *object_count) {
  if (!utf8 || !utf8_bytes || !objects || !object_count) return false;
  size_t starts[64];
  size_t ends[64];
  size_t line_count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= utf8_bytes; ++index) {
    if (index != utf8_bytes && utf8[index] != '\n') continue;
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
    if (!ArUnicodeGrapheme_Next(
            utf8, utf8_bytes, offset, NULL, &next) ||
        next <= offset || next > ends[name_line] || next > UINT32_MAX ||
        !ActRaiserLocalizationText_InsertInlineObject(
            objects, capacity, object_count,
            (ArLocalizationInlineObjectSnapshot){
                kArLocalizationInlineObject_NameFieldUnderline,
                (uint32_t)next}))
      return false;
    offset = next;
    ++graphemes;
  }
  return graphemes == kActRaiserLocalizationNameLength;
}
