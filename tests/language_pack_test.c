#include "localization/language_pack.h"
#include "localization/language_contract.h"
#include "fixtures/keyboard_body.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestFile {
  const char *path;
  const uint8_t *data;
  size_t size;
} TestFile;

typedef struct TestVfs {
  const TestFile *files;
  size_t file_count;
  uint32_t read_count;
  uint32_t release_count;
} TestVfs;

static int failures;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,         \
              #condition);                                                      \
      failures++;                                                               \
    }                                                                            \
  } while (0)

static bool TestRead(void *context, const char *path, size_t maximum_bytes,
                     ArLanguagePackBlob *blob, char *error,
                     size_t error_capacity) {
  TestVfs *vfs = (TestVfs *)context;
  vfs->read_count++;
  for (size_t i = 0; i < vfs->file_count; i++) {
    if (strcmp(vfs->files[i].path, path) != 0)
      continue;
    if (vfs->files[i].size > maximum_bytes) {
      snprintf(error, error_capacity, "test file exceeds limit");
      return false;
    }
    blob->struct_size = sizeof(*blob);
    blob->data = vfs->files[i].data;
    blob->size = vfs->files[i].size;
    blob->token = i + 1;
    return true;
  }
  snprintf(error, error_capacity, "test file was not found");
  return false;
}

static void TestRelease(void *context, ArLanguagePackBlob *blob) {
  TestVfs *vfs = (TestVfs *)context;
  vfs->release_count++;
  memset(blob, 0, sizeof(*blob));
}

static ArLanguagePackIo MakeIo(TestVfs *vfs) {
  ArLanguagePackIo io = {0};
  io.struct_size = sizeof(io);
  io.abi_version = AR_LANGUAGE_PACK_IO_ABI_VERSION;
  io.context = vfs;
  io.read_file = TestRead;
  io.release_file = TestRelease;
  return io;
}

static const char kManifest[] =
    "[pack]\n"
    "format = actraiser-language-pack\n"
    "version = 1\n"
    "id = example.fr-ca\n"
    "locale = fr-CA\n"
    "name = Canadian French\n"
    "autonym = Français canadien\n"
    "author = Example Author\n"
    "license = CC-BY-4.0\n"
    "direction = auto\n"
    "target = us-runtime\n"
    "source_profile = us\n"
    "fallback = native-us\n"
    "coverage = partial\n"
    "description = Runtime parser test\n"
    "\n"
    "[fonts]\n"
    "primary = builtin:actraiser-sans\n"
    "fallback = fonts/Fallback.ttf\n"
    "\n"
    "[scripts]\n"
    "source = text/sky.artext\n";

static const char kScript[] =
    "# Human-authored UTF-8 fixture; no retail text.\n"
    ":: sky.demo\n"
    "@anchor reset_text_cursor.00\n"
    "Bienvenue, {master_name}.\n"
    "Cette ligne continue.  \n"
    "\n"
    "Un paragraphe avec {{accolades}}.\n"
    "@page\n"
    "Une page ajoutée.\n"
    "@wait 30\n"
    "@end\n"
    "\n"
    ":: sky.empty\n"
    "@anchor yield.00\n"
    "@empty\n"
    "\n"
    ":: sky.alias\n"
    "@alias sky.demo\n"
    "\n"
    ":: sky.escaped\n"
    "@@visible command\n"
    "\\#visible comment\n";

static const uint8_t kFont[] = {0, 1, 2, 3};

static TestVfs GoodVfs(void) {
  static const TestFile files[] = {
      {"packs/example/pack.ini", (const uint8_t *)kManifest,
       sizeof(kManifest) - 1},
      {"packs/example/text/sky.artext", (const uint8_t *)kScript,
       sizeof(kScript) - 1},
      {"packs/example/fonts/Fallback.ttf", kFont, sizeof(kFont)},
  };
  TestVfs result = {files, sizeof(files) / sizeof(files[0]), 0, 0};
  return result;
}

static const ArLanguageOperation *FindOperation(
    const ArLanguagePack *pack, const ArLanguageMessage *message,
    ArLanguageOperationKind kind) {
  for (uint32_t i = 0; i < message->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(pack, message, i);
    if (operation && operation->kind == kind)
      return operation;
  }
  return NULL;
}

