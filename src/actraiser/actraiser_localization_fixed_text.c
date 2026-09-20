#include "actraiser/actraiser_localization_fixed_text.h"
#include "actraiser/actraiser_localization_name_compose.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "localization/unicode_grapheme.h"
#include <stdio.h>
#include <string.h>

bool ActRaiserLocalizationFixedText_FromPage(const ArDialoguePageSnapshot *page,
                                             ActRaiserResolvedText *text,
                                             char *error,
                                             size_t error_capacity) {
  memset(text, 0, sizeof(*text));
  const char *id = page->message_id;
  const bool name_entry = !strcmp(id, "name_entry.prompt_and_alphabet");
  uint16_t offsets[kArLocalizationFrameTextCapacity + 1];
  /* Every fixed text preserves authored paragraph spacing. The keyboard has
   * its own row reconstruction below; substituted values remain inline data. */
  bool normalized =
      name_entry
          ? ActRaiserLocalizationText_Normalize(
                page->utf8, page->utf8_bytes, page->inline_objects,
                page->inline_object_count, false, text->utf8,
                sizeof(text->utf8), &text->utf8_bytes, text->inline_objects,
                kArLocalizationFrameInlineObjectCapacity,
                &text->inline_object_count, offsets)
          : ActRaiserLocalizationText_NormalizeStructured(
                page->utf8, page->utf8_bytes, page->inline_objects,
                page->inline_object_count, true, text->utf8, sizeof(text->utf8),
                &text->utf8_bytes, text->inline_objects,
                kArLocalizationFrameInlineObjectCapacity,
                &text->inline_object_count, offsets,
                page->structural_boundaries,
                text->structural_boundaries);
  if (!normalized ||
      !ActRaiserLocalizationText_MapBidiSpans(
          page->bidi_spans, page->bidi_span_count, 0, page->utf8_bytes, offsets,
          text->utf8, text->utf8_bytes, 0, &text->bidi) ||
      !ActRaiserTextStyle_AppendPage(&text->styles, page, 0, page->utf8_bytes,
                                     offsets, text->utf8, text->utf8_bytes, 0,
                                     error, error_capacity))
    return false;
  if (name_entry &&
      (!ActRaiserLocalizationNameCompose_ClearUnderlineRow(text) ||
       !ActRaiserLocalizationNameCompose_PrepareField(text) ||
       !ActRaiserLocalizationNameCompose_InsertPageIndicator(
           text, page->page_index, page->page_count) ||
       !ActRaiserLocalizationNameCompose_ExpandKeyGutters(text)))
    return false;
  /* These extracted records include a native selector outside our text claim.
   * Other '>' characters remain authored text. */
  if ((!strcmp(id, "title.start_prompt") ||
       !strcmp(id, "title.selector.professional")) &&
      text->utf8_bytes && text->utf8[0] == '>') {
    size_t first = 1;
    while (first < text->utf8_bytes && text->utf8[first] == ' ')
      ++first;
    memmove(text->utf8, text->utf8 + first, text->utf8_bytes - first + 1);
    ArTextBidiSpans_Edit(&text->bidi, 0, (uint32_t)first, 0);
    ActRaiserTextStyle_Edit(&text->styles, 0, (uint32_t)first, 0);
    text->utf8_bytes -= first;
  }
  for (size_t offset = 0, next; offset < text->utf8_bytes; offset = next) {
    if (!ArUnicodeGrapheme_Next(text->utf8, text->utf8_bytes, offset, NULL,
                                &next) ||
        next <= offset)
      return false;
    ++text->cluster_count;
  }
  text->source_revision = page->source_revision;
  text->language.direction =
      page->direction == kArLanguageDirection_RightToLeft
          ? kArTextDirection_RightToLeft
      : page->direction == kArLanguageDirection_LeftToRight
          ? kArTextDirection_LeftToRight
          : kArTextDirection_Auto;
  snprintf(text->language.locale, sizeof(text->language.locale), "%s",
           page->locale);
  return text->source_revision &&
         ArLocalizationTextLanguage_IsValid(&text->language);
}
