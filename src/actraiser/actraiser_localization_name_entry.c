#include "actraiser/actraiser_localization_name_entry.h"

#include <stdio.h>
#include <string.h>

#include "deterministic_hash.h"
#include "localization/unicode_grapheme.h"
#include "snes_bgr555.h"

enum {
  kWramName = 0x0288,
  kWramSelectedColumn = 0x034B,
  kWramSelectedRow = 0x034C,
  kWramEnteredLength = 0x034D,
  kRequiredWramBytes = kWramEnteredLength + 1,
  kMaximumLogicalLines = 64,
};

bool ActRaiserLocalizationNameEntry_CopyNativeName(const uint8_t *wram,
                                                   size_t wram_bytes,
                                                   char *destination,
                                                   size_t capacity) {
  if (!destination || !capacity)
    return false;
  destination[0] = 0;
  if (!wram || wram_bytes < kWramName + kActRaiserLocalizationNameLength)
    return false;
  size_t length = 0;
  while (length < kActRaiserLocalizationNameLength) {
    const uint8_t byte = wram[kWramName + length];
    if (!byte || byte == 0xffu)
      break;
    if (byte < 0x20u || byte > 0x7eu || length + 1u >= capacity) {
      destination[0] = 0;
      return false;
    }
    destination[length++] = (char)byte;
  }
  destination[length] = 0;
  return length != 0;
}

bool ActRaiserLocalizationNameEntry_CaptureCursor(
    uint32_t argb[kActRaiserLocalizationNameCursorPixels],
    uint16_t bg3_tile_base_words,
    const uint16_t *vram_words, size_t vram_word_count,
    const uint16_t *cgram_words, size_t cgram_word_count) {
  /* $01:EFE6 composes glyph $3E with palette 0, no flips. Mode-1 BG3 is
   * 2bpp: each VRAM word holds both bitplanes for one eight-pixel row. */
  enum { kCursorTile = 0x3E, kVramWords = 0x8000 };
  if (!argb || !vram_words || vram_word_count < kVramWords ||
      !cgram_words || cgram_word_count < 4)
    return false;
  uint32_t palette[4] = {0};
  for (unsigned index = 1; index < 4; ++index) {
    const uint16_t color = cgram_words[index];
    palette[index] = UINT32_C(0xFF000000) |
        (uint32_t)ExpandColor5(color, 15) << 16 |
        (uint32_t)ExpandColor5(color >> 5, 15) << 8 |
        ExpandColor5(color >> 10, 15);
  }
  for (unsigned y = 0; y < 8; ++y) {
    const uint16_t planes = vram_words[
        (bg3_tile_base_words + kCursorTile * 8u + y) & (kVramWords - 1u)];
    for (unsigned x = 0; x < 8; ++x) {
      const unsigned shift = 7u - x;
      const unsigned pixel = ((planes >> shift) & 1u) |
          ((planes >> (shift + 7u)) & 2u);
      argb[y * 8u + x] = palette[pixel];
    }
  }
  return true;
}

static bool Valid(const ActRaiserLocalizationNameEntryState *state) {
  return state && state->struct_size >= sizeof(*state) &&
      state->abi_version == ACTRAISER_LOCALIZATION_NAME_ENTRY_ABI_VERSION &&
      state->entered_length <= kActRaiserLocalizationNameLength &&
      state->selected_row < kActRaiserLocalizationNameEntryRows &&
      state->selected_column < kActRaiserLocalizationNameEntryColumns &&
      state->revision && memchr(state->native_name, 0,
                                sizeof(state->native_name)) &&
      memchr(state->display_name, 0,
                                sizeof(state->display_name));
}

