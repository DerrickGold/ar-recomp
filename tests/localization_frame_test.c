#include "localization/localization_frame.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++failures;                                                            \
  }                                                                        \
} while (0)

int main(void) {
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(frame.snapshot_count == 0 && frame.text_bytes == 0);
  CHECK(ArLocalizationFrame_IsValid(&frame));
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "fr-FR", "test.font", UINT64_C(1), 7,
      &frame.settings));
  ArFontResourceId fallbacks[] = {2, 3};
  CHECK(ArLocalizationFrame_SetFallbackFonts(&frame, fallbacks, 2));
  fallbacks[0] = 4;
  CHECK(frame.fallback_font_count == 2 &&
        frame.fallback_fonts[0] == 2);
  CHECK(!ArLocalizationFrame_SetFallbackFonts(&frame, NULL, 1));
  CHECK(!ArLocalizationFrame_SetFallbackFonts(
      &frame, fallbacks, kArTextPresentationMaximumFallbackFonts + 1));
  const ArFontResourceId invalid_fallbacks[] = {2, 0};
  CHECK(!ArLocalizationFrame_SetFallbackFonts(&frame, invalid_fallbacks, 2));
  CHECK(frame.fallback_font_count == 2 &&
        frame.fallback_fonts[1] == 3);
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "fr-FR", "test.font", UINT64_C(1), 7, &frame.settings));
  CHECK(!frame.fallback_font_count && !frame.fallback_fonts[0]);
  const ArLocalizationFrame font_before = frame;
  CHECK(!ArLocalizationFrame_SetFont(&frame, "fr", "test", 0, 7, &frame.settings));
  CHECK(!memcmp(&frame, &font_before, sizeof(frame)));
  const ArTextCellDestination destination = {
      .background = 3,
      .screen = kArTextCellScreen_Composited,
      .tilemap_base_words = 0x3800,
  };
  static const char text[] = "Élise";
  const ArTextCellRegion native_preserve = {15, 21, 1, 1};
  CHECK(ArLocalizationFrame_AddText(
      &frame, 1, destination, (ArTextCellRegion){5, 19, 23, 7},
      text, strlen(text), 3, 5, 9, kArTextDirection_LeftToRight, 6,
      &native_preserve, 1));
  size_t bytes = 0;
  CHECK(!strcmp(ArLocalizationFrame_GetText(&frame, 0, &bytes), text));
  CHECK(bytes == strlen(text));
  CHECK(frame.cells.count == 1);
  CHECK(frame.cells.records[0].snapshot_slot == 0);
  CHECK(frame.snapshots[0].native_preserve_count == 1);
  CHECK(frame.snapshots[0].native_preserves[0].column == 15);
  CHECK(!strcmp(frame.snapshots[0].language.locale, "fr-FR"));
  CHECK(frame.snapshots[0].language.direction == kArTextDirection_LeftToRight);
  ArLocalizationTextLanguage language = {.locale = "ar",
      .direction = kArTextDirection_RightToLeft};
  CHECK(ArLocalizationFrame_SetTextLanguage(&frame, &language));
  CHECK(!strcmp(frame.snapshots[0].language.locale, "ar"));
  CHECK(frame.snapshots[0].language.direction == kArTextDirection_RightToLeft);
  CHECK(!strcmp(frame.locale, "fr-FR") && frame.primary_font == 1);
  const ArLocalizationFrame language_before = frame;
  CHECK(!ArLocalizationFrame_SetTextLanguage(&frame, NULL));
  language.locale[0] = 0;
  CHECK(!ArLocalizationFrame_SetTextLanguage(&frame, &language));
  memset(language.locale, 'a', sizeof(language.locale));
  CHECK(!ArLocalizationFrame_SetTextLanguage(&frame, &language));
  language.locale[2] = 0;
  language.direction = (ArTextDirection)99;
  CHECK(!ArLocalizationFrame_SetTextLanguage(&frame, &language));
  CHECK(!memcmp(&frame, &language_before, sizeof(frame)));
  ArTextBidiSpans bidi = {.count = 1, .spans = {{0, sizeof(text)-1, kArTextDirection_Auto}}};
  CHECK(ArLocalizationFrame_SetTextBidiSpans(&frame, &bidi));
  CHECK(frame.bidi.count == 1 && frame.snapshots[0].bidi_span_count == 1);
  bidi.spans[0].end = 1; // Cannot split É or change already published spans.
  CHECK(!ArLocalizationFrame_SetTextBidiSpans(&frame, &bidi));
  CHECK(frame.bidi.spans[0].end == sizeof(text)-1);
  CHECK(ArLocalizationFrame_AddIndicator(
      &frame, 1, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){15, 21, 1, 1}));
  CHECK(frame.indicator_count == 1);
  CHECK(frame.indicators[0].surface_id == 1);
  CHECK(frame.indicators[0].kind ==
        kArLocalizationIndicator_DialogueContinue);
  CHECK(!ArLocalizationFrame_AddIndicator(
      &frame, 2, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){15, 21, 1, 1}));
  CHECK(!ArLocalizationFrame_AddIndicator(
      &frame, 1, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){4, 21, 1, 1}));

  ArLocalizationFrame object_frame;
  ArLocalizationFrame window_frame;
  ArLocalizationFrame_Reset(&window_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &window_frame, "fr", "test.font", UINT64_C(1), 7,
      &window_frame.settings));
  const char rolling[] = "Avant\n\xC3\x89" "tape";
  CHECK(!ArLocalizationFrame_AddDialogueWindow(
      &window_frame, 1, destination, (ArTextCellRegion){5, 19, 23, 7},
      rolling, strlen(rolling), 7, 10, 9, kArTextDirection_LeftToRight, 6));
  CHECK(window_frame.snapshot_count == 0); /* no half-accent activation */
  CHECK(ArLocalizationFrame_AddDialogueWindow(
      &window_frame, 1, destination, (ArTextCellRegion){5, 19, 23, 7},
      rolling, strlen(rolling), 8, 10, 9, kArTextDirection_LeftToRight, 6));
  CHECK(window_frame.snapshots[0].layout == kArLocalizationTextLayout_DialogueWindow);
  CHECK(window_frame.snapshots[0].revealed_utf8_bytes == 8);

  /* An intentional empty replacement has a real ownership slot, not a
   * missing snapshot. Reject malformed empty payloads transactionally. */
  ArLocalizationFrame empty_frame;
  ArLocalizationFrame_Reset(&empty_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &empty_frame, "en", "test.font", UINT64_C(1), 7, &empty_frame.settings));
  CHECK(ArLocalizationFrame_AddDialogueWindow(
      &empty_frame, 1, destination, (ArTextCellRegion){5, 19, 24, 6},
      "", 0, 0, 0, 9, kArTextDirection_LeftToRight, 8));
  CHECK(empty_frame.snapshot_count == 1 && empty_frame.cells.count == 1);
  CHECK(empty_frame.text_bytes == 1);
  bytes = 99;
  CHECK(!strcmp(ArLocalizationFrame_GetText(&empty_frame, 0, &bytes), ""));
  CHECK(bytes == 0);
  CHECK(ArLocalizationFrame_AddIndicator(
      &empty_frame, 1, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){17, 24, 1, 1}));
  const ArLocalizationFrame empty_before = empty_frame;
  CHECK(!ArLocalizationFrame_AddText(
      &empty_frame, 2, destination, (ArTextCellRegion){3, 3, 4, 4},
      "", 0, 0, 1, 9, kArTextDirection_LeftToRight, 8, NULL, 0));
  const ArLocalizationInlineObjectSnapshot empty_object = {
      kArLocalizationInlineObject_StatusLife, 0};
  CHECK(!ArLocalizationFrame_AddTextWithObjects(
      &empty_frame, 2, destination, (ArTextCellRegion){3, 3, 4, 4},
      "", 0, 0, 0, 9, kArTextDirection_LeftToRight, 8, NULL, 0, &empty_object, 1));
  CHECK(memcmp(&empty_before, &empty_frame, sizeof(empty_frame)) == 0);

  ArLocalizationFrame_Reset(&object_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &object_frame, "en-US", "test.font", UINT64_C(1), 7,
      &object_frame.settings));
  static const char object_text[] = "A\xE2\x80\x87" "B";
  const ArLocalizationInlineObjectSnapshot objects[] = {
      {kArLocalizationInlineObject_StatusLife, 4},
  };
  CHECK(ArLocalizationFrame_AddTextWithObjects(
      &object_frame, 4, destination, (ArTextCellRegion){2, 2, 10, 2},
      object_text, strlen(object_text), 3, 3, 10,
      kArTextDirection_LeftToRight, 6, NULL, 0, objects, 1));
  CHECK(object_frame.inline_object_count == 1);
  CHECK(object_frame.snapshots[0].inline_object_count == 1);
  CHECK(object_frame.inline_objects[0].end_utf8_byte == 4);
  const ArLocalizationInlineObjectSnapshot shared_cluster[] = {
      {kArLocalizationInlineObject_NameFinish, 4},
      {kArLocalizationInlineObject_NameCursor, 4},
  };
  CHECK(ArLocalizationFrame_AddTextWithObjects(
      &object_frame, 6, destination, (ArTextCellRegion){2, 5, 10, 2},
      object_text, strlen(object_text), 3, 3, 11,
      kArTextDirection_LeftToRight, 6, NULL, 0,
      shared_cluster, 2));
  CHECK(object_frame.inline_object_count == 3);
  CHECK(object_frame.inline_objects[1].end_utf8_byte == 4);
  CHECK(object_frame.inline_objects[2].end_utf8_byte == 4);
  const ArLocalizationFrame object_before = object_frame;
  const ArLocalizationInlineObjectSnapshot invalid_object =
      {kArLocalizationInlineObject_StatusLife, 99};
  CHECK(!ArLocalizationFrame_AddTextWithObjects(
      &object_frame, 5, destination, (ArTextCellRegion){14, 2, 10, 2},
      object_text, strlen(object_text), 3, 3, 10,
      kArTextDirection_LeftToRight, 6, NULL, 0, &invalid_object, 1));
  CHECK(memcmp(&object_before, &object_frame, sizeof(object_frame)) == 0);

  ArLocalizationFrame table_frame;
  ArLocalizationFrame_Reset(&table_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &table_frame, "en-US", "test.font", UINT64_C(1), 7,
      &table_frame.settings));
  /* A grid layout carries its own geometry; the frame interns it and the
   * snapshot references it by index. */
  ArLocalizationTextGrid grid = {
      .rule_count = 2, .row_height = 2,
      .shared_column_count = 2, .shared_template_line = 0,
      .rules = {
          {.first_line = 0, .last_line = 0, .field_count = 2, .cell_count = 2,
           .shared_columns = true,
           .cells = {{0, 10, kArTextHorizontalAlignment_Leading, true, false, false},
                     {10, 26, kArTextHorizontalAlignment_Trailing, false, false, false}}},
          {.first_line = 1, .last_line = 19,
           .field_count = kArLocalizationGridAnyFieldCount,
           .native_reserved = true},
      }};
  CHECK(ArLocalizationFrame_AddTextWithGrid(
      &table_frame, 7, destination, (ArTextCellRegion){3, 6, 26, 20},
      text, strlen(text), 5, 5, 12,
      kArTextDirection_LeftToRight, 7, &grid, NULL,
      NULL, 0, NULL, 0));
  CHECK(table_frame.snapshots[0].layout == kArLocalizationTextLayout_Grid);
  CHECK(table_frame.snapshots[0].grid_index == 1 && table_frame.grid_count == 1);
  CHECK(ArLocalizationFrame_GetGrid(&table_frame, &table_frame.snapshots[0]) ==
        &table_frame.grids[0]);
  const ArLocalizationTextRowRule *row =
      ArLocalizationGrid_FindRow(&table_frame.grids[0], 0, 2);
  CHECK(row && row->shared_columns && row->cells[1].end == 26);
  /* A reserved rule matches whatever shape the row parsed into. */
  CHECK(ArLocalizationGrid_FindRow(&table_frame.grids[0], 4, 5) ==
        &table_frame.grids[0].rules[1]);
  CHECK(!ArLocalizationGrid_FindRow(&table_frame.grids[0], 0, 3));
  /* Structure is copied relative to each snapshot, including an unaligned
   * start in the shared text pool. A dynamic pipe remains ordinary text. */
  const char value_text[] = "É|li|2";
  uint8_t boundaries[AR_TEXT_BOUNDARY_BYTES(sizeof(value_text))] = {0};
  ArTextBoundary_Set(boundaries, 5, true);
  CHECK(ArLocalizationFrame_AddTextWithGrid(
      &table_frame, 8, destination, (ArTextCellRegion){3, 6, 26, 20},
      value_text, sizeof(value_text) - 1, 6, 6, 13,
      kArTextDirection_LeftToRight, 7, &grid, boundaries, NULL, 0, NULL, 0));
  const size_t value_offset = table_frame.snapshots[1].utf8_offset;
  memset(boundaries, 0, sizeof(boundaries));
  for (size_t i = 0; i < sizeof(value_text) - 1; ++i)
    CHECK(ArTextBoundary_Get(table_frame.structural_boundaries, value_offset + i) == (i == 5));
  /* Geometry the renderer could not act on is refused where it is published. */
  ArLocalizationTextGrid invalid = grid;
  invalid.rules[0].cells[1].end = 27; /* outside the claimed region */
  CHECK(!ArLocalizationFrame_AddTextWithGrid(
      &table_frame, 9, destination, (ArTextCellRegion){3, 6, 26, 20},
      text, strlen(text), 5, 5, 12,
      kArTextDirection_LeftToRight, 7, &invalid, NULL, NULL, 0, NULL, 0));
  /* A grid layout without geometry, and geometry without a grid layout, are
   * both rejected. */
  CHECK(!ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &table_frame, 9, destination, (ArTextCellRegion){3, 6, 26, 20},
      text, strlen(text), 5, 5, 12,
      kArTextDirection_LeftToRight, 7, kArLocalizationTextLayout_Grid,
      NULL, 0, NULL, 0));
  /* The key separator belongs to the surface that was just published, and a
   * frame with no surfaces has nowhere to put one. */
  CHECK(ArLocalizationFrame_SetKeySeparator(&table_frame, " ", 1));
  CHECK(table_frame.snapshots[1].key_separator_bytes == 1 &&
        table_frame.snapshots[1].key_separator[0] == ' ');
  CHECK(!ArLocalizationFrame_SetKeySeparator(&table_frame, "", 0));
  CHECK(!ArLocalizationFrame_SetKeySeparator(&table_frame, "        ", 8));
  const ArLocalizationFrame table_before = table_frame;
  CHECK(!ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &table_frame, 8, destination, (ArTextCellRegion){30, 6, 2, 2},
      text, strlen(text), 5, 5, 12,
      kArTextDirection_LeftToRight, 7,
      (ArLocalizationTextLayoutKind)99,
      NULL, 0, NULL, 0));
  CHECK(memcmp(&table_before, &table_frame, sizeof(table_frame)) == 0);

  const ArLocalizationFrame before = frame;
  CHECK(!ArLocalizationFrame_AddText(
      &frame, 2, destination, (ArTextCellRegion){63, 63, 2, 2},
      text, strlen(text), 0, 5, 9, kArTextDirection_LeftToRight, 6,
      NULL, 0));
  CHECK(memcmp(&before, &frame, sizeof(frame)) == 0);
  CHECK(!ArLocalizationFrame_GetText(&frame, 1, NULL));

  /* Every public consumer rejects mismatched ABI extents and forged bounded
   * pool metadata before following a tail member or nested index. */
  CHECK(ArLocalizationFrame_IsValid(&frame));
  ArLocalizationFrame malformed = frame;
  malformed.struct_size = 8;
  const ArLocalizationFrame malformed_before = malformed;
  CHECK(!ArLocalizationFrame_SetFont(
      &malformed, "en", "test", 1, 1, &frame.settings));
  CHECK(!memcmp(&malformed, &malformed_before, sizeof(malformed)));
  CHECK(!ArLocalizationFrame_GetText(&malformed, 0, NULL));
  CHECK(!ArLocalizationFrame_IsValid(&malformed));
  malformed = frame;
  malformed.text_bytes = kArLocalizationFrameTextCapacity + 1u;
  CHECK(!ArLocalizationFrame_IsValid(&malformed));
  malformed = frame;
  malformed.grid_count = kArLocalizationFrameGridCapacity + 1u;
  CHECK(!ArLocalizationFrame_IsValid(&malformed));
  malformed = frame;
  malformed.snapshots[0].utf8_offset = malformed.text_bytes;
  CHECK(!ArLocalizationFrame_IsValid(&malformed));
  malformed = frame;
  malformed.cells.records[0].snapshot_slot =
      (int8_t)malformed.snapshot_count;
  CHECK(!ArLocalizationFrame_IsValid(&malformed));
  puts("localization frame checks passed");
  return failures ? 1 : 0;
}
