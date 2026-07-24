/* user_data_dir.c — P9: root user-writable files (settings.ini, saves/) at the
 * platform pref path instead of the bundle's cwd (which is $ROOT/utils in the
 * packaged layout and gets clobbered on rebuild). Its own TU so the path-join
 * is unit-testable without the game binary. */
#include "user_data_dir.h"

#include <stdio.h>
#include <SDL3/SDL.h>

/* Resolve the user-writable data dir once. SDL_GetPrefPath creates it if
 * needed and returns an absolute path with a trailing separator. */
const char *UserDataDir(void) {
  static char dir[1024];
  static int resolved;
  if (!resolved) {
    char *p = SDL_GetPrefPath("Quintet-Enix", "ActRaiserRecomp");
    if (p) { snprintf(dir, sizeof dir, "%s", p); SDL_free(p); }
    else   { dir[0] = '\0'; }   /* fall back to cwd-relative */
    resolved = 1;
  }
  return dir;
}

char *UserDataFile(char *buf, size_t size, const char *leaf) {
  const char *dir = UserDataDir();
  if (dir[0])
    snprintf(buf, size, "%s%s", dir, leaf);   /* dir has a trailing separator */
  else
    snprintf(buf, size, "%s", leaf);           /* cwd-relative fallback */
  return buf;
}