static uint64_t TrackerRevision(
    const ActRaiserLocalizationNameEntryTracker *tracker) {
  uint64_t revision = DETERMINISTIC_HASH_FNV1A64_OFFSET;
  revision = DeterministicHash_Fnv1a64(
      revision, tracker->utf8_name, strlen(tracker->utf8_name));
  revision = DeterministicHash_Fnv1a64(
      revision, tracker->compatibility_name,
      strlen(tracker->compatibility_name));
  revision = DeterministicHash_Fnv1a64(
      revision, &tracker->keyboard_page, sizeof(tracker->keyboard_page));
  return revision ? revision : 1u;
}

static bool TrackerValid(
    const ActRaiserLocalizationNameEntryTracker *tracker) {
  return tracker && tracker->struct_size >= sizeof(*tracker) &&
      tracker->abi_version ==
          ACTRAISER_LOCALIZATION_NAME_ENTRY_TRACKER_ABI_VERSION &&
      tracker->grapheme_count <= kActRaiserLocalizationNameLength &&
      memchr(tracker->utf8_name, 0, sizeof(tracker->utf8_name)) &&
      memchr(tracker->compatibility_name, 0,
             sizeof(tracker->compatibility_name));
}

static void SeedTracker(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state) {
  snprintf(tracker->utf8_name, sizeof(tracker->utf8_name), "%s",
           native_state->native_name);
  snprintf(tracker->compatibility_name,
           sizeof(tracker->compatibility_name), "%s",
           native_state->native_name);
  tracker->grapheme_count = native_state->entered_length;
  tracker->previous_length = native_state->entered_length;
  tracker->previous_row = native_state->selected_row;
  tracker->previous_column = native_state->selected_column;
  tracker->initialized = true;
  tracker->revision = TrackerRevision(tracker);
}

bool ActRaiserLocalizationNameEntry_Capture(
    ActRaiserLocalizationNameEntryState *state,
    const uint8_t *wram, size_t wram_bytes) {
  if (!state || !wram || wram_bytes < kRequiredWramBytes) return false;
  const uint8_t length = wram[kWramEnteredLength];
  const uint8_t row = wram[kWramSelectedRow];
  const uint8_t column = wram[kWramSelectedColumn];
  if (length > kActRaiserLocalizationNameLength ||
      row >= kActRaiserLocalizationNameEntryRows ||
      column >= kActRaiserLocalizationNameEntryColumns)
    return false;

  ActRaiserLocalizationNameEntryState captured = {
      .struct_size = sizeof(captured),
      .abi_version = ACTRAISER_LOCALIZATION_NAME_ENTRY_ABI_VERSION,
      .entered_length = length,
      .selected_row = row,
      .selected_column = column,
  };
  size_t written = 0;
  uint64_t revision = DETERMINISTIC_HASH_FNV1A64_OFFSET;
  revision = DeterministicHash_Fnv1a64Byte(revision, length);
  revision = DeterministicHash_Fnv1a64Byte(revision, row);
  revision = DeterministicHash_Fnv1a64Byte(revision, column);
  for (uint8_t index = 0; index < length; ++index) {
    const uint8_t byte = wram[kWramName + index];
    if (byte < 0x20 || byte > 0x7E) return false;
    captured.native_name[index] = (char)byte;
    captured.display_name[written++] = (char)byte;
    revision = DeterministicHash_Fnv1a64Byte(revision, byte);
  }
  /* U+2007 keeps the field non-empty for the generic value contract and
   * reserves a stable eight-character entry width without visible tofu. */
  static const char kFigureSpace[] = "\xE2\x80\x87";
  for (uint8_t index = length;
       index < kActRaiserLocalizationNameLength; ++index) {
    memcpy(captured.display_name + written, kFigureSpace,
           sizeof(kFigureSpace) - 1u);
    written += sizeof(kFigureSpace) - 1u;
  }
  captured.display_name[written] = 0;
  captured.revision = revision ? revision : 1u;
  *state = captured;
  return true;
}

bool ActRaiserLocalizationNameEntry_SelectedKeyEnd(
    const ActRaiserLocalizationNameEntryState *state,
    const char *utf8, size_t utf8_bytes, uint32_t *end_utf8_byte) {
  uint32_t start_utf8_byte = 0;
  return ActRaiserLocalizationNameEntry_SelectedKeyRange(
      state, utf8, utf8_bytes, &start_utf8_byte, end_utf8_byte);
}

