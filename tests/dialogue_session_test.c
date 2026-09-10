#include "localization/dialogue_session.h"
#include "fixtures/keyboard_body.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MemoryFile {
  const char *path;
  const uint8_t *data;
  size_t size;
} MemoryFile;

typedef struct MemoryVfs {
  const MemoryFile *files;
  size_t count;
} MemoryVfs;

static int failures;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                      \
      failures++;                                                               \
    }                                                                            \
  } while (0)

static bool ReadFile(void *context, const char *path, size_t maximum_bytes,
                     ArLanguagePackBlob *blob, char *error,
                     size_t error_capacity) {
  MemoryVfs *vfs = (MemoryVfs *)context;
  for (size_t i = 0; i < vfs->count; i++) {
    if (strcmp(vfs->files[i].path, path) != 0)
      continue;
    if (vfs->files[i].size > maximum_bytes) {
      snprintf(error, error_capacity, "file is too large");
      return false;
    }
    blob->struct_size = sizeof(*blob);
    blob->data = vfs->files[i].data;
    blob->size = vfs->files[i].size;
    blob->token = i + 1;
    return true;
  }
  snprintf(error, error_capacity, "file not found");
  return false;
}

static void ReleaseFile(void *context, ArLanguagePackBlob *blob) {
  (void)context;
  memset(blob, 0, sizeof(*blob));
}

static bool LoadPack(ArLanguagePack *pack, const char *id, const char *locale,
                     const char *script, ArLanguagePackError *error) {
  char manifest[1024];
  const int manifest_size = snprintf(
      manifest, sizeof(manifest),
      "[pack]\n"
      "format = actraiser-language-pack\n"
      "version = 1\n"
      "id = %s\n"
      "locale = %s\n"
      "name = Synthetic %s\n"
      "autonym = Synthetic %s\n"
      "author = Test Author\n"
      "license = MIT\n"
      "direction = %s\n"
      "target = us-runtime\n"
      "source_profile = us\n"
      "fallback = native-us\n"
      "coverage = partial\n"
      "[fonts]\n"
      "primary = builtin:actraiser-sans\n"
      "[scripts]\n"
      "source = text/main.artext\n",
      id, locale, id, id, !strcmp(locale, "ar") ? "rtl" : "ltr");
  CHECK(manifest_size > 0 && (size_t)manifest_size < sizeof(manifest));
  const MemoryFile files[] = {
      {"pack.ini", (const uint8_t *)manifest, (size_t)manifest_size},
      {"text/main.artext", (const uint8_t *)script, strlen(script)},
  };
  MemoryVfs vfs = {files, sizeof(files) / sizeof(files[0])};
  ArLanguagePackIo io = {
      .struct_size = sizeof(io),
      .abi_version = AR_LANGUAGE_PACK_IO_ABI_VERSION,
      .context = &vfs,
      .read_file = ReadFile,
      .release_file = ReleaseFile,
  };
  return ArLanguagePack_Load(pack, &io, "pack.ini", error);
}

static ArDialogueContentSelection Selection(
    ArDialoguePresentation presentation, const ArLanguagePack *selected,
    const ArLanguagePack *native_pack) {
  return (ArDialogueContentSelection){
      .struct_size = sizeof(ArDialogueContentSelection),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .presentation = presentation,
      .selected_pack = selected,
      .native_us_enhanced_pack = native_pack,
  };
}

static void RevealPage(ArDialogueSession *session) {
  ArDialoguePageSnapshot page;
  ArLanguagePackError error;
  CHECK(ArDialogueSession_GetPage(session, &page));
  while (session->state.revealed_cluster_count < page.cluster_count) {
    ArDialogueToken token;
    CHECK(ArDialogueSession_Next(session, &token, &error));
    CHECK(token.kind == kArDialogueToken_Grapheme);
  }
}

