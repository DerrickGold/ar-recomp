#include "actraiser/actraiser_localization_world_navigation.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #value); \
  ++failures; \
} } while (0)

typedef struct ResolverState {
  unsigned calls;
  bool blank;
  bool with_bidi;
  char semantic_id[64];
} ResolverState;

static bool Resolve(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint8_t *structural_boundaries, ArLocalizationTextLanguage *language,
    ArTextBidiSpans *bidi, ArLocalizationTextField *live_field,
    char *error, size_t error_capacity) {
  if (live_field) *live_field = (ArLocalizationTextField){0};
  (void)inline_objects;
  (void)inline_object_capacity;
  (void)structural_boundaries;
  (void)error;
  (void)error_capacity;
  ResolverState *state = context;
  ++state->calls;
  snprintf(state->semantic_id, sizeof(state->semantic_id), "%s", semantic_id);
  const char *value = state->blank ? "" : "مدينة";
  const size_t bytes = strlen(value);
  if (bytes >= utf8_capacity) return false;
  memcpy(utf8, value, bytes + 1u);
  *utf8_bytes = bytes;
  *cluster_count = state->blank ? 0 : 5;
  *source_revision = 100 + state->calls;
  *inline_object_count = 0;
  *language = (ArLocalizationTextLanguage){
      .locale = "ar", .direction = kArTextDirection_RightToLeft};
  bidi->count = state->with_bidi ? 1 : 0;
  if (bidi->count)
    bidi->spans[0] = (ArTextBidiSpan){
        .start = 0, .end = 2,
        .direction = kArTextDirection_RightToLeft};
  return true;
}

static void PrepareFrame(ArLocalizationFrame *frame) {
  ArLocalizationFrame_Reset(frame);
  CHECK(ArLocalizationFrame_SetFont(
      frame, "ar", "test.font", 1, 7, &frame->settings));
}

