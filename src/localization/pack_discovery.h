#ifndef AR_LANGUAGE_PACK_DISCOVERY_H
#define AR_LANGUAGE_PACK_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include "localization/language_pack.h"

/* Host-owned directory enumeration supplies manifest candidates. The core
 * catalog reads only metadata, never all scripts/fonts during menu discovery. */
typedef struct ArLanguagePackCatalogEntry {
  ArLanguagePackMetadata metadata;
  char manifest[1024];
} ArLanguagePackCatalogEntry;

/* The supported number of enabled packs. The builder publishes the same value
 * so it can refuse an enable that the game could not offer. */
enum {
  kArLanguagePackCatalogMaximum = 128,
  kArLanguagePackCatalogVisitLimit = 1024,
};
typedef struct ArLanguagePackCatalog {
  /* Ordered by package ID, so which packs a full catalog keeps does not depend
   * on the order the filesystem happened to enumerate them in. */
  ArLanguagePackCatalogEntry entries[kArLanguagePackCatalogMaximum];
  size_t count;
  /* Valid packs dropped because the catalog was already full, and whether the
   * scan stopped before reading the whole directory. Either means the player
   * is looking at a subset and should be told so. */
  size_t dropped;
  bool visit_limit_reached;
  /* Ambiguous portable IDs never select an arbitrary first copy. */
  char conflicts[kArLanguagePackCatalogVisitLimit][kArLanguagePackageIdCapacity];
  size_t conflict_count;
} ArLanguagePackCatalog;

bool ArLanguagePackCatalog_Add(ArLanguagePackCatalog *catalog,
                               const ArLanguagePackIo *io,
                               const char *manifest, const char *directory_id,
                               ArLanguagePackError *error);
bool ArLanguagePackCatalog_ReadArchiveIndex(ArLanguagePackCatalog *catalog,
    const ArLanguagePackIo *io, const char *root, const char *data, size_t size,
    ArLanguagePackError *error);
/* Desktop adapter: scan one explicit installation root once at startup. */
bool ArLanguagePackCatalog_ScanDesktop(ArLanguagePackCatalog *catalog,
                                       const char *root);
#endif