static void TestLongerShorterAndNativeSwitch(void) {
  static const char native_script[] =
      ":: dialogue.event.relay.aitos\nNative source\n";
  static const char long_script[] =
      ":: dialogue.event.relay.aitos\n"
      "Première page café\n"
      "@page\n"
      "Deuxième page\n"
      "@page\n"
      "Troisième page\n";
  static const char short_script[] =
      ":: dialogue.event.relay.aitos\nBref\n";
  ArLanguagePack native_pack, long_pack, short_pack;
  ArLanguagePack_Init(&native_pack);
  ArLanguagePack_Init(&long_pack);
  ArLanguagePack_Init(&short_pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&native_pack, "native.us", "en-US", native_script, &error));
  CHECK(LoadPack(&long_pack, "community.long", "fr-CA", long_script, &error));
  CHECK(LoadPack(&short_pack, "community.short", "fr-FR", short_script,
                 &error));

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection long_selection =
      Selection(kArDialoguePresentation_Enhanced, &long_pack, &native_pack);
  ArDialogueContentSelection short_selection =
      Selection(kArDialoguePresentation_Enhanced, &short_pack, &native_pack);
  CHECK(ArDialogueSession_Begin(&session, &long_selection,
                                "dialogue.event.relay.aitos", NULL, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_count == 3);
  CHECK(strcmp(page.package_id, "community.long") == 0);
  CHECK(strcmp(page.locale, "fr-CA") == 0);
  CHECK(strstr(page.utf8, "café") != NULL);

  for (int i = 0; i < 5; i++) {
    ArDialogueToken token;
    CHECK(ArDialogueSession_Next(&session, &token, &error));
    CHECK(token.kind == kArDialogueToken_Grapheme);
  }
  const uint32_t revealed_before_native =
      session.state.revealed_cluster_count;
  ArDialogueContentSelection native_selection =
      Selection(kArDialoguePresentation_NativeRetail, NULL, &native_pack);
  CHECK(ArDialogueSession_Switch(&session, &native_selection, &error));
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_NativeAdapter);
  CHECK(session.state.revealed_cluster_count == revealed_before_native);
  CHECK(ArDialogueSession_Switch(&session, &long_selection, &error));
  CHECK(session.state.revealed_cluster_count == revealed_before_native);

  RevealPage(&session);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_PageComplete);
  /* Contracting while the old source is waiting for its next page must
   * complete the new final page instead of leaving an impossible wait. */
  CHECK(ArDialogueSession_Switch(&session, &short_selection, &error));
  CHECK(!session.state.awaiting_page_advance);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_End);
  CHECK(ArDialogueSession_Switch(&session, &long_selection, &error));
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_End);

  ArDialogueSession_Destroy(&session);
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &long_selection,
                                "dialogue.event.relay.aitos", NULL, &error));
  RevealPage(&session);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_PageComplete);
  CHECK(ArDialogueSession_AdvancePage(&session));
  RevealPage(&session);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_PageComplete);
  CHECK(ArDialogueSession_AdvancePage(&session));
  CHECK(session.state.authored_page_index == 2);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Grapheme);

  CHECK(ArDialogueSession_Switch(&session, &short_selection, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_count == 1);
  CHECK(page.page_index == 0);
  CHECK(page.revealed_cluster_count == page.cluster_count);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_End);

  /* Moving from a shorter, unfinished source to a longer one retains page
   * zero and leaves every additional authored page reachable. */
  ArDialogueSession_Destroy(&session);
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &short_selection,
                                "dialogue.event.relay.aitos", NULL, &error));
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(ArDialogueSession_Switch(&session, &long_selection, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 0 && page.page_count == 3);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&short_pack);
  ArLanguagePack_Destroy(&long_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestOptionalHudRoute(void) {
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "test.hud", "fr-FR", ":: action.hud.player_label\nJOUEUR\n@end\n", &error));
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection selection = Selection(kArDialoguePresentation_Enhanced, &pack, NULL);
  CHECK(ArDialogueSession_Begin(&session, &selection, "action.hud.player_label", NULL, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.utf8_bytes >= 6 && !strncmp(page.utf8, "JOUEUR", 6));
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

static void TestIntentionalEmptySource(void) {
  ArLanguagePack native_pack, empty_pack;
  ArLanguagePack_Init(&native_pack);
  ArLanguagePack_Init(&empty_pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&native_pack, "native.us", "en-US",
                 ":: dialogue.event.relay.aitos\nNative fallback\n", &error));
  CHECK(LoadPack(&empty_pack, "test.empty", "en-CA",
                 ":: dialogue.event.relay.aitos\n@empty\n", &error));
  ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &empty_pack, &native_pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "dialogue.event.relay.aitos", NULL, &error));
  for (uint32_t native_page = 0; native_page < 3; ++native_page) {
    ArDialogueNativeProgress progress = {
        .struct_size = sizeof(progress), .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
        .authored_page_index = native_page, .revealed_unit_count = 3, .page_unit_count = 8,
        .awaiting_page_advance = native_page < 2, .terminal = native_page == 2,
    };
    CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
    ArDialoguePageSnapshot page;
    CHECK(ArDialogueSession_GetPage(&session, &page));
    CHECK(page.utf8 && !page.utf8[0] && !page.utf8_bytes && !page.cluster_count);
    CHECK(!page.revealed_utf8_bytes && !page.revealed_cluster_count);
    CHECK(page.page_count == 1 && page.page_index == 0);
    CHECK(!strcmp(page.package_id, "test.empty"));
    CHECK(session.state.resolved_source == kArDialogueResolvedSource_SelectedPack);
  }
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_End);
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&empty_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestNativeProgressBridge(void) {
  static const char native_script[] =
      ":: dialogue.event.relay.aitos\nNative source\n";
  static const char enhanced_script[] =
      ":: dialogue.event.relay.aitos\n"
      "Alpha\n"
      "@page\n"
      "Éléphant\n"
      "@page\n"
      "Omega\n";
  static const char short_script[] =
      ":: dialogue.event.relay.aitos\nFin\n";
  ArLanguagePack native_pack, enhanced_pack, short_pack;
  ArLanguagePack_Init(&native_pack);
  ArLanguagePack_Init(&enhanced_pack);
  ArLanguagePack_Init(&short_pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&native_pack, "native.us", "en-US", native_script, &error));
  CHECK(LoadPack(&enhanced_pack, "enhanced.fr", "fr-FR", enhanced_script,
                 &error));
  CHECK(LoadPack(&short_pack, "short.fr", "fr-FR", short_script, &error));

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection native_selection =
      Selection(kArDialoguePresentation_NativeRetail, NULL, &native_pack);
  CHECK(ArDialogueSession_Begin(&session, &native_selection,
                                "dialogue.event.relay.aitos", NULL, &error));
  ArDialogueNativeProgress observed = {
      .struct_size = sizeof(observed),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = 1,
      .revealed_unit_count = 2,
      .page_unit_count = 4,
  };
  CHECK(ArDialogueSession_ObserveNativeProgress(&session, &observed, &error));

  ArDialogueContentSelection enhanced_selection = Selection(
      kArDialoguePresentation_Enhanced, &enhanced_pack, &native_pack);
  CHECK(ArDialogueSession_Switch(&session, &enhanced_selection, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 1 && page.page_count == 3);
  CHECK(page.revealed_cluster_count == 4);

  CHECK(ArDialogueSession_Switch(&session, &native_selection, &error));
  observed.authored_page_index = 0;
  observed.revealed_unit_count = 1;
  observed.page_unit_count = 4;
  /* The first observation may remap enhanced cluster units to native glyph
   * units and is therefore allowed to move numerically backwards. */
  CHECK(ArDialogueSession_ObserveNativeProgress(&session, &observed, &error));
  observed.revealed_unit_count = 0;
  CHECK(!ArDialogueSession_ObserveNativeProgress(&session, &observed, &error));
  CHECK(session.state.authored_page_index == 0);

  observed.authored_page_index = 9;
  observed.revealed_unit_count = 1;
  observed.page_unit_count = 1;
  CHECK(ArDialogueSession_ObserveNativeProgress(&session, &observed, &error));
  ArDialogueContentSelection short_selection =
      Selection(kArDialoguePresentation_Enhanced, &short_pack, &native_pack);
  CHECK(ArDialogueSession_Switch(&session, &short_selection, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 0);
  CHECK(page.revealed_cluster_count == page.cluster_count);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&short_pack);
  ArLanguagePack_Destroy(&enhanced_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestFallbackAndTransactionalFailure(void) {
  static const char native_script[] =
      ":: dialogue.event.relay.aitos\nNative enhanced fallback\n";
  static const char missing_script[] =
      ":: dialogue.event.relay.bloodpool\nUnrelated translated message\n";
  static const char malformed_script[] =
      ":: dialogue.event.relay.aitos\nForbidden {master_name}\n";
  ArLanguagePack native_pack, missing_pack, malformed_pack;
  ArLanguagePack_Init(&native_pack);
  ArLanguagePack_Init(&missing_pack);
  ArLanguagePack_Init(&malformed_pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&native_pack, "native.us", "en-US", native_script, &error));
  CHECK(LoadPack(&missing_pack, "community.partial", "ar", missing_script,
                 &error));
  CHECK(LoadPack(&malformed_pack, "community.invalid", "fr-FR",
                 malformed_script, &error));

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection selection = Selection(
      kArDialoguePresentation_Enhanced, &missing_pack, &native_pack);
  CHECK(ArDialogueSession_Begin(&session, &selection, "dialogue.event.relay.aitos",
                                NULL, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(session.state.resolved_source ==
        kArDialogueResolvedSource_NativeEnhanced);
  CHECK(strcmp(page.package_id, "native.us") == 0);
  CHECK(strstr(page.utf8, "fallback") != NULL);
  CHECK(!strcmp(page.locale, "en-US"));
  CHECK(page.direction == kArLanguageDirection_LeftToRight);
  CHECK(ArDialogueSession_GetAuthoredPage(&session, 0, &page));
  CHECK(!strcmp(page.locale, "en-US") && page.direction == kArLanguageDirection_LeftToRight);

  const uint64_t revision = session.state.source_revision;
  const uint32_t revealed = session.state.revealed_cluster_count;
  ArDialogueContentSelection bad = Selection(
      kArDialoguePresentation_Enhanced, &malformed_pack, &native_pack);
  CHECK(!ArDialogueSession_Switch(&session, &bad, &error));
  CHECK(strstr(error.message, "unavailable on this route") != NULL);
  CHECK(session.state.source_revision == revision);
  CHECK(session.state.revealed_cluster_count == revealed);
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(strcmp(page.package_id, "native.us") == 0);

  selection.native_us_enhanced_pack = NULL;
  CHECK(ArDialogueSession_Switch(&session, &selection, &error));
  CHECK(session.state.resolved_source == kArDialogueResolvedSource_NativeRom);

  selection.native_us_enhanced_pack = &native_pack;
  CHECK(ArDialogueSession_Begin(&session, &selection, "dialogue.event.relay.bloodpool",
                                NULL, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(!strcmp(page.locale, "ar") && page.direction == kArLanguageDirection_RightToLeft);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&malformed_pack);
  ArLanguagePack_Destroy(&missing_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestWaitSwitchAndRestore(void) {
  static const char first_script[] =
      ":: dialogue.event.relay.aitos\nAB\n@wait 10\nCD\n";
  static const char second_script[] =
      ":: dialogue.event.relay.aitos\nWX\n@wait 20\nYZ\n";
  ArLanguagePack first, second;
  ArLanguagePack_Init(&first);
  ArLanguagePack_Init(&second);
  ArLanguagePackError error;
  CHECK(LoadPack(&first, "wait.first", "en-US", first_script, &error));
  CHECK(LoadPack(&second, "wait.second", "fr-FR", second_script, &error));
  ArDialogueContentSelection first_selection =
      Selection(kArDialoguePresentation_Enhanced, &first, &first);
  ArDialogueContentSelection second_selection =
      Selection(kArDialoguePresentation_Enhanced, &second, &first);

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &first_selection,
                                "dialogue.event.relay.aitos", NULL, &error));
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Grapheme);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Grapheme);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_WaitStarted && token.wait_frames == 10);
  ArDialogueSession_TickWait(&session, 3);
  CHECK(session.state.wait_frames_remaining == 7);

  ArDialogueContentSelection native_selection =
      Selection(kArDialoguePresentation_NativeRetail, NULL, &first);
  CHECK(ArDialogueSession_Switch(&session, &native_selection, &error));
  ArDialogueNativeProgress native_progress = {
      .struct_size = sizeof(native_progress),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = session.state.authored_page_index,
      .revealed_unit_count = session.state.revealed_cluster_count,
      .page_unit_count = session.state.page_cluster_count,
      .completed_control_count = session.state.completed_control_count,
      .completed_wait_count = session.state.completed_wait_count,
      .wait_frames_remaining = 7,
      .wait_frames_total = 10,
  };
  CHECK(ArDialogueSession_ObserveNativeProgress(&session, &native_progress,
                                                &error));
  CHECK(ArDialogueSession_Switch(&session, &second_selection, &error));
  CHECK(session.state.wait_frames_remaining == 7);
  CHECK(session.state.wait_frames_total == 10);
  CHECK(session.state.completed_wait_count == 1);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Blocked);

  ArDialogueStableState saved;
  CHECK(ArDialogueSession_ExportState(&session, &saved));
  ArDialogueSession_Destroy(&session);
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Restore(&session, &second_selection, &saved, NULL,
                                  &error));
  CHECK(session.state.wait_frames_remaining == 7);
  ArDialogueSession_TickWait(&session, 7);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Grapheme);
  CHECK(token.first_scalar == 'Y');

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&second);
  ArLanguagePack_Destroy(&first);
}

