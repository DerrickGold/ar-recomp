#ifndef USER_DATA_DIR_H
#define USER_DATA_DIR_H

#include <stddef.h>

/* P9: user-writable data directory (SDL_GetPrefPath). Kept in its own TU so it
 * is unit-testable without linking the game binary. */

/* The pref dir (SDL_GetPrefPath("Quintet-Enix","ActRaiserRecomp")), resolved
 * once and cached. Absolute with a trailing separator, or "" if unavailable
 * (caller then falls back to cwd-relative — in-tree dev runs unchanged). */
const char *UserDataDir(void);

/* Join `leaf` under UserDataDir() into `buf`. If the pref dir is unavailable,
 * writes `leaf` verbatim (cwd-relative fallback). Returns buf. */
char *UserDataFile(char *buf, size_t size, const char *leaf);

#endif /* USER_DATA_DIR_H */
