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
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "fr-FR", "test.font", "/tmp/test.ttf", 7,
      &frame.settings));
  char fallback[] = "/tmp/fallback.ttf";
  const char *fallbacks[] = {fallback, "/tmp/second.ttf"};
  CHECK(ArLocalizationFrame_SetFallbackFonts(&frame, fallbacks, 2));
  fallback[5] = 'X';
  CHECK(frame.fallback_font_count == 2 &&
        !strcmp(frame.fallback_font_paths[0], "/tmp/fallback.ttf"));
  CHECK(!ArLocalizationFrame_SetFallbackFonts(&frame, NULL, 1));
  CHECK(!ArLocalizationFrame_SetFallbackFonts(
      &frame, fallbacks, kArTextPresentationMaximumFallbackFonts + 1));
  const char *invalid_fallbacks[] = {"valid.ttf", ""};
  CHECK(!ArLocalizationFrame_SetFallbackFonts(&frame, invalid_fallbacks, 2));
  CHECK(frame.fallback_font_count == 2 &&
        !strcmp(frame.fallback_font_paths[1], "/tmp/second.ttf"));
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "fr-FR", "test.font", "/tmp/test.ttf", 7, &frame.settings));
  CHECK(!frame.fallback_font_count && !frame.fallback_font_paths[0][0]);
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
      &window_frame, "fr", "test.font", "/tmp/test.ttf", 7,
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
      &empty_frame, "en", "test.font", "/tmp/test.ttf", 7, &empty_frame.settings));
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
      &object_frame, "en-US", "test.font", "/tmp/test.ttf", 7,
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
      &table_frame, "en-US", "test.font", "/tmp/test.ttf", 7,
      &table_frame.settings));
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &table_frame, 7, destination, (ArTextCellRegion){3, 6, 26, 20},
      text, strlen(text), 5, 5, 12,
      kArTextDirection_LeftToRight, 7,
      kArLocalizationTextLayout_StatusCities,
      NULL, 0, NULL, 0));
  CHECK(table_frame.snapshots[0].layout ==
        kArLocalizationTextLayout_StatusCities);
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
  puts("localization frame checks passed");
  return failures ? 1 : 0;
}