typedef struct ResolverState {
  int calls;
  const char *master_name;
} ResolverState;

static bool ResolveValue(void *context, const char *name,
                         ArLanguagePlaceholderKind expected,
                         ArDialogueValue *value, char *error,
                         size_t error_capacity) {
  ResolverState *state = (ResolverState *)context;
  state->calls++;
  value->kind = expected;
  if (strcmp(name, "master_name") == 0) {
    snprintf(value->text, sizeof(value->text), "%s",
             state->master_name ? state->master_name : "Élise");
    return true;
  }
  if (expected == kArLanguagePlaceholder_Icon) {
    snprintf(value->text, sizeof(value->text), "%s", name);
    return true;
  }
  if (expected == kArLanguagePlaceholder_Number) {
    value->number = 2;
    return true;
  }
  if (expected == kArLanguagePlaceholder_LocalizedTerm) {
    snprintf(value->text, sizeof(value->text), "growth_state.01");
    return true;
  }
  snprintf(error, error_capacity, "unexpected synthetic value");
  return false;
}

static void TestNumberFormatting(void) {
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "format.test", "en-US",
      ":: status.report.master_report\n"
      "{master_level:03}/{master_level:01}/{master_level}\n@end\n", &error));
  ResolverState values = {0};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &values, .resolve = ResolveValue,
  };
  const ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection,
                                "status.report.master_report", &resolver, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(!strcmp(page.utf8, "002/2/2"));
  CHECK(page.bidi_span_count == 3);
  CHECK(page.bidi_spans[0].start == 0 && page.bidi_spans[0].end == 3);
  CHECK(page.bidi_spans[1].start == 4 && page.bidi_spans[1].end == 5);
  CHECK(page.bidi_spans[2].start == 6 && page.bidi_spans[2].end == 7);
  for (size_t i = 0; i < page.bidi_span_count; ++i)
    CHECK(page.bidi_spans[i].direction == kArTextDirection_LeftToRight);
  CHECK((uint32_t)values.calls == ArLanguageContract_AllowedPlaceholderCount(
      "status.report.master_report")); /* One capture per allowed value. */
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
  CHECK(LoadPack(&pack, "format.bad", "en-US",
      ":: status.report.master_report\n{master_name:03}\n@end\n", &error));
  CHECK(!ArLanguageContract_ValidatePack(&pack, NULL, &error));
  ArLanguagePack_Destroy(&pack);
}

