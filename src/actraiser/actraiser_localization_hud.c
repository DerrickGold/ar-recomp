#include "actraiser/actraiser_localization_hud.h"
#include "actraiser/actraiser_localization_art.h"
#include "actraiser/actraiser_localization_style.h"

#include <string.h>
#include "snes_bgr555.h"

typedef struct HudField {
  const char *id;
  ArTextCellRegion region;
  uint8_t source_column, source_row, source_count;
  uint16_t tiles[6];
  ArLocalizationTextLayoutKind layout;
  uint8_t left_inset, right_inset;
} HudField;

/* $02:8E7E template -> $7F:B040; $00:A4D6 adds the ENEMY strip at
 * $7F:B0C0. Those graphics contain packed partial letters, not ASCII glyphs.
 * ACT's asymmetric bookends continue into the adjacent letter tiles. Capture
 * their complete pixels before claiming the six-cell label/frame together.
 * Right-align proportional labels at the native ink edge, not at the tile's
 * far edge. This retains the six-pixel TIME gutter and four-pixel gap
 * before health bars, without stretching glyphs or moving native graphics. */
/* Named so the value rows below can say which template guards them. They were
 * bare subscripts, which quietly bind a row to whatever field happens to sit
 * at that position -- and inserting the simulation fields is exactly the edit
 * that would have rebound them. */
enum {
  kHudFieldActLabel,
  kHudFieldTimeLabel,
  kHudFieldScoreLabel,
  kHudFieldPlayerLabel,
  kHudFieldEnemyLabel,
  kHudFieldSimContextLabel,
  kHudFieldSimAngelLabel,
  kHudFieldSimSpLabel,
  kHudFieldCount,
};
_Static_assert(kHudFieldCount == kActRaiserLocalizationHudLabels,
               "every named HUD field must have a row in kFields");

static const HudField kFields[kActRaiserLocalizationHudLabels] = {
  {"action.hud.act_label", {0, 1, 6, 1}, 0, 1, 6, {34,35,36,37,38,39},
   kArLocalizationTextLayout_FramedLabel, 5, 4},
  {"action.hud.time_label", {11, 1, 4, 1}, 11, 1, 4, {1,2,3,4},
   kArLocalizationTextLayout_RightAlignedLabel, 0, 6},
  {"action.hud.score_label", {21, 1, 5, 1}, 21, 1, 5, {5,6,7,8,4},
   kArLocalizationTextLayout_LeftAlignedLabel, 0, 0},
  {"action.hud.player_label", {0, 2, 6, 1}, 0, 2, 6, {9,10,11,12,13,14},
   kArLocalizationTextLayout_RightAlignedLabel, 5, 4},
  {"action.hud.enemy_label", {0, 3, 6, 1}, 0, 3, 6, {15,16,17,18,19,20},
   kArLocalizationTextLayout_RightAlignedLabel, 5, 4},
  /* The simulation and Sky Palace status bar reuses the action bar's shape:
   * the same bookended context label over the same six-cell name/bar row, so
   * these mirror their action counterparts cell for cell. One context label
   * covers both scenes -- town and palace draw the identical tiles. SP is a
   * plain two-cell ASCII label rather than the packed artwork the others use,
   * and leads its value the way SCORE leads the score. */
  {"sim_sky.hud.context_label", {0, 1, 6, 1}, 0, 1, 6, {34,91,92,93,94,39},
   kArLocalizationTextLayout_FramedLabel, 5, 4},
  {"sim_sky.hud.angel_label", {0, 2, 6, 1}, 0, 2, 6, {21,22,23,24,25,26},
   kArLocalizationTextLayout_RightAlignedLabel, 5, 4},
  {"sim_sky.hud.sp_label", {21, 2, 2, 1}, 21, 2, 2, {83,80},
   kArLocalizationTextLayout_CenteredLabel, 0, 0},
};

/* Final fields from the native BCD formatter, guarded by their owning labels.
 */
