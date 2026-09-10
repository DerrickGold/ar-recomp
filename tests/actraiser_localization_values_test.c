#include "actraiser/actraiser_localization_values.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

typedef struct TestVfs {
  const uint8_t *manifest;
  size_t manifest_bytes;
  const uint8_t *script;
  size_t script_bytes;
} TestVfs;

static int failures;
static uint8_t wram[kActRaiserWramSize];

#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++failures;                                                            \
  }                                                                        \
} while (0)

static bool ReadFile(void *context, const char *path, size_t maximum_bytes,
                     ArLanguagePackBlob *blob, char *error,
                     size_t error_capacity) {
  TestVfs *vfs = (TestVfs *)context;
  const uint8_t *data = NULL;
  size_t bytes = 0;
  if (!strcmp(path, "pack/pack.ini")) {
    data = vfs->manifest;
    bytes = vfs->manifest_bytes;
  } else if (!strcmp(path, "pack/text/source.artext")) {
    data = vfs->script;
    bytes = vfs->script_bytes;
  }
  if (!data || bytes > maximum_bytes) {
    snprintf(error, error_capacity, "fixture path unavailable");
    return false;
  }
  *blob = (ArLanguagePackBlob){
      .struct_size = sizeof(*blob), .data = data, .size = bytes,
  };
  return true;
}

static void ReleaseFile(void *context, ArLanguagePackBlob *blob) {
  (void)context;
  memset(blob, 0, sizeof(*blob));
}

static void Write16(size_t address, uint16_t value) {
  wram[address] = (uint8_t)value;
  wram[address + 1u] = (uint8_t)(value >> 8);
}

static bool Resolve(ActRaiserLocalizationValues *values, const char *name,
                    ArLanguagePlaceholderKind kind,
                    ArDialogueValue *value) {
  char error[256];
  const bool resolved = ActRaiserLocalizationValues_Resolve(
      values, name, kind, value, error, sizeof(error));
  if (!resolved) fprintf(stderr, "resolve %s: %s\n", name, error);
  return resolved;
}