static void TestValueSpanBoundaries(void) {
  ArLanguagePack pack, native;
  ArLanguagePack_Init(&pack); ArLanguagePack_Init(&native);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack,"spans.test","ar",
      ":: status.report.master_report\nA{master_name}Z\n@end\n"
      ":: status.report.cities_report\n{city_fillmore_growth_state}|{total_population}\n@end\n",&error));
  CHECK(LoadPack(&native,"spans.native","en-US",
      ":: growth_state.01\nNative term\n@end\n",&error));
  ResolverState values = {.master_name = "\xcc\x81"};
  ArDialogueValueResolver resolver = {.struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION, .context = &values, .resolve = ResolveValue};
  ArDialogueContentSelection selection = Selection(kArDialoguePresentation_Enhanced,&pack,&native);
  ArDialogueSession session; ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session,&selection,"status.report.master_report",&resolver,&error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session,&page));
  CHECK(!strcmp(page.utf8,"A\xcc\x81Z") && page.cluster_count == 2);
  CHECK(page.bidi_span_count == 1 && page.bidi_spans[0].start == 0 && page.bidi_spans[0].end == 3);
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session,&token,&error));
  CHECK(token.kind == kArDialogueToken_Grapheme && token.end_utf8_byte == 3);
  CHECK(ArDialogueSession_Next(&session,&token,&error));
  CHECK(token.kind == kArDialogueToken_Grapheme && token.end_utf8_byte == 4);
  CHECK(ArDialogueSession_Begin(&session,&selection,"status.report.cities_report",&resolver,&error));
  CHECK(ArDialogueSession_GetPage(&session,&page));
  CHECK(!strcmp(page.utf8,"Native term|2") && page.direction == kArLanguageDirection_RightToLeft);
  CHECK(page.bidi_span_count == 2);
  CHECK(page.bidi_spans[0].direction == kArTextDirection_LeftToRight);
  CHECK(page.bidi_spans[0].start == 0 && page.bidi_spans[0].end == 11);
  CHECK(page.bidi_spans[1].start == 12 && page.bidi_spans[1].end == 13);
  ArDialogueSession_Destroy(&session); ArLanguagePack_Destroy(&pack); ArLanguagePack_Destroy(&native);
}

