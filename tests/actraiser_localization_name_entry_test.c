#include "actraiser/actraiser_localization_name_entry.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "check failed at %s:%d: %s\n",                         \
            __FILE__, __LINE__, #condition);                                 \
    return 1;                                                                \
  }                                                                          \
} while (0)

static int TestCapture(void) {
  uint8_t wram[0x034E] = {0};
  wram[0x034B] = 4;
  wram[0x034C] = 2;
  ActRaiserLocalizationNameEntryState state;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &state, wram, sizeof(wram)));
  CHECK(state.entered_length == 0);
  CHECK(state.selected_row == 2);
  CHECK(state.selected_column == 4);
  CHECK(strlen(state.display_name) == 24);
  const uint64_t blank_revision = state.revision;

  memcpy(wram + 0x0288, "CODEX", 5);
  wram[0x034D] = 5;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &state, wram, sizeof(wram)));
  CHECK(!memcmp(state.display_name, "CODEX", 5));
  CHECK(!strcmp(state.native_name, "CODEX"));
  CHECK(strlen(state.display_name) == 14);
  CHECK(state.revision != blank_revision);

  wram[0x034D] = 9;
  CHECK(!ActRaiserLocalizationNameEntry_Capture(
      &state, wram, sizeof(wram)));
  return 0;
}

static int TestCursorArtwork(void) {
  uint16_t vram[0x8000] = {0};
  const uint16_t palette[] = {0x7FFF, 0, 0x7F33, 0x7FFF};
  uint32_t argb[kActRaiserLocalizationNameCursorPixels] = {0};
  /* Synthetic bitplanes exercise all four indices, including transparent
   * color 0 even when CGRAM[0] is white, and opaque black outlines. */
  vram[0x5000 + 0x3E * 8] = 0x3355;
  vram[0x5000 + 0x3E * 8 + 7] = 0x8080;
  CHECK(ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x5000, vram, 0x8000, palette, 4));
  const uint32_t expected[] = {0, 0xFF000000, 0xFF9CCEFF, 0xFFFFFFFF};
  for (unsigned x = 0; x < 8; ++x)
    CHECK(argb[x] == expected[x % 4]);
  for (unsigned pixel = 8; pixel < 56; ++pixel)
    CHECK(argb[pixel] == 0);
  CHECK(argb[56] == 0xFFFFFFFF);
  CHECK(argb[63] == 0);

  /* Invalid memory leaves the caller's prior snapshot untouched. */
  uint32_t before[kActRaiserLocalizationNameCursorPixels];
  memcpy(before, argb, sizeof(before));
  CHECK(!ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x5000, vram, 0x7FFF, palette, 4));
  CHECK(!ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x5000, vram, 0x8000, palette, 3));
  CHECK(!ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x5000, NULL, 0x8000, palette, 4));
  CHECK(!ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x5000, vram, 0x8000, NULL, 4));
  CHECK(!ActRaiserLocalizationNameEntry_CaptureCursor(
      NULL, 0x5000, vram, 0x8000, palette, 4));
  CHECK(!memcmp(before, argb, sizeof(before)));

  /* Character addresses use the PPU's 15-bit VRAM word wrapping. */
  vram[0x1F0] = 0x3355;
  CHECK(ActRaiserLocalizationNameEntry_CaptureCursor(
      argb, 0x8000, vram, 0x8000, palette, 4));
  for (unsigned x = 0; x < 8; ++x)
    CHECK(argb[x] == expected[x % 4]);
  return 0;
}