static const struct {
  const char *id;
  uint8_t column, row, count, owner, left_inset;
  ArLocalizationTextLayoutKind layout;
} kNumbers[] = {
    {"action.hud.lives_value", 8, 1, 2, kHudFieldActLabel, 0,
     kArLocalizationTextLayout_SingleLineLabel},
    {"action.hud.time_value", 15, 1, 3, kHudFieldTimeLabel, 1,
     kArLocalizationTextLayout_SingleLineLabel},
    {"action.hud.score_value", 26, 1, 5, kHudFieldScoreLabel, 0,
     kArLocalizationTextLayout_RightAlignedLabel},
    /* Simulation and Sky Palace: population above SP, both written by the same
     * native formatter as "current/maximum". SP is one cell narrower and the
     * formatter indents it with a leading blank, which the rasterizer crops
     * away -- so the indent is stated as an inset instead. Anchoring to the
     * trailing edge would not do: these glyphs are narrower than the native
     * cell pitch, so a right-aligned value starts well inside where the
     * original begins. */
    {"sim_sky.hud.population_value", 23, 1, 8, kHudFieldSimContextLabel, 0,
     kArLocalizationTextLayout_SingleLineLabel},
    {"sim_sky.hud.sp_value", 23, 2, 8, kHudFieldSimSpLabel, 8,
     kArLocalizationTextLayout_SingleLineLabel},
};

bool ActRaiserLocalizationHud_Presentation(
    const char *id, ActRaiserLocalizationHudPresentation *out) {
  for (size_t i = 0; i < kActRaiserLocalizationHudLabels; ++i) {
    const HudField *field = &kFields[i];
    if (!strcmp(id, field->id)) {
      *out = (ActRaiserLocalizationHudPresentation){
          field->region,     field->layout,     7, 1,
          field->left_inset, field->right_inset};
      return true;
    }
  }
  for (size_t i = 0; i < sizeof(kNumbers) / sizeof(kNumbers[0]); ++i) {
    if (!strcmp(id, kNumbers[i].id)) {
      *out = (ActRaiserLocalizationHudPresentation){
          {kNumbers[i].column, kNumbers[i].row, kNumbers[i].count, 1},
          kNumbers[i].layout,
          8,
          0,
          kNumbers[i].left_inset,
          1};
      return true;
    }
  }
  return false;
}

static uint16_t Word(const uint16_t *vram, uint16_t base, unsigned row, unsigned col) {
  return vram[(base + row * 32 + col) & 0x7fff];
}

static bool Matches(const HudField *field, const uint16_t *vram, uint16_t base) {
  for (unsigned i = 0; i < field->source_count; ++i)
    if ((Word(vram, base, field->source_row, field->source_column + i) & 0x3ff) != field->tiles[i])
      return false;
  return true;
}

/* Native strip coordinates: left ornament x=5..12, right x=37..43.
 * Each spans two tiles; cropping excludes every original letter pixel but
 * retains the unequal top/bottom strokes, shadow ink and transparent row. */
void ActRaiserLocalizationHud_CapturePalette(
    ActRaiserTextPalette *palette, uint16_t map_base, const uint16_t *vram,
    size_t vram_count, const uint16_t *cgram, size_t cgram_count) {
  if (!palette || !vram || vram_count < 0x8000 || !cgram || cgram_count < 32)
    return;
  for (size_t i = 0; i < kActRaiserLocalizationHudLabels; ++i) {
    if (!Matches(&kFields[i], vram, map_base))
      continue;
    const uint16_t word =
        Word(vram, map_base, kFields[i].source_row, kFields[i].source_column);
    ActRaiserTextPalette_SetHud(palette, cgram + ((word >> 10) & 7) * 4);
    return;
  }
}

