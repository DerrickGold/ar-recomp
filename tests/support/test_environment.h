#ifndef AR_TEST_ENVIRONMENT_H
#define AR_TEST_ENVIRONMENT_H
/* Process environment controls for native Windows and POSIX test runners. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static inline int Test_SetEnv(const char *name, const char *value, int overwrite) {
#ifdef _WIN32
  if (!name || !name[0] || strchr(name, '=') || !value) {
    errno = EINVAL;
    return -1;
  }
  if (!overwrite && getenv(name)) return 0;
  const int error = _putenv_s(name, value);
  if (error) errno = error;
  return error ? -1 : 0;
#else
  return setenv(name, value, overwrite);
#endif
}

static inline int Test_UnsetEnv(const char *name) {
#ifdef _WIN32
  return Test_SetEnv(name, "", 1);
#else
  return unsetenv(name);
#endif
}
#endif
