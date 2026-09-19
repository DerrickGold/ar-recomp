#include "actraiser/actraiser_localization_credits.h"

#include <stdio.h>
#include <string.h>
#include "localization/unicode_grapheme.h"
#include "snes_bgr555.h"

static bool PageMatches(unsigned page, uint16_t map_base,
                         const uint8_t *wram, const uint16_t *vram) {
  const uint8_t *source = wram + 0x4000 + page * 0x800;
  bool ink = false;
  for (unsigned i = 0; i < 1024; ++i) {
    const uint16_t word = source[i * 2] | (uint16_t)source[i * 2 + 1] << 8;
    if ((word & ~0x4ffu) || vram[(map_base + i) & 0x7fff] != word)
      return false;
    if ((i < 32 || i >= 27 * 32) && word != 0x10) return false;
    ink |= i < 28 * 32 && (word & 0xff) != 0x10 && (word & 0xff) != 0x42;
  }
  return ink;
}

static bool ResolvePage(ActRaiserLocalizationCredits *credits,
                         ActRaiserLocalizationComposeTextResolver resolve,
                         void *context) {
  char id[32], source[kActRaiserCreditsTextCapacity];
  if (credits->page < 15)
    snprintf(id, sizeof(id), "credits.page_%02u", credits->page);
  else
    snprintf(id, sizeof(id), "%s", credits->page == 17 ? "credits.the_end"
        : credits->page == 18 ? "credits.best_player" : "credits.game_over");
  size_t bytes = 0;
  uint32_t clusters = 0;
  uint8_t objects = 0;
  memset(&credits->language, 0, sizeof(credits->language));
  credits->bidi.count = 0;
  uint8_t boundaries[AR_TEXT_BOUNDARY_BYTES(sizeof(source))] = {0};
  if (!resolve(context, id, source, sizeof(source), &bytes, &clusters,
                &credits->revision, NULL, 0, &objects, boundaries,
                &credits->language, &credits->bidi, NULL, NULL, 0) ||
      !ArLocalizationTextLanguage_IsValid(&credits->language) ||
      objects || bytes >= sizeof(source) || !credits->revision ||
      !ArTextBidiSpans_FitSource(&credits->bidi, source, bytes))
    return false;
  unsigned lines = bytes ? 1 : 0;
  for (size_t i = 0; i < bytes; ++i)
    if (source[i] == '\n') {
      if (!ArTextBoundary_Get(boundaries, i)) return false;
      ++lines;
    }
  if (lines > 6) return false;
  credits->bytes = credits->clusters = credits->accent_end = 0;
  memset(credits->boundaries, 0, sizeof(credits->boundaries));
  if (!bytes) return true; /* Authored empty page is an intentional erase. */
  // Put each authored line on a centered four-cell pitch. Padding newlines
  // are structural rows, not glyph work or authored timing/reveal events.
  const unsigned first = 11 - 2 * (lines - 1);
  if (bytes + first + (lines - 1) * 3 >= sizeof(credits->text)) return false;
  for (uint16_t s = 0; s < credits->bidi.count; ++s) {
    ArTextBidiSpan *span = &credits->bidi.spans[s];
    uint32_t before = first, inside = 0;
    for (size_t i = 0; i < bytes; ++i) if (source[i] == '\n') {
      if (i < span->start) before += 3;
      else if (i < span->end) inside += 3;
    }
    span->start += before; span->end += before + inside;
  }
  for (unsigned i = 0; i < first; ++i) credits->text[credits->bytes++] = '\n';
  for (size_t i = 0; i < bytes; ++i) {
    credits->text[credits->bytes++] = source[i];
    if (source[i] == '\n')
      for (unsigned j = 0; j < 3; ++j) credits->text[credits->bytes++] = '\n';
  }
  credits->text[credits->bytes] = 0;
  for (size_t i = 0; i < credits->bytes; ++i)
    ArTextBoundary_Set(credits->boundaries, i, credits->text[i] == '\n');
  for (size_t offset = 0; offset < credits->bytes;) {
    size_t next;
    uint32_t scalar;
    if (!ArUnicodeGrapheme_Next(credits->text, credits->bytes, offset, &scalar, &next))
      return false;
    // Preserve the native heading's first-letter accent after its decorative
    // dash. Other scripts use the first grapheme, never a single UTF-8 byte.
    if (!credits->accent_end && scalar != '\n' && scalar != ' ' &&
        scalar != '-' && scalar != 0x2013 && scalar != 0x2014)
      credits->accent_end = (uint32_t)next;
    ++credits->clusters;
    offset = next;
  }
  return true;
}

