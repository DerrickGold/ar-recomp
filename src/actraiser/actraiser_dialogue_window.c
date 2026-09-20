#include "actraiser/actraiser_dialogue_window.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "localization/unicode_grapheme.h"
#include <string.h>

static uint32_t CountClusters(const char *utf8, size_t bytes) {
  uint32_t count = 0;
  size_t offset = 0;
  while (offset < bytes) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(utf8, bytes, offset, NULL, &next) ||
        next <= offset || count == UINT32_MAX)
      return UINT32_MAX;
    offset = next;
    ++count;
  }
  return count;
}

bool ActRaiserDialogueWindow_Build(ActRaiserDialogueWindow *window,
                                   const ArDialogueSession *session,
                                   const ArDialoguePageSnapshot *current,
                                   uint16_t native_first_page,
                                   uint16_t clear_control_count) {
  if (window->valid && window->revision == current->source_revision &&
      window->native_first_page == native_first_page &&
      window->native_clear_control_count == clear_control_count &&
      window->current_page == current->page_index)
    return true;
  window->valid = false;
  uint32_t first_page = native_first_page;
  size_t first_offset = 0;
  if (clear_control_count) {
    if (!ArDialogueSession_GetControlPosition(session, clear_control_count - 1u,
                                              &first_page, &first_offset) ||
        first_page > current->page_index)
      return false;
  }
  if (first_page > current->page_index)
    first_page = current->page_index;
  window->bytes = 0;
  memset(window->structural_boundaries, 0,
         sizeof(window->structural_boundaries));
  window->bidi.count = 0;
  memset(&window->styles, 0, sizeof(window->styles));
  for (uint32_t index = first_page; index <= current->page_index; ++index) {
    ArDialoguePageSnapshot page;
    if (!ArDialogueSession_GetAuthoredPage(session, index, &page))
      return false;
    const size_t source_offset = index == first_page ? first_offset : 0;
    if (source_offset > page.utf8_bytes)
      return false;
    if (index != first_page) {
      if (window->bytes + 1u >= sizeof(window->text))
        return false;
      window->text[window->bytes++] = '\n';
    }
    const size_t offset = window->bytes;
    size_t bytes = 0;
    uint8_t object_count = 0;
    if (!ActRaiserLocalizationText_Normalize(
            page.utf8 + source_offset, page.utf8_bytes - source_offset, NULL, 0,
            true, window->text + offset, sizeof(window->text) - offset, &bytes,
            NULL, 0, &object_count, window->reveal_offsets) ||
        !ActRaiserLocalizationText_MapBidiSpans(
            page.bidi_spans, page.bidi_span_count, source_offset,
            page.utf8_bytes - source_offset, window->reveal_offsets,
            window->text + offset, bytes, offset, &window->bidi) ||
        !ActRaiserTextStyle_AppendPage(
            &window->styles, &page, source_offset,
            page.utf8_bytes - source_offset, window->reveal_offsets,
            window->text + offset, bytes, offset, NULL, 0))
      return false;
    /* The source slice need not begin on a bitmap-byte boundary, so remap the
     * relevant bits explicitly instead of copying packed bytes. */
    for (size_t source_index = 0;
         source_index < page.utf8_bytes - source_offset; ++source_index) {
      if (!ArTextBoundary_Get(page.structural_boundaries,
                              source_offset + source_index))
        continue;
      const size_t mapped = window->reveal_offsets[source_index];
      if (mapped < bytes && window->text[offset + mapped] == ' ')
        ArTextBoundary_Set(window->structural_boundaries, offset + mapped,
                           true);
    }
    window->bytes += bytes;
    if (index == current->page_index) {
      window->current_page_offset = offset;
      window->current_page_source_offset = source_offset;
      window->current_page_source_bytes = page.utf8_bytes - source_offset;
    }
  }
  window->clusters = CountClusters(window->text, window->bytes);
  if (window->clusters == UINT32_MAX)
    return false;
  window->native_first_page = native_first_page;
  window->native_clear_control_count = clear_control_count;
  window->current_page = current->page_index;
  window->revision = current->source_revision;
  window->valid = true;
  return true;
}

size_t
ActRaiserDialogueWindow_RevealedBytes(const ActRaiserDialogueWindow *window,
                                      size_t source_revealed) {
  source_revealed = source_revealed > window->current_page_source_offset
                        ? source_revealed - window->current_page_source_offset
                        : 0;
  if (source_revealed > window->current_page_source_bytes)
    source_revealed = window->current_page_source_bytes;
  const size_t revealed =
      window->current_page_offset + window->reveal_offsets[source_revealed];
  return revealed < window->bytes ? revealed : window->bytes;
}

bool ActRaiserDialogueWindow_ControlClears(const char *id) {
  return id && !strncmp(id, "reset_text_cursor.", 18);
}