/* One host-path contract, exercised in the shapes a Windows or network host
 * hands us. Pack-internal members stay portable, so the joined result may mix
 * separators; Win32 accepts that and POSIX never sees these prefixes. */
static void TestMemberPathResolution(void) {
  static const struct {
    const char *manifest;
    const char *expected;
  } cases[] = {
      {"packs/example/pack.ini", "packs/example/script/main.txt"},
      {"pack.ini", "script/main.txt"},
      {"C:\\packs\\example\\pack.ini", "C:\\packs\\example\\script/main.txt"},
      {"C:/packs/example/pack.ini", "C:/packs/example/script/main.txt"},
      {"C:\\packs/example\\pack.ini", "C:\\packs/example\\script/main.txt"},
      {"C:pack.ini", "C:script/main.txt"},
      {"\\\\server\\share\\my packs\\pack.ini",
       "\\\\server\\share\\my packs\\script/main.txt"},
      {"packs/\u00e9t\u00e9 2026/pack.ini", "packs/\u00e9t\u00e9 2026/script/main.txt"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char resolved[kArLanguageFontPathCapacity];
    CHECK(ArLanguagePack_ResolveMemberPath(cases[i].manifest, "script/main.txt",
                                           resolved, sizeof(resolved)));
    if (strcmp(resolved, cases[i].expected) != 0) {
      fprintf(stderr, "%s:%d: %s resolved to %s, expected %s\n", __FILE__,
              __LINE__, cases[i].manifest, resolved, cases[i].expected);
      failures++;
    }
  }
  char small[8];
  CHECK(!ArLanguagePack_ResolveMemberPath("packs/example/pack.ini",
                                          "script/main.txt", small,
                                          sizeof(small)));
  CHECK(!ArLanguagePack_ResolveMemberPath(NULL, "x", small, sizeof(small)));
}

static void TestMetadataFastPath(void) {
  TestVfs vfs = GoodVfs();
  ArLanguagePackIo io = MakeIo(&vfs);
  ArLanguagePackMetadata metadata;
  ArLanguagePackError error;
  uint64_t revision = 0;
  CHECK(ArLanguagePack_ReadMetadata(&io, "packs/example/pack.ini", &metadata,
                                    &revision, &error));
  CHECK(error.message[0] == 0);
  CHECK(vfs.read_count == 1);
  CHECK(vfs.release_count == 1);
  CHECK(revision != 0);
  CHECK(strcmp(metadata.package_id, "example.fr-ca") == 0);
  CHECK(strcmp(metadata.locale, "fr-CA") == 0);
  CHECK(strcmp(metadata.autonym, "Français canadien") == 0);
  CHECK(metadata.target == kArLanguagePackTarget_UsRuntime);
  CHECK(metadata.source_profile == kArLanguageSourceProfile_Us);
  CHECK(metadata.fallback_font_count == 1);
}

static void TestFullLoad(void) {
  TestVfs vfs = GoodVfs();
  ArLanguagePackIo io = MakeIo(&vfs);
  ArLanguagePackError error;
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  CHECK(ArLanguagePack_Load(&pack, &io, "packs/example/pack.ini", &error));
  CHECK(error.message[0] == 0);
  CHECK(vfs.read_count == 3);
  CHECK(vfs.read_count == vfs.release_count);
  CHECK(pack.content_revision != 0);
  CHECK(ArLanguagePack_MessageCount(&pack) == 4);

  const ArLanguageMessage *demo =
      ArLanguagePack_FindMessage(&pack, "sky.demo");
  CHECK(demo != NULL);
  const ArLanguageOperation *placeholder =
      FindOperation(&pack, demo, kArLanguageOperation_Placeholder);
  CHECK(placeholder != NULL);
  CHECK(strcmp(ArLanguagePack_GetString(&pack, placeholder->value.placeholder),
               "master_name") == 0);
  const ArLanguageOperation *wait =
      FindOperation(&pack, demo, kArLanguageOperation_WaitFrames);
  CHECK(wait && wait->value.wait_frames == 30);
  CHECK(FindOperation(&pack, demo, kArLanguageOperation_ParagraphBreak));
  CHECK(FindOperation(&pack, demo, kArLanguageOperation_PageBreak));
  const ArLanguageOperation *end =
      ArLanguagePack_GetOperation(&pack, demo, demo->operation_count - 1);
  CHECK(end && end->kind == kArLanguageOperation_End);

  bool preserved_trailing_spaces = false;
  for (uint32_t i = 0; i < demo->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(&pack, demo, i);
    if (operation->kind != kArLanguageOperation_Text)
      continue;
    const char *text = ArLanguagePack_GetString(&pack, operation->value.text);
    if (strstr(text, "continue.  "))
      preserved_trailing_spaces = true;
  }
  CHECK(preserved_trailing_spaces);

  const ArLanguageMessage *empty =
      ArLanguagePack_FindMessage(&pack, "sky.empty");
  CHECK(empty && FindOperation(&pack, empty, kArLanguageOperation_Empty));
  const ArLanguageMessage *alias =
      ArLanguagePack_FindMessage(&pack, "sky.alias");
  CHECK(alias && alias->is_alias);
  CHECK(strcmp(ArLanguagePack_GetString(&pack, alias->alias), "sky.demo") ==
        0);
  const ArLanguageMessage *escaped =
      ArLanguagePack_FindMessage(&pack, "sky.escaped");
  CHECK(escaped != NULL);
  char escaped_value[128] = {0};
  size_t escaped_size = 0;
  for (uint32_t i = 0; i < escaped->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(&pack, escaped, i);
    if (operation->kind != kArLanguageOperation_Text)
      continue;
    const char *part =
        ArLanguagePack_GetString(&pack, operation->value.text);
    const size_t part_size = strlen(part);
    CHECK(escaped_size + part_size < sizeof(escaped_value));
    memcpy(escaped_value + escaped_size, part, part_size + 1);
    escaped_size += part_size;
  }
  CHECK(strcmp(escaped_value, "@visible command #visible comment") == 0);
  ArLanguagePack_Destroy(&pack);
}

static void ExpectFailure(const char *manifest, const uint8_t *script,
                          size_t script_size, bool include_font,
                          const char *expected) {
  TestFile files[3] = {
      {"pack.ini", (const uint8_t *)manifest, strlen(manifest)},
      {"text/sky.artext", script, script_size},
      {"fonts/Fallback.ttf", kFont, sizeof(kFont)},
  };
  TestVfs vfs = {files, include_font ? 3 : 2, 0, 0};
  ArLanguagePackIo io = MakeIo(&vfs);
  ArLanguagePackError error;
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  CHECK(!ArLanguagePack_Load(&pack, &io, "pack.ini", &error));
  CHECK(strstr(error.message, expected) != NULL);
  CHECK(vfs.release_count <= vfs.read_count);
  CHECK(vfs.read_count - vfs.release_count <= 1);
  CHECK(ArLanguagePack_MessageCount(&pack) == 0);
  ArLanguagePack_Destroy(&pack);
}

static void TestRejectedInputs(void) {
  static const char invalid_utf8[] = ":: sky.demo\n\xC0\xAF\n";
  ExpectFailure(kManifest, (const uint8_t *)invalid_utf8,
                sizeof(invalid_utf8) - 1, true, "valid UTF-8");

  static const char unknown[] = ":: sky.demo\n@cursor 1 2\n";
  ExpectFailure(kManifest, (const uint8_t *)unknown, sizeof(unknown) - 1, true,
                "unknown command");

  static const char long_wait[] = ":: sky.demo\nText\n@wait 601\n";
  ExpectFailure(kManifest, (const uint8_t *)long_wait,
                sizeof(long_wait) - 1, true, "@wait must be");

  static const char cycle[] =
      ":: sky.one\n@alias sky.two\n:: sky.two\n@alias sky.one\n";
  ExpectFailure(kManifest, (const uint8_t *)cycle, sizeof(cycle) - 1, true,
                "alias cycle");

  static const char quoted_anchor[] =
      ":: sky.demo\n@anchor \"reset_text_cursor\\.00\"\nHello\n";
  ExpectFailure(kManifest, (const uint8_t *)quoted_anchor,
                sizeof(quoted_anchor) - 1, true, "stable anchor id");

  ExpectFailure(kManifest, (const uint8_t *)kScript, sizeof(kScript) - 1,
                false, "Fallback.ttf");

  char unsafe_manifest[sizeof(kManifest) + 32];
  strcpy(unsafe_manifest, kManifest);
  char *source = strstr(unsafe_manifest, "text/sky.artext");
  CHECK(source != NULL);
  memcpy(source, "../x/sky.artext", strlen("../x/sky.artext"));
  ExpectFailure(unsafe_manifest, (const uint8_t *)kScript,
                sizeof(kScript) - 1, true, "portable and relative");

  static const char *const bad_fonts[] = {
      "font.ttf:stream", "C:font.ttf", "fonts/./font.ttf", "NUL.ttf",
      "nul .ttf", "LPT\xC2\xB3.ttf", "fonts/end./font.ttf", "bad?.ttf"};
  const char *font = strstr(kManifest, "fonts/Fallback.ttf");
  CHECK(font != NULL);
  for (size_t i = 0; i < sizeof(bad_fonts) / sizeof(bad_fonts[0]); i++) {
    const int length = snprintf(unsafe_manifest, sizeof(unsafe_manifest),
        "%.*s%s%s", (int)(font - kManifest), kManifest, bad_fonts[i],
        font + strlen("fonts/Fallback.ttf"));
    CHECK(length > 0 && (size_t)length < sizeof(unsafe_manifest));
    ExpectFailure(unsafe_manifest, (const uint8_t *)kScript,
                  sizeof(kScript) - 1, true, "portable and relative");
  }
}

static void TestTransactionalReload(void) {
  TestVfs vfs = GoodVfs();
  ArLanguagePackIo io = MakeIo(&vfs);
  ArLanguagePackError error;
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  CHECK(ArLanguagePack_Load(&pack, &io, "packs/example/pack.ini", &error));
  const uint64_t revision = pack.content_revision;
  CHECK(!ArLanguagePack_Load(&pack, &io, "packs/missing/pack.ini", &error));
  CHECK(pack.content_revision == revision);
  CHECK(ArLanguagePack_MessageCount(&pack) == 4);
  CHECK(ArLanguagePack_FindMessage(&pack, "sky.demo") != NULL);
  ArLanguagePack_Destroy(&pack);
}

static bool LoadContractPack(const char *script, const char *coverage,
                             ArLanguagePack *pack,
                             ArLanguagePackError *error) {
  char manifest[1024];
  const int length = snprintf(
      manifest, sizeof(manifest),
      "[pack]\n"
      "format = actraiser-language-pack\n"
      "version = 1\n"
      "id = test.contract\n"
      "locale = en-US\n"
      "name = Contract Test\n"
      "autonym = Contract Test\n"
      "author = Test Author\n"
      "license = MIT\n"
      "direction = ltr\n"
      "target = us-runtime\n"
      "source_profile = us\n"
      "fallback = native-us\n"
      "coverage = %s\n"
      "[fonts]\n"
      "primary = builtin:actraiser-sans\n"
      "[scripts]\n"
      "source = text/source.artext\n",
      coverage);
  CHECK(length > 0 && (size_t)length < sizeof(manifest));
  const TestFile files[] = {
      {"pack.ini", (const uint8_t *)manifest, (size_t)length},
      {"text/source.artext", (const uint8_t *)script, strlen(script)},
  };
  TestVfs vfs = {files, sizeof(files) / sizeof(files[0]), 0, 0};
  ArLanguagePackIo io = MakeIo(&vfs);
  return ArLanguagePack_Load(pack, &io, "pack.ini", error);
}

static void ExpectContractFailure(const char *script, const char *expected) {
  ArLanguagePack pack;
  ArLanguagePackError error;
  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(script, "partial", &pack, &error));
  CHECK(!ArLanguageContract_ValidatePack(&pack, NULL, &error));
  CHECK(strstr(error.message, expected) != NULL);
  ArLanguagePack_Destroy(&pack);
}

