#ifndef USER_DATA_DIR_H
#define USER_DATA_DIR_H

#include <stddef.h>

/* Resolve a leaf beneath the process data root. Native launchers select the
 * portable/custom/per-user root; folder releases anchor beside the executable.
 * Developer builds without an explicit root retain their launch directory.
 * Storage policy belongs to the launcher, not to individual save/settings IO.
 * Returns buf; an undersized buffer is cleared rather than silently truncated. */
char *UserDataFile(char *buf, size_t size, const char *leaf);

#endif /* USER_DATA_DIR_H */
