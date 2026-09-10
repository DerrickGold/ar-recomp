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