bool ActRaiserLocalizationNameEntry_SelectedKeyRange(
    const ActRaiserLocalizationNameEntryState *state,
    const char *utf8, size_t utf8_bytes,
    uint32_t *start_utf8_byte, uint32_t *end_utf8_byte) {
  if (start_utf8_byte) *start_utf8_byte = 0;
  if (end_utf8_byte) *end_utf8_byte = 0;
  if (!Valid(state) || !utf8 || !utf8_bytes || !start_utf8_byte ||
      !end_utf8_byte ||
      utf8_bytes > UINT32_MAX)
    return false;

  size_t starts[kMaximumLogicalLines];
  size_t ends[kMaximumLogicalLines];
  size_t line_count = 0;
  size_t start = 0;
  for (size_t index = 0; index <= utf8_bytes; ++index) {
    if (index != utf8_bytes && utf8[index] != '\n') continue;
    if (line_count >= kMaximumLogicalLines) return false;
    starts[line_count] = start;
    ends[line_count] = index;
    ++line_count;
    start = index + 1u;
  }
  if (line_count < kActRaiserLocalizationNameEntryRows) return false;
  const size_t target_line =
      line_count - kActRaiserLocalizationNameEntryRows + state->selected_row;
  size_t offset = starts[target_line];
  uint8_t key = 0;
  bool found = false;
  uint32_t selected_start = 0;
  uint32_t selected_end = 0;
  while (offset < ends[target_line]) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(utf8, utf8_bytes, offset, NULL, &next) ||
        next <= offset || next > ends[target_line])
      return false;
    const bool separator = next == offset + 1u &&
        (utf8[offset] == ' ' || utf8[offset] == '\t');
    if (!separator) {
      if (key == state->selected_column) {
        selected_start = (uint32_t)offset;
        selected_end = (uint32_t)next;
        found = true;
      }
      if (key == UINT8_MAX) return false;
      ++key;
    }
    offset = next;
  }
  if (!found || key != kActRaiserLocalizationNameEntryColumns) return false;
  *start_utf8_byte = selected_start;
  *end_utf8_byte = selected_end;
  return true;
}

void ActRaiserLocalizationNameEntryTracker_Init(
    ActRaiserLocalizationNameEntryTracker *tracker) {
  if (!tracker) return;
  memset(tracker, 0, sizeof(*tracker));
  tracker->struct_size = sizeof(*tracker);
  tracker->abi_version =
      ACTRAISER_LOCALIZATION_NAME_ENTRY_TRACKER_ABI_VERSION;
}

bool ActRaiserLocalizationNameEntryTracker_SelectPage(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state,
    uint32_t page_count) {
  if (!TrackerValid(tracker) || !Valid(native_state) || !page_count)
    return false;
  if (!tracker->initialized) {
    SeedTracker(tracker, native_state);
    return true;
  }
  const uint32_t prior = tracker->keyboard_page;
  if (tracker->previous_column ==
          kActRaiserLocalizationNameEntryColumns - 1u &&
      native_state->selected_column == 0) {
    tracker->keyboard_page = (tracker->keyboard_page + 1u) % page_count;
  } else if (tracker->previous_column == 0 &&
             native_state->selected_column ==
                 kActRaiserLocalizationNameEntryColumns - 1u) {
    tracker->keyboard_page = tracker->keyboard_page
        ? tracker->keyboard_page - 1u : page_count - 1u;
  } else if (tracker->keyboard_page >= page_count) {
    tracker->keyboard_page = page_count - 1u;
  }
  if (tracker->keyboard_page != prior)
    tracker->revision = TrackerRevision(tracker);
  return true;
}

