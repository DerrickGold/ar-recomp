#include "localization/pack_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned reads;
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
  if (!strstr(path, "/pack.ini") || strlen(manifest) > maximum) return false;
  *blob = (ArLanguagePackBlob){.struct_size=sizeof(*blob), .data=(const uint8_t *)manifest, .size=strlen(manifest)};
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
  CHECK(catalog->count == 1);
  free(catalog); return 0;
}
