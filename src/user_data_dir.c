/* Portable user-data paths.
 *
 * Shipped bundles anchor the process working directory beside the executable
 * before any file is opened (main.c:RunningAsBundle). Developer builds keep
 * the caller's working directory. Keeping every settings/save caller behind
 * this helper makes the load and save paths agree without sending portable
 * data to a machine-global preference directory. */
#include "user_data_dir.h"

#include <stdio.h>

char *UserDataFile(char *buf, size_t size, const char *leaf) {
  snprintf(buf, size, "%s", leaf);
  return buf;
}
