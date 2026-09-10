#include "localization/pack_discovery.h"

#include <SDL3/SDL_filesystem.h>
#include <stdio.h>
#include <string.h>

typedef struct ScanContext {
  ArLanguagePackCatalog *catalog;
  ArLanguagePackIo io;
  size_t visited;
} ScanContext;

static SDL_EnumerationResult SDLCALL VisitPack(void *user, const char *directory,
                                              const char *name) {
  ScanContext *scan = user;
  if (++scan->visited > kArLanguagePackCatalogVisitLimit) {
    scan->catalog->visit_limit_reached = true;
    return SDL_ENUM_SUCCESS;
  }
  if (!name[0] || name[0] == '.' || strlen(name) >= kArLanguagePackageIdCapacity ||
      strpbrk(name, "/\\:")) return SDL_ENUM_CONTINUE;
  char path[1024];
  int length = snprintf(path, sizeof(path), "%s%s/pack.ini", directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) return SDL_ENUM_CONTINUE;
  ArLanguagePackError error = {{0}};
  if (!ArLanguagePackCatalog_Add(scan->catalog, &scan->io, path, name, &error))
    fprintf(stderr, "[localization] skipping %s: %s\n", path, error.message);
  return SDL_ENUM_CONTINUE;
}

bool ArLanguagePackCatalog_ScanDesktop(ArLanguagePackCatalog *catalog,
                                       const char *root) {
  if (!catalog || !root || !root[0]) return false;
  catalog->count = 0;
  catalog->dropped = 0;
  catalog->visit_limit_reached = false;
  SDL_PathInfo info;
  if (!SDL_GetPathInfo(root, &info)) return true; /* No installed packs yet. */
  if (info.type != SDL_PATHTYPE_DIRECTORY) return false;
  ScanContext scan = {.catalog = catalog};
  ArLanguagePackFileIo_Init(&scan.io);
  if (!SDL_EnumerateDirectory(root, VisitPack, &scan))
    return false;
  /* Say so rather than quietly presenting a subset as the whole library. */
  if (catalog->dropped)
    fprintf(stderr,
            "[localization] %zu installed package(s) beyond the supported %d "
            "are not selectable; disable some to choose them\n",
            catalog->dropped, kArLanguagePackCatalogMaximum);
  if (catalog->visit_limit_reached)
    fprintf(stderr,
            "[localization] stopped after %d entries in %s; some installed "
            "packages were not examined\n",
            kArLanguagePackCatalogVisitLimit, root);
  return true;
}
