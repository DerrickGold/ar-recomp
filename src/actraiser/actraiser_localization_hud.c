#include "actraiser/actraiser_localization_hud.h"
#include "actraiser/actraiser_localization_art.h"

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

static uint16_t Word(const uint16_t *vram, uint16_t base, unsigned row, unsigned col) {
  return vram[(base + row * 32 + col) & 0x7fff];
}

static uint32_t Rgb(uint16_t color) {
  return (uint32_t)ExpandColor5(color, 15) << 16 |
         (uint32_t)ExpandColor5(color >> 5, 15) << 8 |
         ExpandColor5(color >> 10, 15);
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

static void Append(ArLocalizationFrame *frame, uint32_t id,
                   ArTextCellDestination destination, ArTextCellRegion region,
                   const char *text, size_t bytes, uint32_t clusters,
                   uint64_t revision, ArTextDirection direction,
                   uint16_t native_word, const uint16_t *cgram, bool number,
                   ArLocalizationTextLayoutKind layout,
                   uint8_t left_inset, uint8_t right_inset) {
  const uint8_t slot = frame->snapshot_count;
  if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
          frame, id, destination, region, text, bytes, clusters, clusters,
          revision, direction, 7,
          layout,
          NULL, 0, NULL, 0)) return;
  const unsigned palette = ((native_word >> 10) & 7) * 4;
  ArLocalizationTextSnapshot *snapshot = &frame->snapshots[slot];
  snapshot->style_id = kArTextStyle_RetailPaletteBands;
  snapshot->band_rgb = Rgb(cgram[palette + 2]);
  snapshot->body_rgb = Rgb(cgram[palette + 3]);
  /* Retail glyph tiles are three inks, not two: colour 1 is the shade drawn
   * beside every stroke. Reading only the band pair left the replacement
   * lighter than the lettering it stands in for. */
  snapshot->shadow_rgb = Rgb(cgram[palette + 1]);
  snapshot->shadow_enabled = true;
  /* A bookended label carries its shading in the artwork ends, so its text is
   * one flat colour. That is a property of the framed shape, not of which
   * label happens to be first in the table. */
  if (layout == kArLocalizationTextLayout_FramedLabel)
    snapshot->band_rgb = snapshot->body_rgb;
  snapshot->italic = number;
  snapshot->top_inset_pixels = 1; /* Retail glyph tiles leave scanline zero blank. */
  snapshot->left_inset_pixels = left_inset;
  snapshot->right_inset_pixels = right_inset;
}

void ActRaiserLocalizationHud_Append(
    ActRaiserLocalizationHud *hud, ArLocalizationFrame *frame,
    ArTextCellDestination destination, ArTextDirection direction,
    uint16_t tile_base_words,
    const uint16_t *vram, size_t vram_count,
    const uint16_t *cgram, size_t cgram_count,
    ActRaiserLocalizationComposeTextResolver resolve, void *context) {
  if (!hud || !frame || !vram || vram_count < 0x8000 ||
      !cgram || cgram_count < 32 || !resolve) return;
  if (!hud->resolved) {
    memset(hud, 0, sizeof(*hud));
    for (unsigned i = 0; i < kActRaiserLocalizationHudLabels; ++i) {
      ActRaiserLocalizationHudLabel *label = &hud->labels[i];
      uint8_t objects = 0;
      char error[256];
      label->valid = resolve(context, kFields[i].id, label->text, sizeof(label->text),
          &label->bytes, &label->clusters, &label->revision, NULL, 0, &objects,
          error, sizeof(error)) && !objects &&
          label->bytes < sizeof(label->text) && !label->text[label->bytes] &&
          label->revision && ((label->bytes != 0) == (label->clusters != 0));
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
      Append(frame, 200 + i, destination, field->region,
             label->text, label->bytes, label->clusters, label->revision,
             direction, Word(vram, base, field->source_row, field->source_column), cgram, false,
             field->layout, field->left_inset, field->right_inset);
    }
  }
  /* These are the final formatted digits actually uploaded by the native
   * BCD writer. Preserve its zero padding and blank-leading score policy.
   * Lives/timer face their preceding icon/label; only the variable-length
   * score is right-aligned. Require the corresponding HUD template, not just
   * a coincidental digit. */
  static const struct {
    uint8_t column, row, count, owner, left_inset;
    ArLocalizationTextLayoutKind layout;
  } numbers[] = {
    {8, 1, 2, kHudFieldActLabel, 0,
     kArLocalizationTextLayout_SingleLineLabel},
    {15, 1, 3, kHudFieldTimeLabel, 1,
     kArLocalizationTextLayout_SingleLineLabel},
    {26, 1, 5, kHudFieldScoreLabel, 0,
     kArLocalizationTextLayout_RightAlignedLabel},
    /* Simulation and Sky Palace: population above SP, both written by the same
     * native formatter as "current/maximum". SP is one cell narrower and the
     * formatter indents it with a leading blank, which the rasterizer crops
     * away -- so the indent is stated as an inset instead. Anchoring to the
     * trailing edge would not do: these glyphs are narrower than the native
     * cell pitch, so a right-aligned value starts well inside where the
     * original begins. */
    {23, 1, 8, kHudFieldSimContextLabel, 0,
     kArLocalizationTextLayout_SingleLineLabel},
    {23, 2, 8, kHudFieldSimSpLabel, 8,
     kArLocalizationTextLayout_SingleLineLabel},
  };
  for (unsigned i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
    if (!Matches(&kFields[numbers[i].owner], vram, base)) continue;
    char digits[9] = {0};
    bool valid = true;
    uint64_t revision = 1;
    uint16_t ink_word = 0;
    for (unsigned j = 0; j < numbers[i].count; ++j) {
      const uint16_t word = Word(vram, base, numbers[i].row, numbers[i].column + j);
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
    if (valid && ink_word)
      Append(frame, 210 + i, destination,
          (ArTextCellRegion){numbers[i].column, numbers[i].row,
                             numbers[i].count, 1},
          digits, numbers[i].count, numbers[i].count, revision,
          kArTextDirection_LeftToRight, ink_word, cgram, true,
          numbers[i].layout, numbers[i].left_inset, 1);
  }
}