/* Fixed menus, cards and labels display exactly what their native surface
 * reserves. Content past that is not a style choice; the game never shows it,
 * so the pack must be rejected rather than silently truncated. */
static void TestPresentationContracts(void) {
  ArLanguagePresentationContract shape;
  CHECK(ArLanguageContract_Presentation("action.hud.act_1", &shape) &&
        shape.shape == kArLanguagePresentation_Fixed &&
        shape.maximum_pages == 1 && shape.maximum_lines == 0);
  CHECK(ArLanguageContract_Presentation("title.save_choice.labels", &shape) &&
        shape.required_nonempty_lines == 2);
  CHECK(ArLanguageContract_Presentation("name_entry.prompt_and_alphabet",
                                        &shape) &&
        shape.shape == kArLanguagePresentation_Keyboard &&
        shape.maximum_pages == 0);
  CHECK(ArLanguageContract_Presentation("town.name.aitos", &shape) &&
        shape.shape == kArLanguagePresentation_Inline &&
        shape.maximum_lines == 1);
  CHECK(ArLanguageContract_Presentation("dialogue.event.relay.aitos",
                                        &shape) &&
        shape.shape == kArLanguagePresentation_Flow &&
        shape.maximum_pages == 0 && shape.maximum_lines == 0);
  CHECK(!ArLanguageContract_Presentation("community.unknown", &shape));

  ExpectContractFailure(
      ":: action.hud.act_1\nFirst card.\n@page\nNever displayed.\n",
      "pages beyond that are never shown");
  ExpectContractFailure(
      ":: action.hud.act_label\nToo\n@line\ntall\n",
      "reserves 1 line(s); the message has 2");
  CHECK(ArLanguageContract_Presentation("action.hud.act_label", &shape) &&
        shape.maximum_lines == 1);
  ExpectContractFailure(
      ":: town.name.aitos\nAitos\n@line\nover two rows\n",
      "inline term reserves 1 line(s)");
  ExpectContractFailure(
      ":: title.save_choice.labels\nContinue\n@line\nNew game\n@line\n"
      "A third option\n",
      "exactly 2 choice(s); the message has 3");
  ExpectContractFailure(
      ":: title.save_choice.labels\nContinue\n",
      "exactly 2 choice(s); the message has 1");

  /* What must keep working: documented empties, native blank spacer rows,
   * multipage keyboards, and dialogue of any length. */
  static const char accepted[] =
      ":: title.save_choice.labels\n@empty\n"
      ":: title.mode_select.with_save\nContinue\n@line\n@line\nNew game\n"
      ":: name_entry.prompt_and_alphabet\n"
      AR_TEST_KEYBOARD_PAGE("É") "@page\n" AR_TEST_KEYBOARD_PAGE("α")
      ":: dialogue.event.relay.aitos\nOne\n@page\nTwo\n@page\nThree\n"
      ":: action.hud.act_1\nACT\n";
  ArLanguagePack pack;
  ArLanguagePackError error;
  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(accepted, "partial", &pack, &error));
  CHECK(ArLanguageContract_ValidatePack(&pack, NULL, &error));
  ArLanguagePack_Destroy(&pack);
}