static void TestValueSpanBudget(void) {
  char script[8192];
  size_t bytes = (size_t)snprintf(script, sizeof(script),
      ":: sky.action_mode.confirm\n@anchor reset_text_cursor.00\n");
  for (unsigned i = 0; i < kArTextMaximumBidiSpans; ++i)
    bytes += (size_t)snprintf(script + bytes, sizeof(script) - bytes, "{master_name}");
  const size_t boundary = bytes;
  snprintf(script + bytes, sizeof(script) - bytes, "\n@anchor yield.01\n");
  ArLanguagePack pack, over;
  ArLanguagePack_Init(&pack); ArLanguagePack_Init(&over);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "spans.limit", "en-US", script, &error));
  snprintf(script + boundary, sizeof(script) - boundary,
      "\n@page\n{master_name}\n@anchor yield.01\n");
  CHECK(LoadPack(&over, "spans.over", "en-US", script, &error));
  ResolverState values = {.master_name = "A"};
  const ArDialogueValueResolver resolver = {.struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION, .context = &values, .resolve = ResolveValue};
  ArDialogueContentSelection selection = Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session; ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "sky.action_mode.confirm", &resolver, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.bidi_span_count == kArTextMaximumBidiSpans);
  CHECK(page.cluster_count == kArTextMaximumBidiSpans);
  selection.selected_pack = &over;
  CHECK(!ArDialogueSession_Switch(&session, &selection, &error));
  CHECK(strstr(error.message, "inserted value spans") != NULL);
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(!strcmp(page.package_id, "spans.limit") && page.page_count == 1);
  ArDialogueSession_Destroy(&session); ArLanguagePack_Destroy(&pack); ArLanguagePack_Destroy(&over);
}

