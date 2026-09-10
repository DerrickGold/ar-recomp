#include "localization/pack_discovery.h"

#include <stdio.h>
#include <stdlib.h>
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

static void ExcludeId(ArLanguagePackCatalog *catalog, const char *id) {
  for (size_t i = 0; i < catalog->count;) {
    if (SamePortableId(catalog->entries[i].metadata.package_id, id)) {
      memmove(catalog->entries + i, catalog->entries + i + 1,
              (--catalog->count - i) * sizeof(*catalog->entries));
    } else {
      ++i;
    }
  }
  for (size_t i = 0; i < catalog->conflict_count; ++i)
    if (SamePortableId(catalog->conflicts[i], id)) return;
  if (catalog->conflict_count < kArLanguagePackCatalogVisitLimit)
    snprintf(catalog->conflicts[catalog->conflict_count++],
             kArLanguagePackageIdCapacity, "%s", id);
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
  for (size_t i = 0; i < catalog->conflict_count; ++i)
    if (SamePortableId(catalog->conflicts[i], metadata.package_id)) {
      if (error) snprintf(error->message, sizeof(error->message), "conflicting enabled package ID");
      return false;
    }
  for (size_t i = 0; i < catalog->count; ++i)
    if (SamePortableId(catalog->entries[i].metadata.package_id, metadata.package_id)) {
      ExcludeId(catalog, metadata.package_id);
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

/* The Go preparer returns identifiers and restricted cache tokens, not paths
 * chosen by an archive. This is transport parsing, not another ZIP/pack parser.
 * Cached candidates still pass the ordinary C metadata and activation loaders. */
static bool CacheToken(const char *token) {
  size_t n = strlen(token);
  if (n < 68 || n > 98 || token[64] != '/' || token[65] != 'v' || token[66] != '-')
    return false;
  for (size_t i = 0; i < 64; ++i)
    if (!((token[i] >= '0' && token[i] <= '9') ||
          (token[i] >= 'a' && token[i] <= 'f')))
      return false;
  for (size_t i = 67; i < n; ++i)
    if (token[i] < '0' || token[i] > '9') return false;
  return true;
}

bool ArLanguagePackCatalog_ReadArchiveIndex(ArLanguagePackCatalog *catalog,
    const ArLanguagePackIo *io, const char *root, const char *data, size_t size,
    ArLanguagePackError *error) {
  const char header[] = "actraiser-language-catalog\t1\n";
  if (!catalog || !root || !data || size < sizeof(header) - 1 ||
      size > 256 * 1024 || memcmp(data, header, sizeof(header) - 1) ||
      memchr(data, 0, size))
    goto invalid;
  ArLanguagePackCatalog *candidate = malloc(sizeof(*candidate));
  if (!candidate) goto invalid;
  *candidate = *catalog;
  size_t offset = sizeof(header) - 1, lines = 0;
  while (offset < size) {
    const char *end = memchr(data + offset, '\n', size - offset);
    char line[256];
    if (!end || (size_t)(end - data - offset) >= sizeof(line) ||
        ++lines > kArLanguagePackCatalogVisitLimit)
      goto fail;
    size_t length = (size_t)(end - data - offset);
    memcpy(line, data + offset, length);
    line[length] = 0;
    offset += length + 1;
    char *id = strchr(line, '\t');
    if (!id) goto fail;
    *id++ = 0;
    char *token = strchr(id, '\t');
    if (token) *token++ = 0;
    if (!id[0] || strlen(id) >= kArLanguagePackageIdCapacity ||
        strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != strlen(id))
      goto fail;
    if (!strcmp(line, "conflict") && !token) {
      ExcludeId(candidate, id);
      continue;
    }
    if (strcmp(line, "pack") || !token || !CacheToken(token)) goto fail;
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/.arlang-cache/%s/pack.ini", root, token);
    if (written < 0 || (size_t)written >= sizeof(path)) goto fail;
    ArLanguagePackError detail = {{0}};
    if (!ArLanguagePackCatalog_Add(candidate, io, path, id, &detail))
      fprintf(stderr, "[localization] skipping prepared %s: %s\n", id, detail.message);
  }
  *catalog = *candidate;
  free(candidate);
  return true;
fail:
  free(candidate);
invalid:
  if (error) snprintf(error->message, sizeof(error->message), "invalid archive catalog protocol");
  return false;
}
