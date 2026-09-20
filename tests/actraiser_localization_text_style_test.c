#include "actraiser/actraiser_localization_name_compose.h"
#include "actraiser/actraiser_localization_resolved_text.h"
#include "actraiser/actraiser_localization_text_normalize.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void AddText(ArLocalizationFrame *frame, const char *text) {
  ArLocalizationFrame_Reset(frame);
  CHECK(ArLocalizationFrame_SetFont(frame, "en", "fixture", 1, 1,
                                    &frame->settings));
  CHECK(ArLocalizationFrame_AddText(
      frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){0, 0, 30, 10}, text, strlen(text), 1, 1, 1,
      kArTextDirection_LeftToRight, 8, NULL, 0));
}

static void TestOwnedStylesAndPalettes(void) {
  ArDialogueTreatmentDefinition treatment = {
      .definition = {
          .name = "hud",
          .band = {.kind = kArTextInk_Binding, .binding = "hud.band"},
          .body = {.kind = kArTextInk_Binding, .binding = "hud.body"},
          .shadow = {.kind = kArTextInk_Binding, .binding = "hud.shadow"},
          .keyline_shadow = true}};
  ArTextTemplateSpan span = {.start = 2,
                             .end = 4,
                             .style = {.font = "hud",
                                       .treatment = "hud",
                                       .italic = 2,
                                       .scale_percent = 125}};
  const char source[] = "A 12 B";
  ArDialoguePageSnapshot page = {.format_version = 2,
                                 .utf8 = source,
                                 .utf8_bytes = sizeof(source) - 1,
                                 .style_spans = &span,
                                 .style_span_count = 1,
                                 .default_style = {.scale_percent = 120},
                                 .numerals = 2,
                                 .treatments = &treatment,
                                 .treatment_count = 1,
                                 .source_path = "dialogue.artext",
                                 .source_line = 7,
                                 .resolved_message_id = "fixture"};
  uint16_t offsets[sizeof(source)];
  for (size_t i = 0; i < sizeof(source); ++i)
    offsets[i] = (uint16_t)i;
  ActRaiserTextStylePlan plan = {0};
  char error[256] = {0};
  CHECK(ActRaiserTextStyle_AppendPage(&plan, &page, 0, page.utf8_bytes, offsets,
                                      source, strlen(source), 0, error,
                                      sizeof(error)));
  CHECK(plan.authored && plan.span_count == 1 && plan.style_count == 2);
  /* Neither the source session nor its treatment storage is borrowed. */
  memset(&treatment, 0, sizeof(treatment));
  memset(&span, 0, sizeof(span));
  memset(&page, 0, sizeof(page));
  ArLocalizationFrame frame;
  AddText(&frame, source);
  uint16_t cgram[256] = {0};
  cgram[1] = 0x1f;
  cgram[2] = 0x3e0;
  cgram[3] = 0x7c00;
  ActRaiserTextPalette inks;
  ActRaiserTextPalette_Capture(&inks, cgram, 256);
  /* A HUD style cannot accidentally pick up the dialogue's RGB endpoints. */
  CHECK(!ActRaiserTextStyle_Publish(&plan, 0, &inks, &frame));
  CHECK(frame.cells.count == 0 && ArLocalizationFrame_IsValid(&frame));
  const uint16_t hud[] = {0, 0, 0x1f, 0x7fff};
  ActRaiserTextPalette_SetHud(&inks, hud);
  AddText(&frame, source);
  CHECK(ActRaiserTextStyle_Publish(&plan, 0, &inks, &frame));
  CHECK(ArLocalizationFrame_IsValid(&frame));
  CHECK(frame.snapshots[0].appearance.scale_basis == 12000);
  CHECK(!strcmp(frame.snapshots[0].origin.source_path, "dialogue.artext"));
  CHECK(frame.snapshots[0].origin.source_line == 7);
  CHECK(!strcmp(frame.snapshots[0].origin.resolved_id, "fixture"));
  CHECK(frame.snapshots[0].appearance.slant_ascii_numerals);
  CHECK(frame.appearance_span_count == 1);
  const ArTextRunAppearance *appearance = &frame.appearance_spans[0].appearance;
  CHECK(appearance->scale_basis == 15000 && appearance->italic);
  CHECK(!appearance->slant_ascii_numerals && appearance->keyline_shadow);
  CHECK(!strcmp(appearance->font_role, "body"));
  /* A selected role overrides the inherited body stack when it is declared. */
  CHECK(appearance->band_rgb == 0xff0000 && appearance->body_rgb == 0xffffff);
  AddText(&frame, source);
  const ArTextFontRole role = {.name = "hud", .primary = 2};
  CHECK(ArLocalizationFrame_SetFontRoles(&frame, &role, 1));
  CHECK(ActRaiserTextStyle_Publish(&plan, 0, &inks, &frame));
  CHECK(!strcmp(frame.appearance_spans[0].appearance.font_role, "hud"));
  CHECK(appearance->band_rgb == 0xff0000 && appearance->body_rgb == 0xffffff);
  /* Per-frame palette changes are captured, and earlier frames stay immutable.
   */
  const ArLocalizationFrame previous = frame;
  const uint16_t changed[] = {0, 0, 0x3e0, 0x7c00};
  ActRaiserTextPalette_SetHud(&inks, changed);
  AddText(&frame, "12");
  CHECK(ActRaiserTextStyle_Publish(&plan, 2, &inks, &frame));
  CHECK(frame.appearance_spans[0].start == 0 &&
        frame.appearance_spans[0].end == 2);
  CHECK(frame.appearance_spans[0].appearance.band_rgb == 0x00ff00);
  CHECK(previous.appearance_spans[0].appearance.band_rgb == 0xff0000);
  /* The frame setter rejects corrupt ranges transactionally. */
  AddText(&frame, "É");
  ArTextAppearanceSpan invalid = {
      .start = 1, .end = 2, .appearance = plan.styles[0].appearance};
  CHECK(!ArLocalizationFrame_SetTextAppearance(
      &frame, &plan.styles[0].appearance, &invalid, 1));
  CHECK(!frame.appearance_span_count && !frame.snapshots[0].has_appearance);
  invalid.start = 0;
  CHECK(ArLocalizationFrame_SetTextAppearance(
      &frame, &plan.styles[0].appearance, &invalid, 1));
  frame.appearance_spans[0].end = 3;
  CHECK(!ArLocalizationFrame_IsValid(&frame));
}

