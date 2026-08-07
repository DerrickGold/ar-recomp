#ifndef ATOMIC_REPLACE_H
#define ATOMIC_REPLACE_H

#include <stdbool.h>

/* Replace `path` with `temporary` as one operation, on both platforms.
 *
 * WHY THIS EXISTS: plain rename() is not portable for this. POSIX replaces the
 * destination atomically; Windows' rename() FAILS when the destination exists,
 * so a bare rename means every save after the first silently does nothing on a
 * platform this project ships.
 *
 * The obvious workaround -- remove(path) and rename again -- is WRONG, and was
 * shipped here briefly: it destroys the destination before knowing the retry
 * will work, so a failure at that point leaves NO file at all. That is the
 * inverse of what an atomic write is for, and both call sites went on to print
 * "your file is unchanged" over a file they had just deleted.
 *
 * MoveFileExA(MOVEFILE_REPLACE_EXISTING) is the actual primitive: a genuine
 * atomic replace with no window where neither file exists. save_system.c and
 * settings.c each grew their own private copy of this before it was worth
 * sharing; both now call this one, the single implementation.
 *
 * NOT named ReplaceFile: <windows.h> defines that as a UNICODE-selected alias
 * for ReplaceFileW/A, so a bare `ReplaceFile` here would be macro-rewritten and
 * collide with the Win32 prototype -- which is why the callers that used to
 * spell their private copies SaveReplaceFileAtomic / Settings_ReplaceFile
 * avoided the bare name too.
 *
 * Returns false with errno/GetLastError() set by the underlying call. On
 * failure the destination is untouched, so a caller's "original kept" message
 * is true. Callers remove the temp file themselves. */
bool AtomicReplaceFile(const char *temporary, const char *path);

#endif /* ATOMIC_REPLACE_H */