/* Loading indexes messages once, so a long alias chain costs one pass rather
 * than one full-pack scan per hop. This checks the outcomes, not the timing:
 * the same chains, cycles and missing targets must still be diagnosed. */
static void TestAliasChainsAndLookup(void) {
  static const char *const chain[] = {
      "dialogue.event.relay.aitos",       "dialogue.event.relay.bloodpool",
      "dialogue.event.relay.fillmore",    "dialogue.event.relay.kasandora",
      "dialogue.event.relay.marahna",     "dialogue.event.relay.northwall",
      "sim.miracle.earthquake.cancel",    "sim.miracle.lightning.target_cancel",
      "sim.miracle.lightning.target_prompt", "sim.miracle.rain.target_cancel",
      "sim.miracle.rain.target_prompt",   "sim.miracle.sun.target_cancel",
      "sim.miracle.sun.target_prompt",    "sim.miracle.wind.cancel",
      "sim.offerings.cancelled",          "sim.offerings.completed",
  };
  const size_t links = sizeof(chain) / sizeof(chain[0]);
  char script[4096];
  size_t used = 0;
  for (size_t i = 0; i + 1 < links; i++)
    used += (size_t)snprintf(script + used, sizeof(script) - used,
                             ":: %s\n@alias %s\n", chain[i], chain[i + 1]);
  used += (size_t)snprintf(script + used, sizeof(script) - used,
                           ":: %s\nThe end of the chain.\n", chain[links - 1]);
  CHECK(used < sizeof(script));

  ArLanguagePack pack;
  ArLanguagePackError error;
  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(script, "partial", &pack, &error));
  /* Every link resolves to the one body, and lookup finds each by id. */
  for (size_t i = 0; i < links; i++) {
    const ArLanguageMessage *message =
        ArLanguagePack_FindMessage(&pack, chain[i]);
    CHECK(message != NULL);
    CHECK(message == NULL || message->is_alias == (i + 1 < links));
  }
  CHECK(!ArLanguagePack_FindMessage(&pack, "dialogue.event.relay.missing"));
  ArLanguagePack_Destroy(&pack);

  /* An operation pointer from another pack is refused without a scan. */
  ArLanguagePack other;
  ArLanguagePack_Init(&other);
  CHECK(LoadContractPack(":: dialogue.event.relay.aitos\nOnly message.\n",
                         "partial", &other, &error));
  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(script, "partial", &pack, &error));
  const ArLanguageMessage *foreign =
      ArLanguagePack_FindMessage(&other, "dialogue.event.relay.aitos");
  CHECK(foreign && !ArLanguagePack_GetOperation(&pack, foreign, 0));
  CHECK(ArLanguagePack_GetOperation(&other, foreign, 0) != NULL);
  ArLanguagePack_Destroy(&pack);
  ArLanguagePack_Destroy(&other);

  char cycle[4096];
  used = 0;
  for (size_t i = 0; i < links; i++)
    used += (size_t)snprintf(cycle + used, sizeof(cycle) - used,
                             ":: %s\n@alias %s\n", chain[i],
                             chain[(i + 1) % links]);
  CHECK(used < sizeof(cycle));
  ArLanguagePack_Init(&pack);
  CHECK(!LoadContractPack(cycle, "partial", &pack, &error));
  CHECK(strstr(error.message, "alias cycle detected") != NULL);
  ArLanguagePack_Destroy(&pack);

  ArLanguagePack_Init(&pack);
  CHECK(!LoadContractPack(":: dialogue.event.relay.aitos\n"
                          "@alias dialogue.event.relay.bloodpool\n",
                          "partial", &pack, &error));
  CHECK(strstr(error.message, "is not included in this pack") != NULL);
  ArLanguagePack_Destroy(&pack);

  ArLanguagePack_Init(&pack);
  CHECK(!LoadContractPack(":: dialogue.event.relay.aitos\nOne.\n"
                          ":: dialogue.event.relay.aitos\nTwo.\n",
                          "partial", &pack, &error));
  CHECK(strstr(error.message, "duplicate message") != NULL);
  ArLanguagePack_Destroy(&pack);
}

