#include "localization/pack_discovery.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ScanContext {
  ArLanguagePackCatalog *catalog;
  ArLanguagePackIo io;
  size_t visited;
} ScanContext;

static bool ArchiveName(const char *name) {
  size_t n = strlen(name);
  return name[0] != '.' && n > 7 && !SDL_strcasecmp(name + n - 7, ".arlang");
}

static SDL_EnumerationResult SDLCALL FindArchive(void *user, const char *directory,
                                                const char *name) {
  (void)directory;
  if (!ArchiveName(name)) return SDL_ENUM_CONTINUE;
  *(bool *)user = true;
  return SDL_ENUM_SUCCESS;
}

static bool PrepareArchives(ArLanguagePackCatalog *catalog,
                            const ArLanguagePackIo *io, const char *root) {
  /* Only fixed locations beside the game executable are executable candidates.
   * Neither PATH nor a pack/cache member can choose what program we launch.
   * A bundle retains utils/tools/actraiser-builder after build-only cleanup. */
#ifdef _WIN32
  const char *leaves[] = {
      "utils/tools/actraiser-builder.exe", "actraiser-builder.exe",
      "tools/actraiser-builder.exe"};
#else
  const char *leaves[] = {"utils/tools/actraiser-builder",
                          "actraiser-builder", "tools/actraiser-builder",
#ifdef __APPLE__
                          /* SDL_GetBasePath returns Contents/Resources in an
                           * app, while executable helpers belong in MacOS. */
                          "../MacOS/actraiser-builder",
#endif
  };
#endif
  char helper[1024] = {0};
  const char *base = SDL_GetBasePath();
  if (!base) return false;
  for (size_t i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i) {
    int n = snprintf(helper, sizeof(helper), "%s%s", base, leaves[i]);
    SDL_PathInfo info;
    if (n > 0 && (size_t)n < sizeof(helper) && SDL_GetPathInfo(helper, &info) &&
        info.type == SDL_PATHTYPE_FILE)
      break;
    helper[0] = 0;
  }
  if (!helper[0]) {
    fprintf(stderr, "[localization] .arlang files need the installed ActRaiser "
                    "Builder helper; restore utils/tools/actraiser-builder or "
                    "use unpacked packs\n");
    return false;
  }
  const char *args[] = {helper, "language", "prepare", "--packs-root", root, NULL};
  SDL_Process *process = SDL_CreateProcess(args, false);
  if (!process) {
    fprintf(stderr, "[localization] could not start archive helper: %s\n", SDL_GetError());
    return false;
  }
  Uint64 start = SDL_GetTicks();
  int status = -1;
  bool finished = false;
  while (!(finished = SDL_WaitProcess(process, false, &status)) &&
         SDL_GetTicks() - start < 30000)
    SDL_Delay(10);
  if (!finished) {
    if (SDL_KillProcess(process, true)) SDL_WaitProcess(process, true, &status);
    fprintf(stderr, "[localization] archive preparation timed out\n");
  }
  SDL_DestroyProcess(process);
  if (!finished || status != 0) {
    fprintf(stderr, "[localization] archive preparation failed; "
                    "no stale cached catalog will be used\n");
    return false;
  }
  char path[1024];
  int n = snprintf(path, sizeof(path), "%s/.arlang-cache/catalog-v1.tsv", root);
  if (n < 0 || (size_t)n >= sizeof(path)) return false;
  ArLanguagePackBlob blob = {0};
  ArLanguagePackError error = {{0}};
  if (!io->read_file(io->context, path, 256 * 1024, &blob, error.message,
                     sizeof(error.message))) {
    fprintf(stderr, "[localization] cannot read prepared archive catalog: %s\n", error.message);
    return false;
  }
  bool ok = ArLanguagePackCatalog_ReadArchiveIndex(catalog, io, root,
      (const char *)blob.data, blob.size, &error);
  io->release_file(io->context, &blob);
  if (!ok) fprintf(stderr, "[localization] %s\n", error.message);
  return ok;
}

static SDL_EnumerationResult SDLCALL VisitPack(void *user, const char *directory,
                                              const char *name) {
  ScanContext *scan = user;
  if (++scan->visited > kArLanguagePackCatalogVisitLimit) {
    scan->catalog->visit_limit_reached = true;
    return SDL_ENUM_SUCCESS;
  }
  if (ArchiveName(name)) return SDL_ENUM_CONTINUE;
  if (!name[0] || name[0] == '.' || strlen(name) >= kArLanguagePackageIdCapacity ||
      strpbrk(name, "/\\:")) return SDL_ENUM_CONTINUE;
  char path[1024];
  int length = snprintf(path, sizeof(path), "%s%s/pack.ini", directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) return SDL_ENUM_CONTINUE;
  SDL_PathInfo info;
  if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE; /* Disabled/uninstalled. */
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
  catalog->conflict_count = 0;
  SDL_PathInfo info;
  if (!SDL_GetPathInfo(root, &info)) return true; /* No installed packs yet. */
  if (info.type != SDL_PATHTYPE_DIRECTORY) return false;
  ScanContext scan = {.catalog = catalog};
  ArLanguagePackFileIo_Init(&scan.io);
  bool archives = false;
  if (!SDL_EnumerateDirectory(root, FindArchive, &archives)) return false;
  if (archives) (void)PrepareArchives(catalog, &scan.io, root);
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
