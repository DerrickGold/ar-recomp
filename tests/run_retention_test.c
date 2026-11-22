#include "app/run_retention.h"
#include "support/test_assert.h"

#include <SDL3/SDL.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static FILE *MakeRun(const char *root, const char *name, size_t size, char path[1024]) {
  FILE *lock = NULL;
  assert(RunRetention_AutomaticPath(root, name, path, 1024, &lock));
  char output[1200];
  snprintf(output, sizeof(output), "%s/console.log", path);
  FILE *file = fopen(output, "wb");
  assert(file);
  for (size_t i = 0; i < size; ++i) assert(fputc('x', file) != EOF);
  assert(fclose(file) == 0);
  return lock;
}

static bool Exists(const char *path) {
  SDL_PathInfo info;
  return SDL_GetPathInfo(path, &info);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const char *root = argv[1];
  assert(SDL_CreateDirectory(root));
  char old[1024], large[1024], latest[1024], active[1024], preserved[1024];
  fclose(MakeRun(root, "20261121-120000", 4, old));
  fclose(MakeRun(root, "20261121-120001", 128, large));
  FILE *running = MakeRun(root, "20261121-120002", 128, active);
  fclose(MakeRun(root, "20261121-120003", 128, preserved));
  char explicit_dir[1200];
  snprintf(explicit_dir, sizeof(explicit_dir), "%s/evidence", preserved);
  assert(SDL_CreateDirectory(explicit_dir));
  assert(RunRetention_Preserve(explicit_dir));
  assert(!RunRetention_Preserve(explicit_dir));
  fclose(MakeRun(root, "20261121-120004", 4, latest));
  char legacy[1200];
  snprintf(legacy, sizeof(legacy), "%s/20261121-110000", root);
  assert(SDL_CreateDirectory(legacy));
#ifndef _WIN32
  char linked[1024];
  fclose(MakeRun(root, "20261121-120005", 0, linked));
  char link[1200];
  snprintf(link, sizeof(link), "%s/external", linked);
  assert(symlink(legacy, link) == 0);
#endif
  RunRetention_Prune(root, 1, 16, 0);
  assert(!Exists(old) && !Exists(large) && Exists(latest));
  assert(Exists(active) && Exists(preserved) && Exists(legacy));
#ifndef _WIN32
  assert(Exists(linked) && Exists(link));
#endif
  fclose(running);
  RunRetention_Prune(root, 10, 1024, time(NULL) + 60);
  assert(!Exists(latest) && !Exists(active));
  assert(Exists(preserved) && Exists(legacy));
  return 0;
}