static void TestSemanticContracts(void) {
  static const char valid[] =
      ":: action.hud.act_1\n"
      "Authored test text.\n"
      ":: action.hud.act_2\n"
      "@alias action.hud.act_1\n"
      ":: dialogue.event.wrapper_00.call_03.source_00\n"
      "@anchor reset_text_cursor.00\n"
      "Hello, {town_name}.\n";
  ArLanguagePack pack;
  ArLanguagePackError error;
  ArLanguageContractStats stats;
  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(valid, "partial", &pack, &error));
  CHECK(ArLanguageContract_ValidatePack(&pack, &stats, &error));
  CHECK(stats.validated_messages == 3);
  CHECK(stats.aliases == 1);
  CHECK(stats.required_messages == 495);
  CHECK(ArLanguageContract_RouteCount() == 558);
  CHECK(strcmp(ArLanguageContract_RouteId(0), "action.hud.act_1") == 0);
  CHECK(ArLanguageContract_RouteId(558) == NULL);
  CHECK(ArLanguageContract_RouteAvailable("action.hud.act_1",
                                          kArLanguageSourceProfile_Us));
  CHECK(!ArLanguageContract_RouteAvailable(
      "dialogue.offering.slot_20", kArLanguageSourceProfile_Us));
  CHECK(ArLanguageContract_RouteAvailable(
      "dialogue.offering.slot_20", kArLanguageSourceProfile_Japanese));
  CHECK(ArLanguageContract_AllowedPlaceholderCount(
            "dialogue.event.wrapper_00.call_03.source_00") == 2);
  CHECK(ArLanguageContract_PlaceholderKind("town_name") ==
        kArLanguagePlaceholder_LocalizedText);
  CHECK(ArLanguageContract_PlaceholderKind("lair_count") ==
        kArLanguagePlaceholder_Number);
  CHECK(ArLanguageContract_PlaceholderKind("not_a_value") ==
        kArLanguagePlaceholder_Unknown);
  ArLanguagePack_Destroy(&pack);

  static const char unknown_route[] =
      ":: community.unknown\nAuthored text.\n";
  ExpectContractFailure(unknown_route, "unknown semantic message");

  static const char bad_placeholder[] =
      ":: dialogue.event.wrapper_00.call_03.source_00\n"
      "@anchor reset_text_cursor.00\n"
      "Hello, {lair_count}.\n";
  ExpectContractFailure(bad_placeholder, "unavailable on this route");

  static const char missing_anchor[] =
      ":: dialogue.event.wrapper_00.call_03.source_00\n"
      "Hello, {town_name}.\n";
  ExpectContractFailure(missing_anchor, "locked anchors changed");

  static const char after_yield[] =
      ":: sky.action_mode.confirm\n"
      "@anchor reset_text_cursor.00\nReady?\n@anchor yield.01\n"
      "@page\nUnreachable text.\n";
  ExpectContractFailure(after_yield, "content after a menu yield is unreachable");

  static const char event[] =
      ":: action.hud.act_1\n"
      "@event mutate_game forbidden\n";
  ExpectContractFailure(event, "not allow-listed");

  static const char incompatible_alias[] =
      ":: action.hud.act_1\n"
      "@alias dialogue.event.wrapper_00.call_03.source_00\n"
      ":: dialogue.event.wrapper_00.call_03.source_00\n"
      "@anchor reset_text_cursor.00\n"
      "Hello, {town_name}.\n";
  ExpectContractFailure(incompatible_alias, "locked anchors changed");

  ArLanguagePack_Init(&pack);
  CHECK(LoadContractPack(valid, "complete", &pack, &error));
  CHECK(!ArLanguageContract_ValidatePack(&pack, NULL, &error));
  CHECK(strstr(error.message, "complete pack is missing") != NULL);
  ArLanguagePack_Destroy(&pack);
}

