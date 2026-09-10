#include "localization/dialogue_session.h"

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
      "direction = ltr\n"
      "target = us-runtime\n"
      "source_profile = us\n"
      "fallback = native-us\n"
      "coverage = partial\n"
      "[fonts]\n"
      "primary = builtin:actraiser-sans\n"
      "[scripts]\n"
      "source = text/main.artext\n",
      id, locale, id, id);
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
      ":: action.hud.act_1\nNative source\n";
  static const char long_script[] =
      ":: action.hud.act_1\n"
      "Première page café\n"
      "@page\n"
      "Deuxième page\n"
      "@page\n"
      "Troisième page\n";
  static const char short_script[] =
      ":: action.hud.act_1\nBref\n";
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
                                "action.hud.act_1", NULL, &error));
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
                                "action.hud.act_1", NULL, &error));
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
                                "action.hud.act_1", NULL, &error));
  CHECK(ArDialogueSession_Next(&session, &token, &error));
  CHECK(ArDialogueSession_Switch(&session, &long_selection, &error));
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(page.page_index == 0 && page.page_count == 3);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&short_pack);
  ArLanguagePack_Destroy(&long_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestNativeProgressBridge(void) {
  static const char native_script[] =
      ":: action.hud.act_1\nNative source\n";
  static const char enhanced_script[] =
      ":: action.hud.act_1\n"
      "Alpha\n"
      "@page\n"
      "Éléphant\n"
      "@page\n"
      "Omega\n";
  static const char short_script[] =
      ":: action.hud.act_1\nFin\n";
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
                                "action.hud.act_1", NULL, &error));
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
      ":: action.hud.act_1\nNative enhanced fallback\n";
  static const char missing_script[] =
      ":: action.hud.act_2\nUnrelated translated message\n";
  static const char malformed_script[] =
      ":: action.hud.act_1\nForbidden {master_name}\n";
  ArLanguagePack native_pack, missing_pack, malformed_pack;
  ArLanguagePack_Init(&native_pack);
  ArLanguagePack_Init(&missing_pack);
  ArLanguagePack_Init(&malformed_pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&native_pack, "native.us", "en-US", native_script, &error));
  CHECK(LoadPack(&missing_pack, "community.partial", "fr-FR", missing_script,
                 &error));
  CHECK(LoadPack(&malformed_pack, "community.invalid", "fr-FR",
                 malformed_script, &error));

  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  ArDialogueContentSelection selection = Selection(
      kArDialoguePresentation_Enhanced, &missing_pack, &native_pack);
  CHECK(ArDialogueSession_Begin(&session, &selection, "action.hud.act_1",
                                NULL, &error));
  ArDialoguePageSnapshot page;
  CHECK(ArDialogueSession_GetPage(&session, &page));
  CHECK(session.state.resolved_source ==
        kArDialogueResolvedSource_NativeEnhanced);
  CHECK(strcmp(page.package_id, "native.us") == 0);
  CHECK(strstr(page.utf8, "fallback") != NULL);

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

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&malformed_pack);
  ArLanguagePack_Destroy(&missing_pack);
  ArLanguagePack_Destroy(&native_pack);
}

static void TestWaitSwitchAndRestore(void) {
  static const char first_script[] =
      ":: action.hud.act_1\nAB\n@wait 10\nCD\n";
  static const char second_script[] =
      ":: action.hud.act_1\nWX\n@wait 20\nYZ\n";
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
                                "action.hud.act_1", NULL, &error));
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
} ResolverState;

static bool ResolveValue(void *context, const char *name,
                         ArLanguagePlaceholderKind expected,
                         ArDialogueValue *value, char *error,
                         size_t error_capacity) {
  ResolverState *state = (ResolverState *)context;
  state->calls++;
  value->kind = expected;
  if (strcmp(name, "master_name") == 0) {
    snprintf(value->text, sizeof(value->text), "Élise");
    return true;
  }
  if (expected == kArLanguagePlaceholder_Icon) {
    snprintf(value->text, sizeof(value->text), "%s", name);
    return true;
  }
  snprintf(error, error_capacity, "unexpected synthetic value");
  return false;
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
      "A{icon.name_entry.backspace}B{icon.name_entry.finish}\n";
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
  CHECK(strcmp(page.inline_objects[0].id, "icon.name_entry.backspace") == 0);
  CHECK(strcmp(page.inline_objects[1].id, "icon.name_entry.finish") == 0);
  CHECK(strstr(page.utf8, "\xEF\xBF\xBC") != NULL);

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&icons);
  ArLanguagePack_Destroy(&second);
  ArLanguagePack_Destroy(&first);
}

static void TestCorruptStateRejected(void) {
  static const char script[] = ":: action.hud.act_1\nSafe\n";
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error;
  CHECK(LoadPack(&pack, "safe.pack", "en-US", script, &error));
  ArDialogueContentSelection selection =
      Selection(kArDialoguePresentation_Enhanced, &pack, &pack);
  ArDialogueSession session;
  ArDialogueSession_Init(&session);
  CHECK(ArDialogueSession_Begin(&session, &selection, "action.hud.act_1",
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
      ":: action.hud.act_1\n"
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
  CHECK(ArDialogueSession_Begin(&session, &selection, "action.hud.act_1",
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

  ArDialogueSession_Destroy(&session);
  ArLanguagePack_Destroy(&pack);
}

int main(void) {
  TestLongerShorterAndNativeSwitch();
  TestNativeProgressBridge();
  TestFallbackAndTransactionalFailure();
  TestWaitSwitchAndRestore();
  TestControlsValuesAndIcons();
  TestCorruptStateRejected();
  TestEnhancedNativeProgressSynchronization();
  if (failures) {
    fprintf(stderr, "%d dialogue session test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("dialogue session checks passed");
  return EXIT_SUCCESS;
}
