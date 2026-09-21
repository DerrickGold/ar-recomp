#include "actraiser/actraiser_localization_credits.h"

#include <stdio.h>
#include <string.h>
#include "localization/unicode_grapheme.h"
#include "snes_bgr555.h"

static bool SelectedPageIntact(unsigned page, uint16_t map_base,
                               const uint8_t *wram, const uint16_t *vram) {
  const uint8_t *source = wram + 0x4000 + page * 0x800;
  bool ink = false;
  /* AEEB uploads 27 rows, although AB30 copies 32 rows to staging. Rows
   * 27..31 are outside our text region and must not decide page ownership. */
  for (unsigned i = 0; i < 27 * 32; ++i) {
    const uint16_t word = source[i * 2] | (uint16_t)source[i * 2 + 1] << 8;
    if ((word & ~0x4ffu) || vram[(map_base + i) & 0x7fff] != word)
      return false;
    if (i < 32 && word != 0x10) return false;
    ink |= (word & 0xff) != 0x10 && (word & 0xff) != 0x42;
  }
  return ink;
}

static bool ResolvePage(ActRaiserLocalizationCredits *credits,
                         ActRaiserLocalizationComposeTextResolver resolve,
                         void *context) {
  char id[32];
  if (credits->page < 15)
    snprintf(id, sizeof(id), "credits.page_%02u", credits->page);
  else
    snprintf(id, sizeof(id), "%s", credits->page == 17 ? "credits.the_end"
        : credits->page == 18 ? "credits.best_player" : "credits.game_over");
  memset(&credits->text, 0, sizeof(credits->text));
  return resolve(context, id, &credits->text, NULL, 0) &&
         ActRaiserLocalizationCredits_PrepareText(&credits->text,
                                                  &credits->accent_end);
}

bool ActRaiserLocalizationCredits_PrepareText(ActRaiserResolvedText *text,
                                              uint32_t *accent_end) {
  char source[kActRaiserCreditsTextCapacity];
  if (!text || !accent_end ||
      !ArLocalizationTextLanguage_IsValid(&text->language) ||
      text->inline_object_count || text->utf8_bytes >= sizeof(source) ||
      !text->source_revision ||
      !ArTextBidiSpans_FitSource(&text->bidi, text->utf8, text->utf8_bytes))
    return false;
  const size_t bytes = text->utf8_bytes;
  memcpy(source, text->utf8, bytes + 1);
  unsigned lines = bytes ? 1 : 0;
  for (size_t i = 0; i < bytes; ++i)
    if (source[i] == '\n') {
      if (!ArTextBoundary_Get(text->structural_boundaries, i))
        return false;
      ++lines;
    }
  if (lines > 6) return false;
  text->utf8_bytes = text->cluster_count = (*accent_end) = 0;
  memset(text->structural_boundaries, 0, sizeof(text->structural_boundaries));
  if (!bytes) return true; /* Authored empty page is an intentional erase. */
  // Put each authored line on a centered four-cell pitch. Padding newlines
  // are structural rows, not glyph work or authored timing/reveal events.
  const unsigned first = 11 - 2 * (lines - 1);
  if (bytes + first + (lines - 1) * 3 >= sizeof(text->utf8))
    return false;
  for (uint16_t s = 0; s < text->bidi.count; ++s) {
    ArTextBidiSpan *span = &text->bidi.spans[s];
    uint32_t before = first, inside = 0;
    for (size_t i = 0; i < bytes; ++i) if (source[i] == '\n') {
      if (i < span->start) before += 3;
      else if (i < span->end) inside += 3;
    }
    span->start += before; span->end += before + inside;
  }
  /* Relocate styles through the same inserted rows, from the end so source
   * offsets stay valid until each edit has been applied. */
  for (size_t i = bytes; i-- > 0;)
    if (source[i] == '\n')
      ActRaiserTextStyle_Edit(&text->styles, (uint32_t)i + 1, 0, 3);
  ActRaiserTextStyle_Edit(&text->styles, 0, 0, first);
  for (unsigned i = 0; i < first; ++i)
    text->utf8[text->utf8_bytes++] = '\n';
  for (size_t i = 0; i < bytes; ++i) {
    text->utf8[text->utf8_bytes++] = source[i];
    if (source[i] == '\n')
      for (unsigned j = 0; j < 3; ++j)
        text->utf8[text->utf8_bytes++] = '\n';
  }
  text->utf8[text->utf8_bytes] = 0;
  for (size_t i = 0; i < text->utf8_bytes; ++i)
    ArTextBoundary_Set(text->structural_boundaries, i, text->utf8[i] == '\n');
  for (size_t offset = 0; offset < text->utf8_bytes;) {
    size_t next;
    uint32_t scalar;
    if (!ArUnicodeGrapheme_Next(text->utf8, text->utf8_bytes, offset, &scalar,
                                &next))
      return false;
    // Preserve the native heading's first-letter accent after its decorative
    // dash. Other scripts use the first grapheme, never a single UTF-8 byte.
    if (!(*accent_end) && scalar != '\n' && scalar != ' ' && scalar != '-' &&
        scalar != 0x2013 && scalar != 0x2014)
      (*accent_end) = (uint32_t)next;
    ++text->cluster_count;
    offset = next;
  }
  return true;
}