static void TestAuthoredBoundaries(void) {
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "boundaries.test", "en-US",
      ":: status.report.master_report\n"
      "{master_name}\n@line\n@line\n@line\n"
      "{master_name}|{master_level}|left|right\n@end\n"
      ":: status.report.cities_report\n{city_fillmore_growth_state}|{total_population}\n@end\n"
      ":: growth_state.00\nMax|growth\n@end\n"
      ":: growth_state.01\n@alias growth_state.00\n",
      &error));
  ResolverState values = {.master_name = "É|li\nse"};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &values, .resolve = ResolveValue};
  const ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  const bool began = ArDialogueSession_Begin(&session, &selection,
      "status.report.master_report", &resolver, &error);
  CHECK(began);
  if (!began) {
    fprintf(stderr, "boundary fixture: %s\n", error.message);
    ArDialogueSession_Destroy(&session);
    ArLanguagePack_Destroy(&pack);
    return;
  }
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(!strcmp(page.utf8, "É|li\nse\n\n\nÉ|li\nse|2|left|right"));
  CHECK(page.bidi_span_count == 3);
  CHECK(page.bidi_spans[0].start == 0 && page.bidi_spans[0].end == strlen("É|li\nse"));
  CHECK(page.bidi_spans[0].direction == kArTextDirection_Auto);
  unsigned pipes = 0, lines = 0, literal_pipes = 0, literal_lines = 0;
  for (size_t i = 0; i < page.utf8_bytes; ++i) {
    const bool boundary = ArTextBoundary_Get(page.structural_boundaries, i);
    if (page.utf8[i] == '|') { if (boundary) ++pipes; else ++literal_pipes; }
    else if (page.utf8[i] == '\n') { if (boundary) ++lines; else ++literal_lines; }
    else CHECK(!boundary);
  }
  CHECK(pipes == 3 && lines == 3 && literal_pipes == 2 && literal_lines == 2);
  ArDialoguePageSnapshot authored;
  CHECK(ArDialogueSession_GetAuthoredPage(&session, 0, &authored));
  CHECK(authored.structural_boundaries == page.structural_boundaries);
  CHECK(authored.bidi_spans == page.bidi_spans && authored.bidi_span_count == page.bidi_span_count);
  CHECK(ArDialogueSession_Begin(&session, &selection,
      "status.report.cities_report", &resolver, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(!strcmp(page.utf8, "Max|growth|2"));
  CHECK(page.bidi_span_count == 2 && page.bidi_spans[0].end == 10);
  CHECK(page.bidi_spans[0].direction == kArTextDirection_LeftToRight);
  for (size_t i = 0; i < page.utf8_bytes; ++i)
    CHECK(ArTextBoundary_Get(page.structural_boundaries, i) == (i == 10));
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

static void TestControlsValuesAndIcons(void) {
  static const char first_script[] =
      ":: sky.action_mode.confirm\n"
      "@anchor reset_text_cursor.00\n"
      "Hello {master_name}\n"
      "@anchor yield.01\n";
  static const char second_script[] =
      ":: sky.action_mode.confirm\n"
      "Texte placé avant le même point sémantique\n"
      "@anchor reset_text_cursor.00\n"
      "Bonjour {master_name}, avec davantage de texte\n"
      "@anchor yield.01\n";
  static const char icon_script[] =
      ":: name_entry.prompt_and_alphabet\n"
      AR_TEST_KEYBOARD_PAGE("É") "@page\n" AR_TEST_KEYBOARD_PAGE("α");
  ArLanguagePack first, second, icons;
  ArLanguagePack_Init(&first);
  ArLanguagePack_Init(&second);
  ArLanguagePack_Init(&icons);
  ArLanguagePackError error;
  CHECK(LoadPack(&first, "control.first", "en-US", first_script, &error));
  CHECK(LoadPack(&second, "control.second", "fr-FR", second_script, &error));
  CHECK(LoadPack(&icons, "icons.test", "en-US", icon_script, &error));
  ResolverState resolver_state = {0};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &resolver_state,
      .resolve = ResolveValue,
  };
  ArDialogueContentSelection first_selection =
      Selection(kArDialoguePresentation_Enhanced, &first, &first);
  ArDialogueContentSelection second_selection =
      Selection(kArDialoguePresentation_Enhanced, &second, &first);

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &first_selection,
                                "sky.action_mode.confirm", &resolver, &error));
  CHECK(resolver_state.calls == 1);
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Control);
  CHECK(token.control_ordinal == 0);
  CHECK(strcmp(token.control_id, "reset_text_cursor.00") == 0);

  /* Switching before native execution/acknowledgement must not lose the
   * pending control. */
  CHECK(ArDialogueSession_Switch(&session, &second_selection, &error));
  CHECK(resolver_state.calls == 1);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Control && token.control_ordinal == 0);
  ArDialogueStableState pending;
  CHECK(ArDialogueSession_ExportState(&session, &pending));
  ArDialogueSession scratch;
  ArDialogueSession_Init(&scratch);
  CHECK(ArDialogueSession_Restore(&scratch, &second_selection, &pending,
                                  &resolver, &error));
  CHECK(ArDialogueSession_Next(&scratch, &token, &error));
  CHECK(token.kind == kArDialogueToken_Control && token.control_ordinal == 0);
  ArDialogueSession_Destroy(&scratch);
  CHECK(ArDialogueSession_CompleteControl(&session, 0));
  CHECK(!ArDialogueSession_CompleteControl(&session, 0));

  for (;;) {
    CHECK(ArDialogueSession_Next(&session, &token, &error));
    if (token.kind == kArDialogueToken_Control)
      break;
    CHECK(token.kind == kArDialogueToken_Grapheme);
  }
  CHECK(token.control_ordinal == 1);
  CHECK(strcmp(token.control_id, "yield.01") == 0);
  CHECK(ArDialogueSession_CompleteControl(&session, 1));
  CHECK(session.state.awaiting_input);
  ArDialogueContentSelection native_selection =
      Selection(kArDialoguePresentation_NativeRetail, NULL, &first);
  CHECK(ArDialogueSession_Switch(&session, &native_selection, &error));
  ArDialogueNativeProgress native_progress = {
      .struct_size = sizeof(native_progress),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = session.state.authored_page_index,
      .revealed_unit_count = session.state.revealed_cluster_count,
      .page_unit_count = session.state.page_cluster_count,
      .completed_control_count = session.state.completed_control_count,
      .completed_wait_count = session.state.completed_wait_count,
  };
  CHECK(ArDialogueSession_ObserveNativeProgress(&session, &native_progress,
                                                &error));
  CHECK(ArDialogueSession_Switch(&session, &first_selection, &error));
  CHECK(!ArDialogueSession_ResumeInput(&session));
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_End);
  CHECK(session.state.completed_control_count == 2);

  ArDialogueSession_Destroy(&session);
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection icon_selection =
      Selection(kArDialoguePresentation_Enhanced, &icons, &icons);
  resolver_state.calls = 0;
  CHECK(ArDialogueSession_Begin(&session, &icon_selection,
                                "name_entry.prompt_and_alphabet", &resolver,
                                &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.inline_object_count == 2);
  CHECK(page.page_count == 2);
  CHECK(strcmp(page.inline_objects[0].id, "icon.name_entry.backspace") == 0);
  CHECK(strcmp(page.inline_objects[1].id, "icon.name_entry.finish") == 0);
  CHECK(strstr(page.utf8, "\xEF\xBF\xBC") != NULL);
  CHECK(ArDialogueSession_GetAuthoredPage(&session, 1, &page));
  CHECK(page.page_index == 1 && strstr(page.utf8, "α") != NULL);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&icons);
  ArLanguagePack_Destroy(&second);
  ArLanguagePack_Destroy(&first);
}

