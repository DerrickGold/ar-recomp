#include "app/run_retention.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "snesrecomp/support/utf8_fs.h"

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static const char kOwner[] = ".ar-temporary-run";
static const char kPreserve[] = ".ar-preserved";
static const char kMagic[] = "ActRaiser temporary diagnostics v1\n";

typedef enum PathKind { kOther, kDirectory, kFile } PathKind;

/* SDL path info follows links. Refuse symlinks/junctions before traversal. */
static PathKind Kind(const char *path) {
#ifdef _WIN32
  const DWORD attrs = sr_path_attributes(path);
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
    return kOther;
  return attrs & FILE_ATTRIBUTE_DIRECTORY ? kDirectory : kFile;
#else
  struct stat st;
  if (lstat(path, &st)) return kOther;
  if (S_ISDIR(st.st_mode)) return kDirectory;
  return S_ISREG(st.st_mode) ? kFile : kOther;
#endif
}

static bool Join(char path[kHostPathCapacity], const char *directory, const char *name) {
  const int length = snprintf(path, kHostPathCapacity, "%s/%s", directory, name);
  return length >= 0 && length < kHostPathCapacity;
}

static bool Lock(FILE *file) {
#ifdef _WIN32
  const HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
  OVERLAPPED overlapped = {0};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                  0, 1, 0, &overlapped)) return false;
  SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
#else
  const int fd = fileno(file);
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) return false;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  return true;
}

static FILE *RotationLock(const char *root) {
  char path[kHostPathCapacity];
  if (Kind(root) != kDirectory || !Join(path, root, ".ar-rotation.lock")) return NULL;
#ifdef _WIN32
  if (Kind(path) != kFile && sr_path_exists(path)) return NULL;
  FILE *file = sr_fopen(path, "a+b");
#else
  const int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
  struct stat info;
  if (fd < 0) return NULL;
  if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
    close(fd);
    return NULL;
  }
  FILE *file = fdopen(fd, "r+b");
  if (!file) close(fd);
#endif
  if (file && Lock(file)) return file;
  if (file) fclose(file);
  return NULL;
}

FILE *RunRetention_Claim(const char *directory) {
  char path[kHostPathCapacity];
  if (!Join(path, directory, kOwner)) return NULL;
  FILE *file = sr_fopen(path, "wbx");
  if (!file) return NULL;
  if (Lock(file) && fwrite(kMagic, 1, sizeof(kMagic), file) == sizeof(kMagic) &&
      fflush(file) == 0) return file;
  fclose(file);
  sr_remove(path);
  return NULL;
}

bool RunRetention_AutomaticPath(const char *root, const char *timestamp, char *path,
                                size_t capacity, FILE **owner) {
  if (Kind(root) != kDirectory) return false;
  for (unsigned i = 0; i < 100; ++i) {
    const int length = i ? snprintf(path, capacity, "%s/%s-%u", root, timestamp, i)
                         : snprintf(path, capacity, "%s/%s", root, timestamp);
    if (length < 0 || (size_t)length >= capacity) break;
    if (sr_mkdir(path) != 0) continue;
    *owner = RunRetention_Claim(path);
    if (*owner) return true;
    SDL_RemovePath(path);
    break;
  }
  return false;
}

static SDL_EnumerationResult SDLCALL CheckEmpty(void *context, const char *directory,
                                                const char *name) {
  (void)directory;
  (void)name;
  *(bool *)context = false;
  return SDL_ENUM_SUCCESS;
}

bool RunRetention_Preserve(const char *directory) {
  /* An empty directory has no automatic ownership marker. Exclusive creation
   * also rejects simultaneous requests for the same preserved destination. */
  char path[kHostPathCapacity];
  bool empty = true;
  FILE *marker = SDL_EnumerateDirectory(directory, CheckEmpty, &empty) && empty &&
      Join(path, directory, kPreserve) ? sr_fopen(path, "wbx") : NULL;
  return marker && fclose(marker) == 0;
}

static FILE *OpenInactiveRun(const char *directory, SDL_Time *modified) {
  char path[kHostPathCapacity], magic[sizeof(kMagic)];
  if (Kind(directory) != kDirectory || !Join(path, directory, kOwner) || Kind(path) != kFile)
    return NULL;
  FILE *file = sr_fopen(path, "r+b");
  if (!file) return NULL;
  SDL_PathInfo info;
  if (Lock(file) && fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
      !memcmp(magic, kMagic, sizeof(magic)) && SDL_GetPathInfo(path, &info)) {
    *modified = info.modify_time;
    return file;
  }
  fclose(file);
  return NULL;
}

typedef struct Tree {
  uint64_t bytes;
  unsigned depth;
  bool ok, remove;
} Tree;

