#include "localization/pack_discovery.h"
#include <stdio.h>
#include <stdlib.h>

/* Process/relocation integration probe; output contains identities, not prose. */
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  ArLanguagePackCatalog *catalog = calloc(1, sizeof(*catalog));
  if (!catalog) return 2;
  if (!ArLanguagePackCatalog_ScanDesktop(catalog, argv[1])) { free(catalog); return 1; }
  for (size_t i = 0; i < catalog->count; ++i)
    printf("%s\t%s\n", catalog->entries[i].metadata.package_id, catalog->entries[i].manifest);
  free(catalog);
  return 0;
}
