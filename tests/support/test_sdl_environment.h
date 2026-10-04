#ifndef AR_TEST_SDL_ENVIRONMENT_H
#define AR_TEST_SDL_ENVIRONMENT_H
/* SDL's DLL and the harness may use separate C runtime environment copies on
 * Windows. Keep both copies in sync when testing runtime environment changes. */
#include <SDL3/SDL_stdinc.h>
#include "test_environment.h"

static inline int Test_SDLSetEnv(const char *name, const char *value, int overwrite) {
  int result = SDL_setenv_unsafe(name, value, overwrite);
#ifdef _WIN32
  if (!result) result = Test_SetEnv(name, value, overwrite);
#endif
  return result;
}

static inline int Test_SDLUnsetEnv(const char *name) {
  int result = SDL_unsetenv_unsafe(name);
#ifdef _WIN32
  if (!result) result = Test_UnsetEnv(name);
#endif
  return result;
}
#endif
