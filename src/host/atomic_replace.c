#include "host/atomic_replace.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "snesrecomp/support/utf8_fs.h"

bool AtomicReplaceFile(const char *temporary, const char *path) {
  return sr_replace_file(temporary, path) != 0;
}

FILE *AtomicReplace_OpenTemporary(const char *path, const char *suffix,
    char **temporary) {
  static atomic_ulong sequence;
  *temporary = NULL;
  const size_t path_length = strlen(path), suffix_length = strlen(suffix);
  if (suffix_length > SIZE_MAX - 48 || path_length > SIZE_MAX - 48 - suffix_length) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  const size_t capacity = path_length + suffix_length + 48;
  char *candidate = malloc(capacity);
  if (!candidate) return NULL;
#ifdef _WIN32
  const unsigned long process = (unsigned long)_getpid();
#else
  const unsigned long process = (unsigned long)getpid();
#endif
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    const unsigned long serial = atomic_fetch_add_explicit(&sequence, 1, memory_order_relaxed);
    snprintf(candidate, capacity, "%s.%lu.%lu.%s", path, process, serial, suffix);
    FILE *file = sr_fopen(candidate, "wbx");
    if (file) {
      *temporary = candidate;
      return file;
    }
    if (errno != EEXIST) break;
  }
  const int error = errno;
  free(candidate);
  errno = error;
  return NULL;
}

bool AtomicReplace_FlushFileData(FILE *file) {
#ifdef _WIN32
  const HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
  return handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle) != 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

void AtomicReplace_SyncContainingDirectory(const char *path) {
#ifndef _WIN32
  const char *slash = strrchr(path, '/');
  char *parent = NULL;
  if (slash && slash != path) {
    parent = strndup(path, (size_t)(slash - path));
    if (!parent) return;
  }
  const int fd = open(parent ? parent : slash ? "/" : ".", O_RDONLY);
  free(parent);
  if (fd < 0) return;
  (void)fsync(fd);
  close(fd);
#else
  (void)path;
#endif
}

bool AtomicReplace_WriteBytes(const char *path, const void *data, size_t size, bool durable) {
  char *temporary;
  FILE *file = AtomicReplace_OpenTemporary(path, "tmp", &temporary);
  if (!file) return false;
  bool ok = fwrite(data, 1, size, file) == size;
  if (fflush(file) != 0 || ferror(file)) ok = false;
  if (ok && durable) ok = AtomicReplace_FlushFileData(file);
  if (fclose(file) != 0) ok = false;
  if (ok) ok = AtomicReplaceFile(temporary, path);
  if (ok && durable) AtomicReplace_SyncContainingDirectory(path);
  if (!ok) sr_remove(temporary);
  free(temporary);
  return ok;
}

#ifdef _WIN32
static bool GetFileIdentity(const char *path, BY_HANDLE_FILE_INFORMATION *info) {
  wchar_t *wide = sr_win_path(path);
  if (!wide) return false;
  const HANDLE file = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(wide);
  if (file == INVALID_HANDLE_VALUE) return false;
  const bool ok = GetFileInformationByHandle(file, info) != 0;
  CloseHandle(file);
  return ok;
}
#endif

bool AtomicReplace_SameFile(const char *a, const char *b) {
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION left, right;
  return GetFileIdentity(a, &left) && GetFileIdentity(b, &right) &&
      left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
      left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
#else
  struct stat left, right;
  return stat(a, &left) == 0 && stat(b, &right) == 0 &&
      left.st_dev == right.st_dev && left.st_ino == right.st_ino;
#endif
}