static void PrintJsonString(const char *value) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if (*p == '"' || *p == '\\')
      printf("\\%c", *p);
    else if (*p < 0x20)
      printf("\\u%04x", (unsigned)*p);
    else
      putchar(*p);
  }
  putchar('"');
}

/* Development-only probe of the production loader and contract validator.
 * No second parser or game executable/ROM is involved. */
static void DumpPack(const ArLanguagePack *pack) {
  static const char *const kinds[] = {
      "text", "placeholder", "line", "paragraph", "page", "wait",
      "anchor", "event", "empty", "end"};
  putchar('[');
  for (uint32_t i = 0; i < ArLanguagePack_MessageCount(pack); i++) {
    const ArLanguageMessage *message = ArLanguagePack_GetMessage(pack, i);
    if (i) putchar(',');
    printf("{\"id\":");
    PrintJsonString(ArLanguagePack_GetString(pack, message->id));
    printf(",\"source_line\":%u", message->source_line);
    if (message->is_alias) {
      printf(",\"alias\":");
      PrintJsonString(ArLanguagePack_GetString(pack, message->alias));
    }
    printf(",\"operations\":[");
    for (uint32_t j = 0; j < message->operation_count; j++) {
      const ArLanguageOperation *op = ArLanguagePack_GetOperation(pack, message, j);
      if (j) putchar(',');
      printf("{\"op\":");
      PrintJsonString(kinds[op->kind]);
      printf(",\"source_line\":%u", op->source_line);
      if (op->kind == kArLanguageOperation_Text) {
        printf(",\"value\":");
        PrintJsonString(ArLanguagePack_GetString(pack, op->value.text));
      } else if (op->kind == kArLanguageOperation_Placeholder) {
        printf(",\"name\":");
        PrintJsonString(ArLanguagePack_GetString(pack, op->value.placeholder));
        if (op->minimum_digits)
          printf(",\"minimum_digits\":%u", op->minimum_digits);
      } else if (op->kind == kArLanguageOperation_Anchor) {
        printf(",\"id\":");
        PrintJsonString(ArLanguagePack_GetString(pack, op->value.anchor));
      } else if (op->kind == kArLanguageOperation_WaitFrames) {
        printf(",\"frames\":%u", op->value.wait_frames);
      }
      putchar('}');
    }
    printf("]}");
  }
  puts("]");
}

