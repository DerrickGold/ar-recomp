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

enum { kArLanguagePackCatalogMaximum = 128 };
typedef struct ArLanguagePackCatalog {
  ArLanguagePackCatalogEntry entries[kArLanguagePackCatalogMaximum];
  size_t count;
} ArLanguagePackCatalog;

bool ArLanguagePackCatalog_Add(ArLanguagePackCatalog *catalog,
                               const ArLanguagePackIo *io,
                               const char *manifest, const char *directory_id,
                               ArLanguagePackError *error);
/* Desktop adapter: scan one explicit installation root once at startup. */
bool ArLanguagePackCatalog_ScanDesktop(ArLanguagePackCatalog *catalog,
                                       const char *root);
#endif