static SDL_EnumerationResult SDLCALL VisitTree(void *context, const char *directory,
                                              const char *name) {
  Tree *tree = context;
  /* Put an explicit capture or a user keep marker anywhere in a temporary
   * bundle to protect that bundle in its entirety. */
  if (!strcmp(name, kPreserve)) goto stop;
  if (!tree->depth && !strcmp(name, kOwner)) return SDL_ENUM_CONTINUE;
  char path[kHostPathCapacity];
  if (!Join(path, directory, name)) goto stop;
  const PathKind kind = Kind(path);
  if (kind == kDirectory) {
    if (tree->depth >= 16) goto stop;
    ++tree->depth;
    const bool enumerated = SDL_EnumerateDirectory(path, VisitTree, tree);
    --tree->depth;
    if (!enumerated || !tree->ok) goto stop;
  } else if (kind == kFile) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || UINT64_MAX - tree->bytes < info.size) goto stop;
    tree->bytes += info.size;
  } else goto stop;
  if (tree->remove && !SDL_RemovePath(path)) goto stop;
  return SDL_ENUM_CONTINUE;
stop:
  tree->ok = false;
  return SDL_ENUM_FAILURE;
}

typedef struct RunEntry {
  char name[40];
  SDL_Time modified;
  uint64_t bytes;
} RunEntry;

typedef struct Runs {
  RunEntry *entries;
  size_t count, capacity;
} Runs;

static bool TimestampName(const char *name) {
  const size_t length = strlen(name);
  if (length < 15 || length > 18 || name[8] != '-') return false;
  for (size_t i = 0; i < 15; ++i)
    if (i != 8 && (name[i] < '0' || name[i] > '9')) return false;
  if (length == 15) return true;
  return length >= 17 && name[15] == '-' && strspn(name + 16, "0123456789") == length - 16;
}

static SDL_EnumerationResult SDLCALL VisitRun(void *context, const char *root,
                                             const char *name) {
  if (!TimestampName(name)) return SDL_ENUM_CONTINUE;
  char path[kHostPathCapacity];
  if (!Join(path, root, name)) return SDL_ENUM_CONTINUE;
  SDL_Time modified;
  FILE *marker = OpenInactiveRun(path, &modified);
  if (!marker) return SDL_ENUM_CONTINUE;
  Tree tree = {.ok = true};
  const bool complete = SDL_EnumerateDirectory(path, VisitTree, &tree) && tree.ok;
  fclose(marker);
  if (!complete) return SDL_ENUM_CONTINUE;

  Runs *runs = context;
  if (runs->count == runs->capacity) {
    const size_t capacity = runs->capacity ? runs->capacity * 2 : 16;
    if (capacity < runs->capacity || capacity > SIZE_MAX / sizeof(RunEntry))
      return SDL_ENUM_FAILURE;
    RunEntry *entries = realloc(runs->entries, capacity * sizeof(*entries));
    if (!entries) return SDL_ENUM_FAILURE;
    runs->entries = entries;
    runs->capacity = capacity;
  }
  RunEntry *entry = &runs->entries[runs->count++];
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  entry->modified = modified;
  entry->bytes = tree.bytes;
  return SDL_ENUM_CONTINUE;
}

static int NewestFirst(const void *a, const void *b) {
  const RunEntry *left = a, *right = b;
  if (left->modified != right->modified) return left->modified > right->modified ? -1 : 1;
  return strcmp(right->name, left->name);
}

static void RemoveRun(const char *root, const RunEntry *entry) {
  char directory[kHostPathCapacity], path[kHostPathCapacity];
  if (!Join(directory, root, entry->name) || !Join(path, directory, kOwner)) return;
  SDL_Time modified;
  FILE *marker = OpenInactiveRun(directory, &modified);
  if (!marker) return;
  Tree tree = {.ok = true};
  bool safe = modified == entry->modified &&
      SDL_EnumerateDirectory(directory, VisitTree, &tree) && tree.ok;
  if (safe) {
    tree = (Tree){.ok = true, .remove = true};
    safe = SDL_EnumerateDirectory(directory, VisitTree, &tree) && tree.ok;
  }
  fclose(marker);
  if (safe && sr_remove(path) == 0) SDL_RemovePath(directory);
}

void RunRetention_Prune(const char *root, size_t keep, uint64_t byte_budget,
                        time_t cutoff) {
  FILE *lock = RotationLock(root);
  if (!lock) return;
  Runs runs = {0};
  if (SDL_EnumerateDirectory(root, VisitRun, &runs) && runs.count) {
    qsort(runs.entries, runs.count, sizeof(*runs.entries), NewestFirst);
    size_t retained = 0;
    uint64_t remaining = byte_budget;
    for (size_t i = 0; i < runs.count; ++i) {
      const RunEntry *entry = &runs.entries[i];
      if (retained < keep && entry->bytes <= remaining &&
          (cutoff <= 0 || SDL_NS_TO_SECONDS(entry->modified) >= cutoff)) {
        ++retained;
        remaining -= entry->bytes;
      } else RemoveRun(root, entry);
    }
  }
  free(runs.entries);
  fclose(lock);
}