static void DumpMetadata(const ArLanguagePackMetadata *m) {
  static const char *const directions[] = {"auto", "ltr", "rtl"};
  static const char *const targets[] = {"us-runtime", "reference-only"};
  static const char *const profiles[] = {"us", "eu-en", "de", "fr", "jp"};
  static const char *const coverages[] = {"partial", "complete"};
  printf("{\"metadata\":{");
#define FIELD(name, value) do { printf("\"%s\":", name); PrintJsonString(value); } while (0)
  FIELD("id", m->package_id); printf(",");
  FIELD("locale", m->locale); printf(",");
  FIELD("name", m->display_name); printf(",");
  FIELD("autonym", m->autonym); printf(",");
  FIELD("author", m->author); printf(",");
  FIELD("license", m->license); printf(",");
  FIELD("direction", directions[m->direction]); printf(",");
  FIELD("target", targets[m->target]); printf(",");
  FIELD("source_profile", profiles[m->source_profile]); printf(",");
  FIELD("fallback", m->fallback); printf(",");
  FIELD("coverage", coverages[m->coverage]);
  printf("},\"fonts\":{");
  FIELD("primary", m->primary_font);
  printf(",\"fallback\":[");
  for (uint32_t i = 0; i < m->fallback_font_count; i++) {
    if (i) printf(",");
    PrintJsonString(m->fallback_fonts[i]);
  }
  printf("]}");
#undef FIELD
}