static int TestSelectedKey(void) {
  uint8_t wram[0x034E] = {0};
  wram[0x034B] = 12;
  wram[0x034C] = 4;
  ActRaiserLocalizationNameEntryState state;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &state, wram, sizeof(wram)));
  static const char message[] =
      "Translated prompt\n        \n--------\n"
      "A B C D E F G H I J K L M\n"
      "N O P Q R S T U V W X Y Z\n"
      "a b c d e f g h i j k l m\n"
      "n o p q r s t u v w x y z\n"
      "0 1 2 3 4 5 6 7 8 9 . \xE2\x80\x87 \xE2\x80\x87";
  uint32_t end = 0;
  uint32_t start = 0;
  CHECK(ActRaiserLocalizationNameEntry_SelectedKeyRange(
      &state, message, sizeof(message) - 1u, &start, &end));
  CHECK(end - start == 3); /* semantic finish object reserves one figure space */
  CHECK(ActRaiserLocalizationNameEntry_SelectedKeyEnd(
      &state, message, sizeof(message) - 1u, &end));
  CHECK(end == sizeof(message) - 1u);

  wram[0x034B] = 1;
  wram[0x034C] = 0;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntry_SelectedKeyEnd(
      &state, message, sizeof(message) - 1u, &end));
  CHECK(end > 1 && message[end - 1u] == 'B');

  static const char short_grid[] = "x\na\nb\nc\nd\ne";
  CHECK(!ActRaiserLocalizationNameEntry_SelectedKeyEnd(
      &state, short_grid, sizeof(short_grid) - 1u, &end));
  return 0;
}

static int TestUnicodeTracker(void) {
  uint8_t wram[0x034E] = {0};
  wram[0x034B] = 12;
  wram[0x034C] = 0;
  ActRaiserLocalizationNameEntryState native_state;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));

  ActRaiserLocalizationNameEntryTracker tracker;
  ActRaiserLocalizationNameEntryTracker_Init(&tracker);
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(tracker.keyboard_page == 0);
  CHECK(ActRaiserLocalizationNameEntryTracker_Synchronize(
      &tracker, &native_state, NULL, 0));

  /* Native horizontal wrapping selects the next authored keyboard page. */
  wram[0x034B] = 0;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(tracker.keyboard_page == 1);
  CHECK(ActRaiserLocalizationNameEntryTracker_Synchronize(
      &tracker, &native_state, NULL, 0));

  /* The native mirror stores the deterministic compatible key while the
   * canonical display retains the selected Unicode grapheme. */
  wram[0x0288] = 'e';
  wram[0x034D] = 1;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  static const char accented[] = "e\xCC\x81";
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(ActRaiserLocalizationNameEntryTracker_Synchronize(
      &tracker, &native_state, accented, sizeof(accented) - 1u));
  CHECK(!strcmp(tracker.utf8_name, accented));
  CHECK(!strcmp(tracker.compatibility_name, "e"));
  CHECK(tracker.grapheme_count == 1);
  char display[kActRaiserLocalizationUnicodeNameDisplayCapacity];
  CHECK(ActRaiserLocalizationNameEntryTracker_CopyDisplayName(
      &tracker, display, sizeof(display)));
  CHECK(!memcmp(display, accented, sizeof(accented) - 1u));
  CHECK(strlen(display) == sizeof(accented) - 1u + 7u * 3u);

  /* Toggling to retail must append the actual native key, without replacing
   * the earlier accented grapheme or silently selecting a translated key. */
  wram[0x0289] = 'A';
  wram[0x034D] = 2;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
      &tracker, &native_state));
  CHECK(!strcmp(tracker.utf8_name, "e\xCC\x81" "A"));
  CHECK(tracker.keyboard_page == 1);
  wram[0x0289] = 0;
  wram[0x034D] = 1;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
      &tracker, &native_state));
  CHECK(!strcmp(tracker.utf8_name, accented));

  wram[0x0288] = 0;
  wram[0x034D] = 0;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(ActRaiserLocalizationNameEntryTracker_Synchronize(
      &tracker, &native_state, NULL, 0));
  CHECK(!tracker.utf8_name[0] && tracker.grapheme_count == 0);

  /* Reverse edge wrapping cycles from the first page to the last. */
  ActRaiserLocalizationNameEntryTracker_Init(&tracker);
  wram[0x034B] = 0;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(ActRaiserLocalizationNameEntryTracker_Synchronize(
      &tracker, &native_state, NULL, 0));
  wram[0x034B] = 12;
  CHECK(ActRaiserLocalizationNameEntry_Capture(
      &native_state, wram, sizeof(wram)));
  CHECK(ActRaiserLocalizationNameEntryTracker_SelectPage(
      &tracker, &native_state, 3));
  CHECK(tracker.keyboard_page == 2);
  return 0;
}

int main(void) {
  if (TestCapture()) return 1;
  if (TestCursorArtwork()) return 1;
  if (TestSelectedKey()) return 1;
  if (TestUnicodeTracker()) return 1;
  return 0;
}
