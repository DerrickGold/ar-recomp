/* Link this with the reusable session/parser only. A generated game catalog,
 * ROM, native pack or PPU must never be needed to run these host mechanics. */
#include "localization/dialogue_session.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%d: %s: %s\n", __LINE__, #condition, error.message);    \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static bool Resolve(void *context, const char *name,
                    ArLanguagePlaceholderKind kind, ArDialogueValue *value,
                    char *error, size_t capacity) {
  (void)error;
  (void)capacity;
  if (strcmp(name, "count") || kind != kArLanguagePlaceholder_Number)
    return false;
  value->kind = kind;
  value->number = *(const int *)context;
  return true;
}

int main(void) {
  static const char script[] =
      "@define-style sample band=#FFFFFF body=#FFFFFF shadow=none\n"
      ":: sample.greeting\n@style sample\nFirst <i>{count:02}</i>\n"
      "@wait 2\n second\n@anchor host.commit\n@page\nLast\n@end\n";
  const ArTextDocumentSource text = {"example.artext", script,
                                     sizeof(script) - 1};
  const ArTextDocumentConfig document = {
      .id = "example",
      .locale = "en",
      .format_version = 2,
      .sources = &text,
      .source_count = 1,
  };
  ArLanguagePackError error = {0};
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  REQUIRE(ArLanguagePack_ParseDocument(&pack, &document, &error));
  const ArDialogueSource source = {
      .effective_pack = &pack,
      .message = ArLanguagePack_FindMessage(&pack, "sample.greeting"),
      .resolved_source = kArDialogueResolvedSource_SelectedPack,
      .presentation = kArDialoguePresentation_Enhanced,
  };
  const ArDialogueContract contract = {
      .values = {{"count", kArLanguagePlaceholder_Number}},
      .value_count = 1,
      .control_count = 1,
  };
  int count = 3;
  ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .resolve = Resolve,
      .context = &count,
  };
  ArDialogueSession first, second, restored;
  ArDialogueSession_Init(&first);
  ArDialogueSession_Init(&second);
  ArDialogueSession_Init(&restored);
  REQUIRE(ArDialogueSession_BeginSource(
      &first, &source, &contract, "sample.greeting", &resolver, 0, &error));
  count = 8;
  REQUIRE(ArDialogueSession_BeginSource(
      &second, &source, &contract, "sample.greeting", &resolver, 0, &error));
  ArDialoguePageSnapshot page;
  REQUIRE(ArDialogueSession_GetPage(&first, &page));
  REQUIRE(!strcmp(page.utf8, "First 03 second") && page.style_span_count == 1);
  REQUIRE(!strcmp(page.source_path, "example.artext") &&
          !page.revealed_utf8_bytes);
  ArDialogueToken token;
  do {
    REQUIRE(ArDialogueSession_Next(&first, &token, &error));
  } while (token.kind == kArDialogueToken_Grapheme);
  REQUIRE(token.kind == kArDialogueToken_WaitStarted);
  REQUIRE(ArDialogueSession_GetPage(&second, &page));
  REQUIRE(!page.revealed_utf8_bytes && !strcmp(page.utf8, "First 08 second"));
  ArDialogueStableState saved;
  REQUIRE(ArDialogueSession_ExportState(&first, &saved));
  REQUIRE(ArDialogueSession_RestoreSource(&restored, &source, &contract, &saved,
                                          &resolver, &error));
  ArDialogueSession_TickWait(&restored, 1);
  REQUIRE(first.state.wait_frames_remaining == 2 &&
          restored.state.wait_frames_remaining == 1);
  ArDialogueSession_TickWait(&restored, 1);
  do {
    REQUIRE(ArDialogueSession_Next(&restored, &token, &error));
  } while (token.kind == kArDialogueToken_Grapheme);
  REQUIRE(token.kind == kArDialogueToken_Control &&
          !strcmp(token.control_id, "host.commit"));
  REQUIRE(ArDialogueSession_ExportState(&restored, &saved));
  REQUIRE(ArDialogueSession_RestoreSource(&restored, &source, &contract, &saved,
                                          &resolver, &error));
  REQUIRE(ArDialogueSession_Next(&restored, &token, &error));
  REQUIRE(token.kind == kArDialogueToken_Control && token.control_ordinal == 0);
  REQUIRE(ArDialogueSession_CompleteControl(&restored, token.control_ordinal));
  REQUIRE(!ArDialogueSession_CompleteControl(&restored, token.control_ordinal));
  REQUIRE(ArDialogueSession_Next(&restored, &token, &error));
  REQUIRE(token.kind == kArDialogueToken_PageComplete);
  REQUIRE(ArDialogueSession_AdvancePage(&restored));
  ArLanguagePack_Destroy(&pack);
  REQUIRE(ArDialogueSession_GetPage(&restored, &page));
  REQUIRE(!strcmp(page.utf8, "Last") && page.page_index == 1);
  REQUIRE(ArDialogueSession_GetPage(&first, &page));
  REQUIRE(page.style_spans[0].style.italic == 2 &&
          !strcmp(page.source_path, "example.artext"));
  REQUIRE(page.treatment_count == 1 &&
          page.treatments[0].definition.body.rgb == 0xFFFFFF);
  REQUIRE(!strcmp(page.treatments[0].source_path, "example.artext"));
  ArDialogueSession_Destroy(&first);
  ArDialogueSession_Destroy(&second);
  ArDialogueSession_Destroy(&restored);
  return 0;
}
