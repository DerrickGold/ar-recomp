#include "actraiser/actraiser_localization_world_navigation.h"

#include <stdio.h>
#include <string.h>

#include "actraiser/actraiser_localization_style.h"
#include "sim/sim_world_navigation_scene.h"

enum {
  kFirstLocation = 1,
  kLocationCount = 7,
  kNativeFontPixels = 7,
  /* OBJ palette zero begins at CGRAM 128. The native glyph uses black ink 1,
   * blue ink 3 and light-blue ink 4; ink 2 is intentionally unused. */
  kObjPaletteBase = 128,
  kObjShadowColor = kObjPaletteBase + 1,
  kObjBandColor = kObjPaletteBase + 3,
  kObjBodyColor = kObjPaletteBase + 4,
};

_Static_assert(kActRaiserLocalizationWorldNavigationSurface >
                   kActRaiserLocalizationComposeSurfaceLast,
               "world-navigation surface must not overlap compose surfaces");

static const char *const kLocationSemanticIds[kLocationCount] = {
  "city.fillmore.name",
  "city.bloodpool.name",
  "city.kasandora.name",
  "city.aitos.name",
  "city.marahna.name",
  "city.northwall.name",
  "city.death_heim.name",
};

void ActRaiserLocalizationWorldNavigation_Init(
    ActRaiserLocalizationWorldNavigation *state) {
  if (state) memset(state, 0, sizeof(*state));
}

void ActRaiserLocalizationWorldNavigation_Invalidate(
    ActRaiserLocalizationWorldNavigation *state) {
  ActRaiserLocalizationWorldNavigation_Init(state);
}

static bool ResolveLocation(
    ActRaiserLocalizationWorldNavigation *state, uint16_t active_location,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (!state || !resolve_text || active_location < kFirstLocation ||
      active_location >= kFirstLocation + kLocationCount)
    return false;
  state->resolved = false;
  state->attempted_location = active_location;
  state->utf8_bytes = 0;
  state->cluster_count = 0;
  state->source_revision = 0;
  state->bidi.count = 0;
  uint8_t inline_object_count = 0;
  ArLocalizationInlineObjectSnapshot inline_objects[1];
  if (!resolve_text(
          resolve_context,
          kLocationSemanticIds[active_location - kFirstLocation],
          state->utf8, sizeof(state->utf8), &state->utf8_bytes,
          &state->cluster_count, &state->source_revision,
          inline_objects, 1, &inline_object_count, NULL,
          &state->language, &state->bidi, error, error_capacity) ||
      inline_object_count ||
      !ArLocalizationTextLanguage_IsValid(&state->language) ||
      !ArTextBidiSpans_FitSource(
          &state->bidi, state->utf8, state->utf8_bytes)) {
    if (error && error_capacity && !error[0])
      snprintf(error, error_capacity,
               "world-navigation location text is invalid");
    return false;
  }
  state->resolved = true;
  return true;
}

static void RollbackScreenText(
    ArLocalizationFrame *frame, uint8_t snapshot_count,
    uint8_t screen_text_count, uint16_t bidi_count, uint32_t text_bytes) {
  if (!frame) return;
  if (frame->snapshot_count > snapshot_count)
    memset(&frame->snapshots[snapshot_count], 0,
           (size_t)(frame->snapshot_count - snapshot_count) *
               sizeof(frame->snapshots[0]));
  if (frame->screen_text_count > screen_text_count)
    memset(&frame->screen_texts[screen_text_count], 0,
           (size_t)(frame->screen_text_count - screen_text_count) *
               sizeof(frame->screen_texts[0]));
  if (frame->bidi.count > bidi_count)
    memset(&frame->bidi.spans[bidi_count], 0,
           (size_t)(frame->bidi.count - bidi_count) *
               sizeof(frame->bidi.spans[0]));
  if (frame->text_bytes > text_bytes)
    memset(frame->text + text_bytes, 0, frame->text_bytes - text_bytes);
  frame->snapshot_count = snapshot_count;
  frame->screen_text_count = screen_text_count;
  frame->bidi.count = bidi_count;
  frame->text_bytes = text_bytes;
}

bool ActRaiserLocalizationWorldNavigation_Append(
    ActRaiserLocalizationWorldNavigation *state,
    ArLocalizationFrame *frame, uint16_t active_location,
    bool native_label_visible,
    const uint16_t *cgram_words, size_t cgram_word_count,
    ActRaiserLocalizationComposeTextResolver resolve_text,
    void *resolve_context, char *error, size_t error_capacity) {
  if (!state || !frame || !native_label_visible ||
      active_location < kFirstLocation ||
      active_location >= kFirstLocation + kLocationCount ||
      !cgram_words || cgram_word_count <= kObjBodyColor)
    return false;
  if (state->attempted_location != active_location &&
      !ResolveLocation(state, active_location, resolve_text, resolve_context,
                       error, error_capacity))
    return false;
  if (!state->resolved) return false;
  if (frame->bidi.count > kArTextMaximumBidiSpans ||
      state->bidi.count > kArTextMaximumBidiSpans - frame->bidi.count) {
    if (error && error_capacity)
      snprintf(error, error_capacity,
               "world-navigation bidi metadata does not fit the frame");
    return false;
  }
  const uint8_t snapshot_count = frame->snapshot_count;
  const uint8_t screen_text_count = frame->screen_text_count;
  const uint16_t bidi_count = frame->bidi.count;
  const uint32_t text_bytes = frame->text_bytes;
  if (!ArLocalizationFrame_AddScreenText(
          frame, kActRaiserLocalizationWorldNavigationSurface,
          kSimWorldNavigationLabelX, kSimWorldNavigationLabelY,
          kSimWorldNavigationLabelWidth, kSimWorldNavigationLabelHeight,
          state->utf8, state->utf8_bytes,
          state->cluster_count, state->cluster_count,
          state->source_revision, state->language.direction,
          kNativeFontPixels, kArLocalizationTextLayout_SingleLineLabel))
    return false;
  if (!ArLocalizationFrame_SetTextLanguage(frame, &state->language) ||
      !ArLocalizationFrame_SetTextBidiSpans(frame, &state->bidi)) {
    RollbackScreenText(frame, snapshot_count, screen_text_count,
                       bidi_count, text_bytes);
    return false;
  }
  ArLocalizationTextSnapshot *snapshot =
      &frame->snapshots[frame->snapshot_count - 1u];
  snapshot->style_id = kArTextStyle_RetailPaletteBands;
  snapshot->shadow_enabled = true;
  snapshot->shadow_shape = kArTextShadow_Diagonal;
  snapshot->slant_ascii_numerals = false;
  snapshot->shadow_rgb =
      ActRaiserLocalizationStyle_Rgb(cgram_words[kObjShadowColor]);
  snapshot->band_rgb =
      ActRaiserLocalizationStyle_Rgb(cgram_words[kObjBandColor]);
  snapshot->body_rgb =
      ActRaiserLocalizationStyle_Rgb(cgram_words[kObjBodyColor]);
  return true;
}