static bool CaptureFrameEnd(ArLocalizationArtwork *art, unsigned column,
                            unsigned width, uint16_t map_base, uint16_t tile_base,
                            const uint16_t *vram, size_t vram_count,
                            const uint16_t *cgram, size_t cgram_count) {
  const uint16_t tiles[] = {Word(vram, map_base, 1, column),
                            Word(vram, map_base, 1, column + 1)};
  ArLocalizationArtwork decoded;
  if (!ActRaiserLocalizationArt_Capture(&decoded, tile_base, tiles, 2,
                                        vram, vram_count, cgram, cgram_count))
    return false;
  *art = (ArLocalizationArtwork){.width = width, .height = 8, .valid = true};
  for (unsigned row = 0; row < 8; ++row)
    memcpy(&art->argb[row * width], &decoded.argb[row * 16 + 5],
           width * sizeof(art->argb[0]));
  return true;
}

static void
Append(ArLocalizationFrame *frame, uint32_t id,
       ArTextCellDestination destination, ArTextCellRegion region,
       const char *text, size_t bytes, uint32_t clusters, uint64_t revision,
       const ArLocalizationTextLanguage *language, const ArTextBidiSpans *bidi,
       const ActRaiserTextStylePlan *styles, const ActRaiserTextPalette *inks,
       uint16_t native_word, const uint16_t *cgram, bool number,
       ArLocalizationTextLayoutKind layout, uint8_t left_inset,
       uint8_t right_inset) {
  const uint8_t slot = frame->snapshot_count;
  if (bidi && bidi->count > kArTextMaximumBidiSpans - frame->bidi.count) return;
  if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
          frame, id, destination, region, text, bytes, clusters, clusters,
          revision, language->direction, number ? 8 : 7,
          layout,
          NULL, 0, NULL, 0) ||
      !ArLocalizationFrame_SetTextLanguage(frame, language) ||
      (bidi && !ArLocalizationFrame_SetTextBidiSpans(frame, bidi))) return;
  const unsigned palette = ((native_word >> 10) & 7) * 4;
  ArLocalizationTextSnapshot *snapshot = &frame->snapshots[slot];
  ActRaiserLocalizationStyle_Ordinary(snapshot, cgram + palette);
  /* A bookended label carries its shading in the artwork ends, so its text is
   * one flat colour. That is a property of the framed shape, not of which
   * label happens to be first in the table. */
  if (layout == kArLocalizationTextLayout_FramedLabel)
    snapshot->band_rgb = snapshot->body_rgb;
  snapshot->italic = number;
  /* Native letters start on scanline one; all ten digits use scanline zero. */
  snapshot->top_inset_pixels = number ? 0 : 1;
  snapshot->left_inset_pixels = left_inset;
  snapshot->right_inset_pixels = right_inset;
  if (styles) {
    ActRaiserTextPalette field_inks = *inks;
    ActRaiserTextPalette_SetHud(&field_inks, cgram + palette);
    (void)ActRaiserTextStyle_Publish(styles, 0, &field_inks, frame);
  }
}