int main(void) {
  static const char manifest[] =
      "[pack]\n"
      "format = actraiser-language-pack\n"
      "version = 1\n"
      "id = fixture.values\n"
      "locale = en-US\n"
      "name = Values Fixture\n"
      "autonym = Values Fixture\n"
      "author = Test\n"
      "license = MIT\n"
      "direction = ltr\n"
      "target = us-runtime\n"
      "source_profile = us\n"
      "fallback = native-us\n"
      "coverage = partial\n"
      "[fonts]\n"
      "primary = builtin:actraiser-sans\n"
      "[scripts]\n"
      "source = text/source.artext\n";
  static const char script[] =
      ":: city.fillmore.name\n"
      "Fíllmore\n"
      "@end\n"
      ":: enemy.name.slot_02\n"
      "Démon\n"
      "@end\n";
  TestVfs vfs = {
      .manifest = (const uint8_t *)manifest,
      .manifest_bytes = sizeof(manifest) - 1u,
      .script = (const uint8_t *)script,
      .script_bytes = sizeof(script) - 1u,
  };
  ArLanguagePackIo io = {
      .struct_size = sizeof(io),
      .abi_version = AR_LANGUAGE_PACK_IO_ABI_VERSION,
      .context = &vfs,
      .read_file = ReadFile,
      .release_file = ReleaseFile,
  };
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError pack_error;
  CHECK(ArLanguagePack_Load(&pack, &io, "pack/pack.ini", &pack_error));

  memset(wram, 0, sizeof(wram));
  wram[kActRaiserWram_CurrentMap] = 1;
  wram[0x0341] = 1;
  wram[0x0002] = 2;
  Write16(0x0006, 17);
  Write16(0x0218, 4321);
  Write16(0x021C, 1234);
  wram[0x0228] = 4;
  Write16(0x022E, 3);
  Write16(0x023A, 2);
  Write16(0x0282, 88);
  Write16(kActRaiserWram_TownLevel, 9);
  Write16(0x0293, 24);
  Write16(kActRaiserWram_PersistentMagicPoints, 5);
  Write16(0x0297, 999);
  wram[0x02AB] = 2;
  Write16(0x02B3, 0x1234);

  ActRaiserLocalizationValues values;
  CHECK(ActRaiserLocalizationValues_Capture(
      &values, wram, sizeof(wram), &pack, NULL, "Maître"));
  ArDialogueValue value;
  Write16(0x0010, 22);
  Write16(0x0012, 38);
  CHECK(Resolve(&values, "sound_music_id", kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 22);
  CHECK(Resolve(&values, "sound_effect_id", kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 38);
  CHECK(Resolve(&values, "master_name",
                kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Maître"));
  CHECK(Resolve(&values, "current_city_name",
                kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Fíllmore"));
  CHECK(Resolve(&values, "enemy_name",
                kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Démon"));
  CHECK(Resolve(&values, "lair_count",
                kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 17);
  CHECK(Resolve(&values, "city_fillmore_population",
                kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 1234);
  CHECK(Resolve(&values, "city_fillmore_growth_state",
                kArLanguagePlaceholder_LocalizedTerm, &value));
  CHECK(!strcmp(value.text, "growth_state.04"));
  CHECK(Resolve(&values, "score_fillmore_act_1",
                kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 12340);
  CHECK(Resolve(&values, "total_score",
                kArLanguagePlaceholder_Number, &value));
  CHECK(value.number == 12340);
  CHECK(Resolve(&values, "icon.status.life",
                kArLanguagePlaceholder_Icon, &value));
  CHECK(!strcmp(value.text, "icon.status.life"));

  const uint64_t revision =
      ActRaiserLocalizationValues_ReportRevision(&values);
  CHECK(revision != 0);
  Write16(0x0218, 4322);
  CHECK(ActRaiserLocalizationValues_ReportRevision(&values) != revision);
  CHECK(!Resolve(&values, "selected_offering_action",
                 kArLanguagePlaceholder_LocalizedText, &value));

  // A partial community pack may translate a dialogue but omit the city/enemy
  // dictionary it references. Missing terms use the native pack, while local
  // overrides win. The fallback is an explicit borrowed input, not a global.
  static const char partial_script[] =
      ":: city.bloodpool.name\nElsewhere\n@end\n"
      ":: enemy.name.slot_02\nExcellent demon\n@end\n";
  vfs.script = (const uint8_t *)partial_script;
  vfs.script_bytes = sizeof(partial_script) - 1;
  ArLanguagePack partial;
  ArLanguagePack_Init(&partial);
  CHECK(ArLanguagePack_Load(&partial, &io, "pack/pack.ini", &pack_error));
  CHECK(ActRaiserLocalizationValues_Capture(
      &values, wram, sizeof(wram), &partial, &pack, "Maître"));
  CHECK(Resolve(&values, "current_city_name", kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Fíllmore"));
  CHECK(Resolve(&values, "town_name", kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Fíllmore"));
  CHECK(Resolve(&values, "enemy_name", kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Excellent demon"));
  uint64_t with_fallback = ActRaiserLocalizationValues_ReportRevision(&values);
  values.fallback_pack = NULL;
  CHECK(ActRaiserLocalizationValues_ReportRevision(&values) != with_fallback);
  CHECK(!Resolve(&values, "current_city_name", kArLanguagePlaceholder_LocalizedText, &value));
  ArLanguagePack_Destroy(&partial);
  static const char empty_script[] = ":: city.fillmore.name\n@empty\n@end\n";
  vfs.script = (const uint8_t *)empty_script;
  vfs.script_bytes = sizeof(empty_script) - 1;
  ArLanguagePack_Init(&partial);
  CHECK(ArLanguagePack_Load(&partial, &io, "pack/pack.ini", &pack_error));
  CHECK(ActRaiserLocalizationValues_Capture(
      &values, wram, sizeof(wram), &partial, &pack, "Maître"));
  CHECK(!Resolve(&values, "current_city_name", kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(Resolve(&values, "enemy_name", kArLanguagePlaceholder_LocalizedText, &value));
  CHECK(!strcmp(value.text, "Démon"));
  values.abi_version = 1;
  CHECK(!Resolve(&values, "enemy_name", kArLanguagePlaceholder_LocalizedText, &value));
  ArLanguagePack_Destroy(&partial);

  ArLanguagePack_Destroy(&pack);
  puts("localization dynamic value checks passed");
  return failures ? 1 : 0;
}