static bool RemoveLastGrapheme(
    ActRaiserLocalizationNameEntryTracker *tracker) {
  const size_t bytes = strlen(tracker->utf8_name);
  size_t offset = 0;
  size_t prior = 0;
  while (offset < bytes) {
    size_t next = 0;
    if (!ArUnicodeGrapheme_Next(
            tracker->utf8_name, bytes, offset, NULL, &next) || next <= offset)
      return false;
    prior = offset;
    offset = next;
  }
  tracker->utf8_name[prior] = 0;
  if (tracker->grapheme_count) --tracker->grapheme_count;
  return true;
}

bool ActRaiserLocalizationNameEntryTracker_Synchronize(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state,
    const char *selected_key_utf8, size_t selected_key_bytes) {
  if (!TrackerValid(tracker) || !Valid(native_state)) return false;
  if (!tracker->initialized) SeedTracker(tracker, native_state);

  const bool compatible_prefix =
      native_state->entered_length >= tracker->previous_length &&
      !memcmp(native_state->native_name, tracker->compatibility_name,
              tracker->previous_length);
  if (native_state->entered_length == tracker->previous_length + 1u &&
      compatible_prefix && selected_key_utf8 && selected_key_bytes &&
      tracker->grapheme_count < kActRaiserLocalizationNameLength) {
    size_t next = 0;
    const size_t used = strlen(tracker->utf8_name);
    if (!ArUnicodeGrapheme_Next(
            selected_key_utf8, selected_key_bytes, 0, NULL, &next) ||
        next != selected_key_bytes ||
        selected_key_bytes >= sizeof(tracker->utf8_name) - used)
      return false;
    memcpy(tracker->utf8_name + used, selected_key_utf8,
           selected_key_bytes);
    tracker->utf8_name[used + selected_key_bytes] = 0;
    ++tracker->grapheme_count;
  } else if (native_state->entered_length + 1u ==
                 tracker->previous_length &&
             !memcmp(native_state->native_name,
                     tracker->compatibility_name,
                     native_state->entered_length)) {
    if (!RemoveLastGrapheme(tracker)) return false;
  } else if (native_state->entered_length != tracker->previous_length ||
             strcmp(native_state->native_name,
                    tracker->compatibility_name)) {
    SeedTracker(tracker, native_state);
  }
  snprintf(tracker->compatibility_name,
           sizeof(tracker->compatibility_name), "%s",
           native_state->native_name);
  tracker->previous_length = native_state->entered_length;
  tracker->previous_row = native_state->selected_row;
  tracker->previous_column = native_state->selected_column;
  tracker->revision = TrackerRevision(tracker);
  return true;
}

bool ActRaiserLocalizationNameEntryTracker_CopyDisplayName(
    const ActRaiserLocalizationNameEntryTracker *tracker,
    char *destination, size_t capacity) {
  if (!TrackerValid(tracker) || !tracker->initialized || !destination ||
      !capacity)
    return false;
  const size_t name_bytes = strlen(tracker->utf8_name);
  static const char kFigureSpace[] = "\xE2\x80\x87";
  const size_t padding =
      kActRaiserLocalizationNameLength - tracker->grapheme_count;
  if (name_bytes + padding * (sizeof(kFigureSpace) - 1u) >= capacity)
    return false;
  memcpy(destination, tracker->utf8_name, name_bytes);
  size_t written = name_bytes;
  for (size_t index = 0; index < padding; ++index) {
    memcpy(destination + written, kFigureSpace, sizeof(kFigureSpace) - 1u);
    written += sizeof(kFigureSpace) - 1u;
  }
  destination[written] = 0;
  return true;
}

bool ActRaiserLocalizationNameEntryTracker_SynchronizeNative(
    ActRaiserLocalizationNameEntryTracker *tracker,
    const ActRaiserLocalizationNameEntryState *native_state) {
  if (!Valid(native_state)) return false;
  const size_t length = native_state->entered_length;
  return ActRaiserLocalizationNameEntryTracker_Synchronize(
      tracker, native_state,
      length ? native_state->native_name + length - 1u : NULL,
      length ? 1u : 0u);
}
