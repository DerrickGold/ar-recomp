#include "atomic_replace.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <stdio.h>
#endif

bool AtomicReplaceFile(const char *temporary, const char *path) {
#ifdef _WIN32
  /* MOVEFILE_REPLACE_EXISTING is what makes this work at all on Windows, where
   * rename() refuses an existing destination. WRITE_THROUGH additionally flushes
   * the directory entry, so a power cut cannot lose the rename while keeping the
   * (already flushed) contents -- the same pairing save_system.c documents at
   * length for battery saves. */
  return MoveFileExA(temporary, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(temporary, path) == 0;
#endif
}