static int ValidatePackFromFile(const char *manifest_path, int mode) {
  ArLanguagePackIo io;
  ArLanguagePackFileIo_Init(&io);
  ArLanguagePackError error;
  if (mode == 3) {
    ArLanguagePackMetadata metadata;
    uint64_t revision;
    if (!ArLanguagePack_ReadMetadata(&io, manifest_path, &metadata, &revision, &error)) {
      fprintf(stderr, "%s\n", error.message);
      return EXIT_FAILURE;
    }
    DumpMetadata(&metadata);
    printf(",\"revision\":\"%016llx\"}\n", (unsigned long long)revision);
    return EXIT_SUCCESS;
  }
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  if (!ArLanguagePack_Load(&pack, &io, manifest_path, &error)) {
    fprintf(stderr, "%s\n", error.message);
    ArLanguagePack_Destroy(&pack);
    return EXIT_FAILURE;
  }
  ArLanguageContractStats stats;
  if (!ArLanguageContract_ValidatePack(&pack, &stats, &error)) {
    fprintf(stderr, "%s\n", error.message);
    ArLanguagePack_Destroy(&pack);
    return EXIT_FAILURE;
  }
  if (mode == 2) {
    DumpMetadata(ArLanguagePack_GetMetadata(&pack));
    printf(",\"revision\":\"%016llx\",\"messages\":",
           (unsigned long long)pack.content_revision);
    DumpPack(&pack);
    puts("}");
  } else if (mode == 1)
    DumpPack(&pack);
  else
    printf("validated %u messages (%u aliases; %u required)\n",
           stats.validated_messages, stats.aliases, stats.required_messages);
  ArLanguagePack_Destroy(&pack);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc == 2)
    return ValidatePackFromFile(argv[1], 0);
  if (argc == 3 && !strcmp(argv[1], "--dump"))
    return ValidatePackFromFile(argv[2], 1);
  if (argc == 3 && !strcmp(argv[1], "--inspect"))
    return ValidatePackFromFile(argv[2], 2);
  if (argc == 3 && !strcmp(argv[1], "--metadata"))
    return ValidatePackFromFile(argv[2], 3);
  if (argc != 1) {
    fprintf(stderr, "usage: %s [--dump|--inspect|--metadata] [pack.ini]\n", argv[0]);
    return EXIT_FAILURE;
  }
  TestMemberPathResolution();
  TestMetadataFastPath();
  TestFullLoad();
  TestRejectedInputs();
  TestTransactionalReload();
  TestSemanticContracts();
  TestPresentationContracts();
  TestAliasChainsAndLookup();
  if (failures) {
    fprintf(stderr, "%d language-pack test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("language-pack runtime tests passed");
  return EXIT_SUCCESS;
}
