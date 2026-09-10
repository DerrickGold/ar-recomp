#include "localization/language_pack.h"
#include "localization/language_contract.h"

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

  ExpectFailure(kManifest, (const uint8_t *)kScript, sizeof(kScript) - 1,
                false, "Fallback.ttf");

  char unsafe_manifest[sizeof(kManifest) + 32];
  strcpy(unsafe_manifest, kManifest);
  char *source = strstr(unsafe_manifest, "text/sky.artext");
  CHECK(source != NULL);
  memcpy(source, "../x/sky.artext", strlen("../x/sky.artext"));
  ExpectFailure(unsafe_manifest, (const uint8_t *)kScript,
                sizeof(kScript) - 1, true, "portable and relative");
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
  CHECK(ArLanguageContract_RouteCount() == 531);
  CHECK(strcmp(ArLanguageContract_RouteId(0), "action.hud.act_1") == 0);
  CHECK(ArLanguageContract_RouteId(531) == NULL);
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

static int ValidatePackFromFile(const char *manifest_path) {
  ArLanguagePackIo io;
  ArLanguagePackFileIo_Init(&io);
  ArLanguagePackError error;
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
  printf("validated %u messages (%u aliases; %u required)\n",
         stats.validated_messages, stats.aliases, stats.required_messages);
  ArLanguagePack_Destroy(&pack);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc == 2)
    return ValidatePackFromFile(argv[1]);
  if (argc != 1) {
    fprintf(stderr, "usage: %s [pack.ini]\n", argv[0]);
    return EXIT_FAILURE;
  }
  TestMetadataFastPath();
  TestFullLoad();
  TestRejectedInputs();
  TestTransactionalReload();
  TestSemanticContracts();
  if (failures) {
    fprintf(stderr, "%d language-pack test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("language-pack runtime tests passed");
  return EXIT_SUCCESS;
}
