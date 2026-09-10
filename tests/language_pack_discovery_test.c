#include "localization/pack_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned reads;
static char id_field[128];
static char manifest_buffer[1024];
static const char *manifest =
  "[pack]\nformat = actraiser-language-pack\nversion = 1\n"
  "id = community.excellent\nname = Excellent English\nlocale = en-CA\n"
  "autonym = English\nauthor = Fixture Team\nlicense = CC-BY-4.0\n"
  "direction = auto\ntarget = us-runtime\nsource_profile = us\n"
  "fallback = native-us\ncoverage = partial\n"
  "[fonts]\nprimary = builtin:actraiser-sans\n[scripts]\nsource = text/example.artext\n";
static bool Read(void *context, const char *path, size_t maximum,
                  ArLanguagePackBlob *blob, char *error, size_t capacity) {
  (void)context; (void)error; (void)capacity;
  ++reads;
  if (!strstr(path, "/pack.ini")) return false;
  const char *text = manifest;
  if (id_field[0]) {
    /* Same fixture manifest with the directory's own package ID. */
    const char *cursor = strstr(manifest, "id = community.excellent\n");
    if (!cursor) return false;
    const size_t prefix = (size_t)(cursor - manifest) + 5u;
    snprintf(manifest_buffer, sizeof(manifest_buffer), "%.*s%s%s",
             (int)prefix, manifest, id_field,
             cursor + strlen("id = community.excellent"));
    text = manifest_buffer;
  }
  if (strlen(text) > maximum) return false;
  *blob = (ArLanguagePackBlob){.struct_size=sizeof(*blob), .data=(const uint8_t *)text, .size=strlen(text)};
  return true;
}
static void Release(void *context, ArLanguagePackBlob *blob) { (void)context; (void)blob; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
  ArLanguagePackCatalog *catalog = calloc(1, sizeof(*catalog)); CHECK(catalog);
  ArLanguagePackIo io = {.struct_size=sizeof(io), .abi_version=AR_LANGUAGE_PACK_IO_ABI_VERSION, .read_file=Read, .release_file=Release};
  ArLanguagePackError error = {{0}};
  CHECK(ArLanguagePackCatalog_Add(catalog, &io, "/bundle/utils/game-assets/languages/packs/community.excellent/pack.ini", "community.excellent", &error));
  CHECK(catalog->count == 1 && reads == 1); /* No script/font reads. */
  CHECK(!strcmp(catalog->entries[0].metadata.author, "Fixture Team"));
  CHECK(!strcmp(catalog->entries[0].metadata.locale, "en-CA"));
  CHECK(!ArLanguagePackCatalog_Add(catalog, &io, "/other/pack.ini", "community.excellent", &error));
  CHECK(!ArLanguagePackCatalog_Add(catalog, &io, "/other/pack.ini", "mismatched-directory", &error));
  CHECK(catalog->count == 0 && catalog->conflict_count == 1);
  CHECK(!ArLanguagePackCatalog_Add(catalog, &io, "/third/pack.ini", "community.excellent", &error));

  memset(catalog, 0, sizeof(*catalog));
  const char *token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef/v-123";
  char index[1024];
  snprintf(index, sizeof(index), "actraiser-language-catalog\t1\npack\tcommunity.excellent\t%s\n", token);
  CHECK(ArLanguagePackCatalog_ReadArchiveIndex(catalog, &io, "/packs", index, strlen(index), &error));
  CHECK(catalog->count == 1 && strstr(catalog->entries[0].manifest, token));
  const char *bad_index = "actraiser-language-catalog\t1\npack\tcommunity.excellent\t../escape\n";
  CHECK(!ArLanguagePackCatalog_ReadArchiveIndex(catalog, &io, "/packs", bad_index, strlen(bad_index), &error));
  CHECK(catalog->count == 1);
  const char *conflict_index = "actraiser-language-catalog\t1\nconflict\tCOMMUNITY.EXCELLENT\n";
  CHECK(ArLanguagePackCatalog_ReadArchiveIndex(catalog, &io, "/packs", conflict_index, strlen(conflict_index), &error));
  CHECK(catalog->count == 0 && catalog->conflict_count == 1);

  /* A library larger than the supported capacity keeps a subset that does not
   * depend on the order the filesystem enumerated it, and says how many it
   * could not offer. */
  for (int pass = 0; pass < 2; ++pass) {
    memset(catalog, 0, sizeof(*catalog));
    for (int i = 0; i < 200; ++i) {
      const int index = pass ? 199 - i : i;
      char path[256];
      snprintf(id_field, sizeof(id_field), "community.pack%03d", index);
      snprintf(path, sizeof(path), "/packs/%s/pack.ini", id_field);
      ArLanguagePackCatalog_Add(catalog, &io, path, id_field, &error);
    }
    CHECK(catalog->count == kArLanguagePackCatalogMaximum);
    CHECK(catalog->dropped == 200 - kArLanguagePackCatalogMaximum);
    CHECK(!strcmp(catalog->entries[0].metadata.package_id, "community.pack000"));
    CHECK(!strcmp(catalog->entries[kArLanguagePackCatalogMaximum - 1]
                      .metadata.package_id,
                  "community.pack127"));
    for (size_t i = 1; i < catalog->count; ++i)
      CHECK(strcmp(catalog->entries[i - 1].metadata.package_id,
                   catalog->entries[i].metadata.package_id) < 0);
  }
  id_field[0] = 0;
  free(catalog); return 0;
}