static void TestEnhancedNativeControlProgress(void) {
  const char script[] =
      ":: sky.action_mode.confirm\n"
      "Été\n"
      "@anchor reset_text_cursor.00\n"
      "Après\n"
      "@anchor yield.01\n";
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "control.sync", "fr-FR", script, &error));
  ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ResolverState values = {0};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver), .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &values, .resolve = ResolveValue,
  };
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "sky.action_mode.confirm", &resolver, &error));
  uint32_t page_index = 99;
  size_t offset = 99;
  const ArDialogueStableState original = session.state;
  CHECK(ArDialogueSession_GetControlPosition(&session, 0, &page_index, &offset));
  CHECK(page_index == 0 && offset == strlen("Été"));
  CHECK(!memcmp(&original, &session.state, sizeof(original)));
  CHECK(!ArDialogueSession_GetControlPosition(&session, 2, &page_index, &offset));
  CHECK(page_index == 0 && offset == strlen("Été"));
  ArDialogueNativeProgress progress = {
      .struct_size = sizeof(progress), .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .revealed_unit_count = 1, .page_unit_count = 8, .control_pending = true,
  };
  CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.revealed_utf8_bytes == offset && page.revealed_cluster_count == 3);
  CHECK(session.state.control_pending && !session.state.completed_control_count);
  ArDialogueToken token;
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Control && token.control_ordinal == 0);
  /* Only a proven native completion moves past the clear, even if the page
   * ratio points before its Unicode boundary. Next cannot re-deliver it. */
  progress.completed_control_count = 1;
  progress.control_pending = false;
  CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.revealed_utf8_bytes == offset);
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Grapheme && token.first_scalar == 'A');
  /* Impossible/regressive acknowledgements and wait states are atomic. */
  const ArDialogueSession before = session;
  const ArDialogueNativeProgress valid = progress;
  for (int invalid = 0; invalid < 5; ++invalid) {
    progress = valid;
    if (invalid == 0) progress.completed_control_count = 0;
    if (invalid == 1) progress.completed_control_count = 3;
    if (invalid == 2) progress.wait_frames_remaining = 1;
    if (invalid == 3) progress.terminal = progress.control_pending = true;
    if (invalid == 4) progress.terminal = true; /* Cannot silently drop yield. */
    CHECK(!ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
    CHECK(!memcmp(&before, &session, sizeof(before)));
  }
  progress = valid;
  progress.control_pending = progress.awaiting_input = true;
  CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.revealed_utf8_bytes == page.utf8_bytes);
  CHECK(session.state.completed_control_count == 1);
  CHECK(ArDialogueSession_Switch(&session, &selection, &error));
  CHECK(ArDialogueSession_ResumeInput(&session));
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(token.kind == kArDialogueToken_Control && token.control_ordinal == 1);
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

static void TestCueCannotSplitGrapheme(void) {
  const char *scripts[] = {
      ":: dialogue.event.relay.aitos\ne\n@wait 1\n\xCC\x81\n",
      ":: sky.action_mode.confirm\ne\n@anchor reset_text_cursor.00\n"
      "\xCC\x81\n@anchor yield.01\n",
  };
  const char *ids[] = {"dialogue.event.relay.aitos", "sky.action_mode.confirm"};
  for (size_t index = 0; index < sizeof(scripts) / sizeof(scripts[0]); ++index) {
    ArLanguagePack pack;
    ArLanguagePack_Init(&pack);
    ArLanguagePackError error;
    CHECK(LoadPack(&pack, "split.grapheme", "fr-FR", scripts[index], &error));
    ArDialogueContentSelection selection =
        Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
    ResolverState values = {0};
    const ArDialogueValueResolver resolver = {
        .struct_size = sizeof(resolver), .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
        .context = &values, .resolve = ResolveValue,
    };
    ArDialogueSession session;
    ArDialogueSession_Init(&session);
    CHECK(!ArDialogueSession_Begin(&session, &selection, ids[index], &resolver, &error));
    CHECK(strstr(error.message, "splits a Unicode grapheme"));
    CHECK(!session.state.message_id[0]);
    ArDialogueSession_Destroy(&session);
    ArLanguagePack_Destroy(&pack);
  }
}