static void TestNormalizedAndEditedRanges(void) {
  const char source[] = "  Alpha   12  ";
  ArTextTemplateSpan span = {.start = 10, .end = 12, .style = {.italic = 2}};
  ArDialoguePageSnapshot page = {.format_version = 2,
                                 .utf8 = source,
                                 .utf8_bytes = sizeof(source) - 1,
                                 .style_spans = &span,
                                 .style_span_count = 1};
  ActRaiserResolvedText text = {0};
  uint16_t offsets[sizeof(source)];
  CHECK(ActRaiserLocalizationText_Normalize(
      source, sizeof(source) - 1, NULL, 0, false, text.utf8, sizeof(text.utf8),
      &text.utf8_bytes, text.inline_objects,
      kArLocalizationFrameInlineObjectCapacity, &text.inline_object_count,
      offsets));
  CHECK(ActRaiserTextStyle_AppendPage(&text.styles, &page, 0, page.utf8_bytes,
                                      offsets, text.utf8, text.utf8_bytes, 0,
                                      NULL, 0));
  CHECK(text.styles.span_count == 1);
  const ActRaiserTextStyleSpan *mapped = &text.styles.spans[0];
  CHECK(mapped->end - mapped->start == 2 &&
        !memcmp(text.utf8 + mapped->start, "12", 2));
  ActRaiserTextStyle_Edit(&text.styles, 0, mapped->start, 0);
  CHECK(text.styles.spans[0].start == 0 && text.styles.spans[0].end == 2);

  /* Keyboard gutter and page indicator edits preserve a styled selected key. */
  const char keyboard[] = "ABCDEFGH\n--------\nA B\nC D\nE F\nG H\nI J";
  memset(&text, 0, sizeof(text));
  strcpy(text.utf8, keyboard);
  text.utf8_bytes = strlen(keyboard);
  const uint32_t key = (uint32_t)(strstr(keyboard, "C D") - keyboard + 2);
  text.styles.span_count = 1;
  text.styles.spans[0] = (ActRaiserTextStyleSpan){key, key + 1, 0};
  text.bidi.count = 1;
  text.bidi.spans[0] = (ArTextBidiSpan){key, key + 1, kArTextDirection_Auto};
  CHECK(ActRaiserLocalizationNameCompose_ClearUnderlineRow(&text));
  CHECK(ActRaiserLocalizationNameCompose_PrepareField(&text));
  CHECK(ActRaiserLocalizationNameCompose_InsertPageIndicator(&text, 1, 3));
  CHECK(ActRaiserLocalizationNameCompose_ExpandKeyGutters(&text));
  mapped = &text.styles.spans[0];
  CHECK(mapped->end == mapped->start + 1 && text.utf8[mapped->start] == 'D');
  CHECK(text.bidi.spans[0].start == mapped->start &&
        text.bidi.spans[0].end == mapped->end);
  CHECK(text.live_field.utf8_bytes == 8 && text.live_field.cells == 8);
  CHECK(text.inline_object_count == 8);
}