int main(void) {
  static const char *const ids[] = {
    "city.fillmore.name", "city.bloodpool.name", "city.kasandora.name",
    "city.aitos.name", "city.marahna.name", "city.northwall.name",
    "city.death_heim.name",
  };
  uint16_t cgram[256] = {0};
  cgram[129] = 0x0000;
  cgram[131] = 0x728c;
  cgram[132] = 0x7b56;
  ActRaiserLocalizationWorldNavigation state;
  ActRaiserLocalizationWorldNavigation_Init(&state);
  ResolverState resolver = {0};
  for (uint16_t location = 1; location <= 7; ++location) {
    ActRaiserLocalizationWorldNavigation_Invalidate(&state);
    ArLocalizationFrame frame;
    PrepareFrame(&frame);
    char error[128] = {0};
    CHECK(ActRaiserLocalizationWorldNavigation_Append(
        &state, &frame, location, true, cgram, 256,
        Resolve, &resolver, error, sizeof(error)));
    CHECK(!strcmp(resolver.semantic_id, ids[location - 1]));
    CHECK(frame.screen_text_count == 1 && frame.snapshot_count == 1 &&
          frame.cells.count == 0 && ArLocalizationFrame_IsValid(&frame));
    const ArLocalizationScreenTextRecord *record =
        ArLocalizationFrame_FindScreenText(
            &frame, kActRaiserLocalizationWorldNavigationSurface);
    CHECK(record && record->x == 156 && record->y == 25 &&
          record->width == 76 && record->height == 8);
    const ArLocalizationTextSnapshot *snapshot = &frame.snapshots[0];
    CHECK(snapshot->language.direction == kArTextDirection_RightToLeft);
    CHECK(snapshot->style_id == kArTextStyle_RetailPaletteBands);
    CHECK(snapshot->shadow_enabled &&
          snapshot->shadow_shape == kArTextShadow_Diagonal);
    CHECK(snapshot->band_rgb == UINT32_C(0x63a5e7));
    CHECK(snapshot->body_rgb == UINT32_C(0xb5d6f7));
    CHECK(snapshot->shadow_rgb == 0);
  }

  /* The cache avoids reparsing an unchanged pack/location every frame. */
  const unsigned calls = resolver.calls;
  ArLocalizationFrame cached;
  PrepareFrame(&cached);
  CHECK(ActRaiserLocalizationWorldNavigation_Append(
      &state, &cached, 7, true, cgram, 256,
      Resolve, &resolver, NULL, 0));
  CHECK(resolver.calls == calls);

  /* A hidden label and invalid locations never claim the native glyph range. */
  ArLocalizationFrame absent;
  PrepareFrame(&absent);
  CHECK(!ActRaiserLocalizationWorldNavigation_Append(
      &state, &absent, 7, false, cgram, 256,
      Resolve, &resolver, NULL, 0));
  CHECK(!absent.screen_text_count && resolver.calls == calls);
  CHECK(!ActRaiserLocalizationWorldNavigation_Append(
      &state, &absent, 0, true, cgram, 256,
      Resolve, &resolver, NULL, 0));

  /* An authored empty value is a successful claim: presentation removes the
   * native glyphs but keeps the plaque. */
  resolver.blank = true;
  ActRaiserLocalizationWorldNavigation_Invalidate(&state);
  ArLocalizationFrame blank;
  PrepareFrame(&blank);
  CHECK(ActRaiserLocalizationWorldNavigation_Append(
      &state, &blank, 1, true, cgram, 256,
      Resolve, &resolver, NULL, 0));
  CHECK(blank.screen_text_count == 1 && !blank.snapshots[0].utf8_bytes &&
        ArLocalizationFrame_IsValid(&blank));

  /* Appending is transactional. A full but valid bidi pool cannot leave a
   * half-published label that would suppress native text after append fails. */
  char pressure_text[kArTextMaximumBidiSpans * 2 + 1];
  memset(pressure_text, 'a', sizeof(pressure_text) - 1u);
  pressure_text[sizeof(pressure_text) - 1u] = 0;
  ArTextBidiSpans pressure_bidi = {.count = kArTextMaximumBidiSpans};
  for (uint16_t i = 0; i < pressure_bidi.count; ++i)
    pressure_bidi.spans[i] = (ArTextBidiSpan){
        .start = i * 2u, .end = i * 2u + 1u,
        .direction = kArTextDirection_LeftToRight};
  ArLocalizationFrame pressure;
  PrepareFrame(&pressure);
  CHECK(ArLocalizationFrame_AddScreenText(
      &pressure, 999, 0, 0, 64, 8,
      pressure_text, sizeof(pressure_text) - 1u,
      sizeof(pressure_text) - 1u, sizeof(pressure_text) - 1u, 88,
      kArTextDirection_LeftToRight, 7,
      kArLocalizationTextLayout_SingleLineLabel));
  const ArLocalizationTextLanguage pressure_language = {
      .locale = "en", .direction = kArTextDirection_LeftToRight};
  CHECK(ArLocalizationFrame_SetTextLanguage(&pressure, &pressure_language));
  CHECK(ArLocalizationFrame_SetTextBidiSpans(&pressure, &pressure_bidi));
  CHECK(ArLocalizationFrame_IsValid(&pressure));
  const uint8_t old_snapshots = pressure.snapshot_count;
  const uint8_t old_screen_texts = pressure.screen_text_count;
  const uint32_t old_text_bytes = pressure.text_bytes;
  resolver.blank = false;
  resolver.with_bidi = true;
  ActRaiserLocalizationWorldNavigation_Invalidate(&state);
  char pressure_error[128] = {0};
  CHECK(!ActRaiserLocalizationWorldNavigation_Append(
      &state, &pressure, 2, true, cgram, 256,
      Resolve, &resolver, pressure_error, sizeof(pressure_error)));
  CHECK(strstr(pressure_error, "does not fit") != NULL);
  CHECK(pressure.snapshot_count == old_snapshots &&
        pressure.screen_text_count == old_screen_texts &&
        pressure.text_bytes == old_text_bytes &&
        pressure.bidi.count == kArTextMaximumBidiSpans &&
        ArLocalizationFrame_IsValid(&pressure));

  puts("world-navigation localization checks passed");
  return failures ? 1 : 0;
}
