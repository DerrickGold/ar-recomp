#ifndef ACTRAISER_LOCALIZATION_NAME_ENTRY_H
#define ACTRAISER_LOCALIZATION_NAME_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACTRAISER_LOCALIZATION_NAME_ENTRY_ABI_VERSION UINT32_C(2)
#define ACTRAISER_LOCALIZATION_NAME_ENTRY_TRACKER_ABI_VERSION UINT32_C(1)

enum {
  kActRaiserLocalizationNameLength = 8,
  kActRaiserLocalizationNameEntryRows = 5,
  kActRaiserLocalizationNameEntryColumns = 13,
  kActRaiserLocalizationNameCursorPixels = 8 * 8,
  /* Eight UTF-8 figure spaces plus a terminator is the widest empty field. */
  kActRaiserLocalizationNameEntryDisplayCapacity = 25,
  /* Eight extended grapheme clusters can require substantially more than
   * eight bytes (combining marks, emoji sequences, and complex scripts). */
  kActRaiserLocalizationUnicodeNameCapacity = 257,
  kActRaiserLocalizationUnicodeNameDisplayCapacity = 281,
};

/* Canonical read-only view of the untouched USA name-entry loop. The host
 * presentation consumes this state; native code remains authoritative for
 * input, acceptance, and its eight-byte compatibility buffer. */
typedef struct ActRaiserLocalizationNameEntryState {
  size_t struct_size;
  uint32_t abi_version;
  uint8_t entered_length;
  uint8_t selected_row;
  uint8_t selected_column;
  char native_name[kActRaiserLocalizationNameLength + 1];
  char display_name[kActRaiserLocalizationNameEntryDisplayCapacity];
  uint64_t revision;
} ActRaiserLocalizationNameEntryState;

/* Host-owned canonical entry state. Native coordinates/input continue to run
 * unchanged, while this state maps those semantic key positions to Unicode
 * graphemes and optional authored keyboard pages. */
typedef struct ActRaiserLocalizationNameEntryTracker {
  size_t struct_size;
  uint32_t abi_version;
  bool initialized;
  uint8_t previous_length;
  uint8_t previous_row;
  uint8_t previous_column;
  uint8_t grapheme_count;
  uint32_t keyboard_page;
  char compatibility_name[kActRaiserLocalizationNameLength + 1];
  char utf8_name[kActRaiserLocalizationUnicodeNameCapacity];
  uint64_t revision;
} ActRaiserLocalizationNameEntryTracker;

bool ActRaiserLocalizationNameEntry_Capture(
    ActRaiserLocalizationNameEntryState *state,
    const uint8_t *wram, size_t wram_bytes);

/* Decode the USA keyboard selector's original 8x8 font tile using the live
 * BG3 character data and palette. Call only for a semantically owned keyboard;
 * this reads artwork, never infers UI ownership from VRAM. */
bool ActRaiserLocalizationNameEntry_CaptureCursor(
    uint32_t argb[kActRaiserLocalizationNameCursorPixels],
    uint16_t bg3_tile_base_words,
    const uint16_t *vram_words, size_t vram_word_count,
    const uint16_t *cgram_words, size_t cgram_word_count);

/* Locates the selected key in the last five logical lines of a normalized
 * pack message. Separators are ASCII spaces/tabs; each other grapheme is one
 * semantic key, including width-reserving inline-object clusters. */
bool ActRaiserLocalizationNameEntry_SelectedKeyEnd(
    const ActRaiserLocalizationNameEntryState *state,
    const char *utf8, size_t utf8_bytes, uint32_t *end_utf8_byte);

bool ActRaiserLocalizationNameEntry_SelectedKeyRange(
    const ActRaiserLocalizationNameEntryState *state,
    const char *utf8, size_t utf8_bytes,
    uint32_t *start_utf8_byte, uint32_t *end_utf8_byte);

void ActRaiserLocalizationNameEntryTracker_Init(
    ActRaiserLocalizationNameEntryTracker *tracker);
/* Applies native left/right edge wrapping to the pack keyboard page. This is
 * intentionally separate so the selected page can be resolved before a typed
 * key is mapped to its Unicode grapheme. */
bool ActRaiserLocalizationNameEntryTracker_SelectPage(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state,
    uint32_t page_count);
bool ActRaiserLocalizationNameEntryTracker_Synchronize(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state,
    const char *selected_key_utf8, size_t selected_key_bytes);
bool ActRaiserLocalizationNameEntryTracker_CopyDisplayName(
    const ActRaiserLocalizationNameEntryTracker *tracker,
    char *destination, size_t capacity);
/* While retail presentation is active, newly accepted keys come from its
 * ASCII compatibility buffer; retain any earlier Unicode graphemes. */
bool ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state);

#endif /* ACTRAISER_LOCALIZATION_NAME_ENTRY_H */
