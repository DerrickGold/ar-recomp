#ifndef USER_DATA_DIR_H
#define USER_DATA_DIR_H

#include <stddef.h>

/* Resolve a portable user-data leaf relative to the process working root.
 * Packaged builds anchor that root beside the executable before opening any
 * files; developer builds retain their launch directory. Keeping this helper
 * central ensures every settings load/save site and both save backends agree.
 * Returns buf. */
char *UserDataFile(char *buf, size_t size, const char *leaf);

#endif /* USER_DATA_DIR_H */
