#ifndef PORTABLE_PATHS_H
#define PORTABLE_PATHS_H

#include <stdbool.h>

/* Return true when the executable is part of a self-contained distribution.
 * The caller must absolutize relative command-line paths before anchoring the
 * process to the executable directory. */
bool PortablePaths_IsBundle(void);

#endif /* PORTABLE_PATHS_H */