static bool ResolveCount(void *context, const char *name,
                         ArLanguagePlaceholderKind kind, ArDialogueValue *value,
                         char *error, size_t capacity) {
  (void)context;
  (void)error;
  (void)capacity;
  if (strcmp(name, "count") || kind != kArLanguagePlaceholder_Number)
    return false;
  value->kind = kind;
  value->number = 7;
  return true;
}

static void TestParsedTemplateToFrame(void) {
  const char script[] =
      "@define-style hud band=native:hud.band body=native:hud.body "
      "shadow=native:hud.shadow\n"
      ":: sample\n@style hud\n@scale 120%\n"
      "Read <span font=\"hud\" scale=\"80%\">{count:02}</span>.\n@end\n"
      ":: alias\n@alias sample\n";
  const ArTextDocumentSource source_text = {"source.artext", script,
                                            sizeof(script) - 1};
  const char *roles[] = {"hud"};
  const ArTextDocumentConfig document = {.id = "fixture",
                                         .locale = "en",
                                         .format_version = 2,
                                         .sources = &source_text,
                                         .source_count = 1,
                                         .font_roles = roles,
                                         .font_role_count = 1};
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error = {{0}};
  CHECK(ArLanguagePack_ParseDocument(&pack, &document, &error));
  const ArDialogueSource source = {
      .effective_pack = &pack,
      .message = ArLanguagePack_FindMessage(&pack, "alias"),
      .resolved_source = kArDialogueResolvedSource_SelectedPack,
      .presentation = kArDialoguePresentation_Enhanced};
  const ArDialogueContract contract = {
      .values = {{"count", kArLanguagePlaceholder_Number}}, .value_count = 1};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .resolve = ResolveCount};
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_BeginSource(&session, &source, &contract, "alias",
                                      &resolver, 0, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  ActRaiserResolvedText text = {0};
  uint16_t offsets[kActRaiserLocalizationComposeTextCapacity + 1];
  CHECK(ActRaiserLocalizationText_Normalize(
      page.utf8, page.utf8_bytes, NULL, 0, false, text.utf8, sizeof(text.utf8),
      &text.utf8_bytes, text.inline_objects,
      kArLocalizationFrameInlineObjectCapacity, &text.inline_object_count,
      offsets));
  CHECK(ActRaiserTextStyle_AppendPage(&text.styles, &page, 0, page.utf8_bytes,
                                      offsets, text.utf8, text.utf8_bytes, 0,
                                      error.message, sizeof(error.message)));
  CHECK(!strcmp(text.utf8, "Read 07."));
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
  ArLocalizationFrame frame;
  AddText(&frame, text.utf8);
  ActRaiserTextPalette inks = {0};
  const uint16_t hud[] = {0, 0, 0x3e0, 0x7fff};
  ActRaiserTextPalette_SetHud(&inks, hud);
  CHECK(ActRaiserTextStyle_Publish(&text.styles, 0, &inks, &frame));
  CHECK(frame.snapshots[0].has_appearance && frame.appearance_span_count == 1);
  CHECK(!strcmp(frame.snapshots[0].origin.message_id, "alias"));
  CHECK(!strcmp(frame.snapshots[0].origin.resolved_id, "sample"));
  CHECK(frame.snapshots[0].origin.source_line == 2);
  CHECK(frame.appearance_spans[0].start == 5 &&
        frame.appearance_spans[0].end == 7);
  CHECK(frame.appearance_spans[0].appearance.scale_basis == 9600);
  CHECK(frame.appearance_spans[0].appearance.band_rgb == 0x00ff00);
  CHECK(ArLocalizationFrame_IsValid(&frame));
}

int main(void) {
  TestParsedTemplateToFrame();
  TestOwnedStylesAndPalettes();
  TestNormalizedAndEditedRanges();
  return failures ? 1 : 0;
}