void ActRaiserLocalizationHud_Append(
    ActRaiserLocalizationHud *hud, ArLocalizationFrame *frame,
    ArTextCellDestination destination, uint16_t tile_base_words,
    const uint16_t *vram, size_t vram_count, const uint16_t *cgram,
    size_t cgram_count, ActRaiserLocalizationComposeTextResolver resolve,
    ActRaiserLocalizationFieldResolver resolve_value, void *context) {
  if (!hud || !frame || !vram || vram_count < 0x8000 ||
      !cgram || cgram_count < 32 || !resolve) return;
  ActRaiserTextPalette inks;
  ActRaiserTextPalette_Capture(&inks, cgram, cgram_count);
  if (!hud->resolved) {
    memset(hud, 0, sizeof(*hud));
    for (unsigned i = 0; i < kActRaiserLocalizationHudLabels; ++i) {
      ActRaiserLocalizationHudLabel *label = &hud->labels[i];
      char error[256];
      label->valid =
          resolve(context, kFields[i].id, &label->text, error, sizeof(error)) &&
          !label->text.inline_object_count &&
          ArLocalizationTextLanguage_IsValid(&label->text.language) &&
          label->text.utf8_bytes < 512 &&
          !label->text.utf8[label->text.utf8_bytes] &&
          ArTextBidiSpans_FitSource(&label->text.bidi, label->text.utf8,
                                    label->text.utf8_bytes) &&
          label->text.source_revision &&
          ((label->text.utf8_bytes != 0) == (label->text.cluster_count != 0));
    }
    hud->resolved = true;
  }
  const uint16_t base = destination.tilemap_base_words;
  for (unsigned i = 0; i < kActRaiserLocalizationHudLabels; ++i) {
    const HudField *field = &kFields[i];
    const ActRaiserLocalizationHudLabel *label = &hud->labels[i];
    if (label->valid && Matches(field, vram, base)) {
      if (field->layout == kArLocalizationTextLayout_FramedLabel &&
          (!CaptureFrameEnd(&frame->artwork[kArLocalizationArtwork_LabelFrameLeft],
                            0, 8, base, tile_base_words, vram, vram_count, cgram, cgram_count) ||
           !CaptureFrameEnd(&frame->artwork[kArLocalizationArtwork_LabelFrameRight],
                            4, 7, base, tile_base_words, vram, vram_count, cgram, cgram_count)))
        continue;
      Append(frame, 200 + i, destination, field->region, label->text.utf8,
             label->text.utf8_bytes, label->text.cluster_count,
             label->text.source_revision, &label->text.language,
             &label->text.bidi, &label->text.styles, &inks,
             Word(vram, base, field->source_row, field->source_column), cgram,
             false, field->layout, field->left_inset, field->right_inset);
    }
  }
  for (unsigned i = 0; i < sizeof(kNumbers) / sizeof(kNumbers[0]); ++i) {
    if (!Matches(&kFields[kNumbers[i].owner], vram, base))
      continue;
    char digits[9] = {0};
    bool valid = true;
    uint64_t revision = 1;
    uint16_t ink_word = 0;
    for (unsigned j = 0; j < kNumbers[i].count; ++j) {
      const uint16_t word =
          Word(vram, base, kNumbers[i].row, kNumbers[i].column + j);
      const uint16_t tile = word & 0x3ff;
      /* The separator is ink like the digits are: it comes from the same
       * formatter and must move with them, or a re-rendered "334" would sit
       * beside a native "/0334". */
      if ((tile >= 0x30 && tile <= 0x39) || tile == 0x2f) {
        digits[j] = (char)tile;
        ink_word = word;
      } else if (tile == 0 || tile == 0x20 || tile == 0x7c) digits[j] = ' ';
      else valid = false;
      revision = revision * 257 + word;
    }
    if (valid && ink_word && resolve_value) {
      ActRaiserLocalizationHudLabel *field = &hud->values[i];
      if (field->native_revision != revision) {
        char error[256];
        field->valid = resolve_value(context, kNumbers[i].id, digits,
                                     &field->text, error, sizeof(error)) &&
                       !field->text.inline_object_count;
        field->native_revision = revision;
      }
      if (!field->valid)
        continue;
      Append(frame, 210 + i, destination,
             (ArTextCellRegion){kNumbers[i].column, kNumbers[i].row,
                                kNumbers[i].count, 1},
             field->text.utf8, field->text.utf8_bytes,
             field->text.cluster_count, field->text.source_revision,
             &field->text.language, &field->text.bidi, &field->text.styles,
             &inks, ink_word, cgram, true, kNumbers[i].layout,
             kNumbers[i].left_inset, 1);
    } else if (valid && ink_word && !resolve_value)
      Append(frame, 210 + i, destination,
             (ArTextCellRegion){kNumbers[i].column, kNumbers[i].row,
                                kNumbers[i].count, 1},
             digits, kNumbers[i].count, kNumbers[i].count, revision,
             &(ArLocalizationTextLanguage){
                 .locale = "en-US", .direction = kArTextDirection_LeftToRight},
             NULL, NULL, &inks, ink_word, cgram, true, kNumbers[i].layout,
             kNumbers[i].left_inset, 1);
  }
}