static void TestCorruptStateRejected(void) {
  static const char script[] = ":: dialogue.event.relay.aitos\nSafe\n";
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "safe.pack", "en-US", script, &error));
  ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "dialogue.event.relay.aitos",
                                NULL, &error));
  ArDialogueStableState saved;
  CHECK(ArDialogueSession_ExportState(&session, &saved));
  memset(saved.message_id, 'x', sizeof(saved.message_id));
  CHECK(!ArDialogueSession_Restore(&session, &selection, &saved, NULL,
                                   &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(strcmp(page.package_id, "safe.pack") == 0);
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

static void TestEnhancedNativeProgressSynchronization(void) {
  static const char script[] =
      ":: dialogue.event.relay.aitos\n"
      "ABCDE\n"
      "@page\n"
      "XYZ\n"
      "@end\n";
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "sync.pack", "en-US", script, &error));
  ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "dialogue.event.relay.aitos",
                                NULL, &error));

  ArDialogueNativeProgress progress = {
      .struct_size = sizeof(progress),
      .abi_version = AR_DIALOGUE_SESSION_ABI_VERSION,
      .authored_page_index = 0,
      .revealed_unit_count = 2,
      .page_unit_count = 4,
  };
  CHECK(ArDialogueSession_SynchronizeNativeProgress(
      &session, &progress, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 0);
  CHECK(page.revealed_cluster_count == 3); /* ceil(2/4 * 5) */
  CHECK(page.revealed_utf8_bytes == 3);

  ArDialoguePageSnapshot authored;
  CHECK(ArDialogueSession_GetAuthoredPage(&session, 1, &authored));
  CHECK(authored.page_index == 1 && authored.page_count == 2);
  CHECK(authored.revealed_cluster_count == authored.cluster_count);
  CHECK(authored.revealed_utf8_bytes == authored.utf8_bytes);
  CHECK(!memcmp(authored.utf8, "XYZ", 3));
  CHECK(!ArDialogueSession_GetAuthoredPage(&session, 2, &authored));
  /* Reading another authored page never moves the live mapped page. */
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 0 && page.revealed_cluster_count == 3);

  /* A native script with more pages maps to the final translated page. */
  progress.authored_page_index = 9;
  progress.revealed_unit_count = 1;
  progress.page_unit_count = 3;
  CHECK(ArDialogueSession_SynchronizeNativeProgress(
      &session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 1);
  CHECK(page.revealed_cluster_count == 1);

  progress.revealed_unit_count = 4;
  CHECK(!ArDialogueSession_SynchronizeNativeProgress(
      &session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 1 && page.revealed_cluster_count == 1);

  progress.revealed_unit_count = 1;
  progress.awaiting_page_advance = true;
  CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.revealed_cluster_count == page.cluster_count);
  progress.awaiting_page_advance = false;
  progress.terminal = true;
  CHECK(ArDialogueSession_SynchronizeNativeProgress(&session, &progress, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.revealed_utf8_bytes == page.utf8_bytes);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

static void TestPresentationBudget(void) {
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "budget.test", "en-US",
                 ":: dialogue.event.relay.aitos\nÉ\n@page\nZ\n", &error));
  const ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  /* Two bytes for É, one for Z and two page separators: exact-fit succeeds. */
  CHECK(ArDialogueSession_BeginBounded(&session, &selection, "dialogue.event.relay.aitos",
                                       NULL, 5, &error));
  const ArDialogueStableState before = session.state;
  const void *program = session.private_program;
  CHECK(!ArDialogueSession_SwitchBounded(&session, &selection, 4, &error));
  CHECK(strstr(error.message, "presentation budget"));
  CHECK(session.private_program == program &&
        !memcmp(&session.state, &before, sizeof(before)));
  CHECK(!ArDialogueSession_BeginBounded(&session, &selection,
                                        "dialogue.event.relay.aitos", NULL, 4, &error));
  CHECK(session.private_program == program &&
        !memcmp(&session.state, &before, sizeof(before)));
  ArDialogueContentSelection native =
      Selection(kArDialoguePresentation_NativeRetail, NULL, NULL);
  CHECK(ArDialogueSession_SwitchBounded(&session, &native, 1, &error));
  CHECK(session.state.resolved_source == kArDialogueResolvedSource_NativeRom);
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);

  CHECK(LoadPack(&pack, "budget.values", "en-US",
                 ":: sky.action_mode.confirm\n@anchor reset_text_cursor.00\n"
                 "{master_name}\n@wait 7\n@anchor yield.01\n",
                 &error));
  ResolverState state = {0};
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &state,
      .resolve = ResolveValue,
  };
  ArDialogueSession_Init(&session);
  CHECK(!ArDialogueSession_BeginBounded(
      &session, &selection, "sky.action_mode.confirm", &resolver, 1, &error));
  CHECK(strstr(error.message, "presentation budget") && state.calls == 1);
  CHECK(!session.state.message_id[0] && !session.state.wait_frames_remaining);
  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

int main(void) {
  TestOptionalHudRoute();
  TestPresentationBudget();
  TestLongerShorterAndNativeSwitch();
  TestNativeProgressBridge();
  TestIntentionalEmptySource();
  TestFallbackAndTransactionalFailure();
  TestWaitSwitchAndRestore();
  TestControlsValuesAndIcons();
  TestEnhancedNativeControlProgress();
  TestCueCannotSplitGrapheme();
  TestNumberFormatting();
  TestValueSpanBoundaries();
  TestValueSpanBudget();
  TestAuthoredBoundaries();
  TestCorruptStateRejected();
  TestEnhancedNativeProgressSynchronization();
  if (failures) {
    fprintf(stderr, "%d dialogue session test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("dialogue session checks passed");
  return EXIT_SUCCESS;
}