void ActRaiserLocalizationCredits_Append(
    ActRaiserLocalizationCredits *credits, ArLocalizationFrame *frame,
    ArTextCellDestination destination,
    uint8_t map_group, uint8_t map_number, uint16_t tile_base_words,
    const uint8_t *wram, size_t wram_bytes,
    const uint16_t *vram, size_t vram_words,
    const uint16_t *cgram, size_t cgram_words,
    ActRaiserLocalizationComposeTextResolver resolve, void *context) {
  if (!credits) return;
  if (!frame || !resolve || map_group != 8 || map_number != 1 ||
      tile_base_words != 0x5000 || !wram || wram_bytes < 0xe000 ||
      !vram || vram_words < 0x8000 || !cgram || cgram_words < 16 ||
      destination.background != 3) {
    credits->resolved = credits->valid = false;
    return;
  }
  int page = -1;
  if (credits->resolved && credits->page < kActRaiserCreditsPageCount &&
      PageMatches(credits->page, destination.tilemap_base_words, wram, vram))
    page = credits->page;
  else for (unsigned i = 0; i < kActRaiserCreditsPageCount; ++i) {
    if (!PageMatches(i, destination.tilemap_base_words, wram, vram)) continue;
    if (page >= 0) { page = -1; break; } // Ambiguous/corrupt page inventory.
    page = (int)i;
  }
  if (page < 0 || page == 15 || page == 16) {
    credits->resolved = credits->valid = false;
    return;
  }
  if (!credits->resolved || credits->page != page) {
    credits->page = (uint8_t)page;
    credits->valid = ResolvePage(credits, resolve, context);
    credits->resolved = true;
  }
  if (!credits->valid) return;
  if (credits->bidi.count > kArTextMaximumBidiSpans - frame->bidi.count) return;
  const ArLocalizationTextGrid grid = {
    .rule_count = 1, .row_height = 4, .crop_rows = 1, .center_rows = 1,
    .rules = {{.first_line = 0, .last_line = 25, .field_count = 1,
               .cell_count = 1, .cells = {{0,32,kArTextHorizontalAlignment_Center,0,0,0}}}},
  };
  if (!ArLocalizationFrame_AddTextWithGrid(frame, 300, destination,
          (ArTextCellRegion){0,1,32,26}, credits->text, credits->bytes,
          credits->clusters, credits->clusters, credits->revision, credits->language.direction,
          13, &grid, credits->boundaries, NULL, 0, NULL, 0) ||
      !ArLocalizationFrame_SetTextLanguage(frame, &credits->language) ||
      !ArLocalizationFrame_SetTextBidiSpans(frame, &credits->bidi)) return;
  ArLocalizationTextSnapshot *snapshot = &frame->snapshots[frame->snapshot_count-1];
  snapshot->style_id = kArTextStyle_RetailPaletteBands;
  const uint16_t body = cgram[1], accent = cgram[5];
  snapshot->body_rgb = snapshot->band_rgb =
      (uint32_t)ExpandColor5(body,15)<<16 | (uint32_t)ExpandColor5(body>>5,15)<<8 |
      ExpandColor5(body>>10,15);
  bool accented = false;
  for (unsigned i = 0; i < 28 * 32; ++i)
    accented |= (vram[(destination.tilemap_base_words+i)&0x7fff] & 0x400) != 0;
  if (accented) {
    snapshot->accent_end_utf8_byte = credits->accent_end;
    snapshot->accent_rgb = (uint32_t)ExpandColor5(accent,15)<<16 |
        (uint32_t)ExpandColor5(accent>>5,15)<<8 | ExpandColor5(accent>>10,15);
  }
}