const ArTextCellRegion kActRaiserCreditsRegion = {0, 1, 32, 26};

bool ActRaiserLocalizationCredits_AddText(ArLocalizationFrame *frame,
                                          ArTextCellDestination destination,
                                          const ActRaiserResolvedText *text) {
  const ArLocalizationTextGrid grid = {
      .rule_count = 1,
      .row_height = 4,
      .crop_rows = 1,
      .center_rows = 1,
      .rules = {{.first_line = 0,
                 .last_line = 25,
                 .field_count = 1,
                 .cell_count = 1,
                 .cells = {{0, 32, kArTextHorizontalAlignment_Center, 0, 0,
                            0}}}},
  };
  return ArLocalizationFrame_AddTextWithGrid(
             frame, 300, destination, kActRaiserCreditsRegion, text->utf8,
             text->utf8_bytes, text->cluster_count, text->cluster_count,
             text->source_revision, text->language.direction,
             kActRaiserCreditsFontPixels, &grid, text->structural_boundaries,
             NULL, 0, NULL, 0) &&
         ArLocalizationFrame_SetTextLanguage(frame, &text->language) &&
         ArLocalizationFrame_SetTextBidiSpans(frame, &text->bidi);
}

void ActRaiserLocalizationCredits_Append(
    ActRaiserLocalizationCredits *credits, ArLocalizationFrame *frame,
    ArTextCellDestination destination, int presented_page,
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
  const int page = presented_page;
  if (page < 0 || page >= kActRaiserCreditsPageCount || page == 15 ||
      page == 16 || !SelectedPageIntact(
          (unsigned)page, destination.tilemap_base_words, wram, vram)) {
    credits->resolved = credits->valid = false;
    return;
  }
  if (!credits->resolved || credits->page != page) {
    credits->page = (uint8_t)page;
    credits->valid = ResolvePage(credits, resolve, context);
    credits->resolved = true;
  }
  if (!credits->valid) return;
  if (credits->text.bidi.count > kArTextMaximumBidiSpans - frame->bidi.count)
    return;
  if (!ActRaiserLocalizationCredits_AddText(frame, destination, &credits->text))
    return;
  ArLocalizationTextSnapshot *snapshot = &frame->snapshots[frame->snapshot_count-1];
  snapshot->style_id = kArTextStyle_RetailPaletteBands;
  const uint16_t body = cgram[1], accent = cgram[5];
  snapshot->body_rgb = snapshot->band_rgb =
      (uint32_t)ExpandColor5(body,15)<<16 | (uint32_t)ExpandColor5(body>>5,15)<<8 |
      ExpandColor5(body>>10,15);
  bool accented = false;
  for (unsigned i = 0; i < 27 * 32; ++i)
    accented |= (vram[(destination.tilemap_base_words+i)&0x7fff] & 0x400) != 0;
  if (accented && !credits->text.styles.authored) {
    snapshot->accent_end_utf8_byte = credits->accent_end;
    snapshot->accent_rgb = (uint32_t)ExpandColor5(accent,15)<<16 |
        (uint32_t)ExpandColor5(accent>>5,15)<<8 | ExpandColor5(accent>>10,15);
  }
  ActRaiserTextPalette inks;
  ActRaiserTextPalette_Capture(&inks, cgram, cgram_words);
  (void)ActRaiserTextStyle_Publish(&credits->text.styles, 0, &inks, frame);
}
