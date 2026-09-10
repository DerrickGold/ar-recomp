#include "localization/pack_discovery.h"

#include <stdio.h>
#include <string.h>

static int ComparePortableId(const char *a, const char *b) {
  for (;;) {
    unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
    if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
    if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
    if (left != right) return left < right ? -1 : 1;
    if (!left) return 0;
  }
}

static bool SamePortableId(const char *a, const char *b) {
  return ComparePortableId(a, b) == 0;
}

bool ArLanguagePackCatalog_Add(ArLanguagePackCatalog *catalog,
                               const ArLanguagePackIo *io,
                               const char *manifest, const char *directory_id,
                               ArLanguagePackError *error) {
  if (!catalog || !manifest || !directory_id ||
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
  /* Insert in ID order and, when full, keep the lexicographically smallest
   * IDs. The player then sees the same subset every launch instead of one
   * that changes with directory enumeration order. */
  size_t position = catalog->count;
  while (position > 0 &&
         ComparePortableId(catalog->entries[position - 1].metadata.package_id,
                           metadata.package_id) > 0)
    --position;
  if (catalog->count >= kArLanguagePackCatalogMaximum) {
    catalog->dropped++;
    if (position >= kArLanguagePackCatalogMaximum) {
      if (error)
        snprintf(error->message, sizeof(error->message),
                 "only %d enabled packages are supported",
                 kArLanguagePackCatalogMaximum);
      return false;
    }
    catalog->count = kArLanguagePackCatalogMaximum - 1u;
  }
  memmove(catalog->entries + position + 1, catalog->entries + position,
          (catalog->count - position) * sizeof(*catalog->entries));
  catalog->count++;
  ArLanguagePackCatalogEntry *entry = &catalog->entries[position];
  entry->metadata = metadata;
  snprintf(entry->manifest, sizeof(entry->manifest), "%s", manifest);
  return true;
}
