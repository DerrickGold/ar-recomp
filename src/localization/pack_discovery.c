#include "localization/pack_discovery.h"

#include <stdio.h>
#include <string.h>

static bool SamePortableId(const char *a, const char *b) {
  for (;;) {
    unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
    if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
    if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
    if (left != right) return false;
    if (!left) return true;
  }
}

bool ArLanguagePackCatalog_Add(ArLanguagePackCatalog *catalog,
                               const ArLanguagePackIo *io,
                               const char *manifest, const char *directory_id,
                               ArLanguagePackError *error) {
  if (!catalog || !manifest || !directory_id ||
      catalog->count >= kArLanguagePackCatalogMaximum ||
      strlen(manifest) >= sizeof(catalog->entries[0].manifest)) return false;
  ArLanguagePackMetadata metadata;
  if (!ArLanguagePack_ReadMetadata(io, manifest, &metadata, NULL, error)) return false;
  if (metadata.target != kArLanguagePackTarget_UsRuntime ||
      metadata.source_profile != kArLanguageSourceProfile_Us ||
      strcmp(metadata.package_id, directory_id) ||
      SamePortableId(metadata.package_id, "native-us")) {
    if (error) snprintf(error->message, sizeof(error->message), "not a matching US runtime package");
    return false;
  }
  for (size_t i = 0; i < catalog->count; ++i)
    if (SamePortableId(catalog->entries[i].metadata.package_id, metadata.package_id)) {
      if (error) snprintf(error->message, sizeof(error->message), "duplicate portable package ID");
      return false;
    }
  ArLanguagePackCatalogEntry *entry = &catalog->entries[catalog->count++];
  entry->metadata = metadata;
  snprintf(entry->manifest, sizeof(entry->manifest), "%s", manifest);
  return true;
}
