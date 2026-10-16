#include "platform/android/android_platform.h"

#ifdef __ANDROID__

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL.h>

static const char kLogTag[] = "ActRaiser";
static const char kAssetList[] = "android-assets.txt";
static const char kStampFile[] = ".android-assets-stamp";

/* Player-owned files are seeded once and never overwritten by a newer APK. */
static bool IsPlayerOwned(const char *path) {
  return !strcmp(path, "config.ini") || !strcmp(path, "settings.ini");
}

static bool MakeParents(const char *path) {
  char buffer[1024];
  if (snprintf(buffer, sizeof buffer, "%s", path) >= (int)sizeof buffer) return false;
  for (char *p = buffer + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
    *p = '/';
  }
  return true;
}

static bool CopyAsset(const char *relative, const char *destination) {
  size_t size = 0;
  /* A relative SDL path on Android reads the APK's assets/. */
  void *data = SDL_LoadFile(relative, &size);
  if (!data) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "missing APK asset %s: %s",
                        relative, SDL_GetError());
    return false;
  }
  bool ok = MakeParents(destination);
  char temporary[1100];
  snprintf(temporary, sizeof temporary, "%s.part", destination);
  FILE *file = ok ? fopen(temporary, "wb") : NULL;
  ok = file && fwrite(data, 1, size, file) == size;
  if (file) ok = (fclose(file) == 0) && ok;
  ok = ok && rename(temporary, destination) == 0;
  if (!ok) unlink(temporary);
  SDL_free(data);
  return ok;
}

static char *ReadSmallFile(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  char *text = calloc(1, 256);
  if (text) fread(text, 1, 255, file);
  fclose(file);
  return text;
}

bool AndroidPlatform_PrepareStorage(char *rom_path, size_t rom_path_size) {
  /* Touches drive the on-screen controls; do not also synthesize mouse
   * clicks, which the desktop path uses for camera drags. */
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  /* Until RunDirInit picks a log file, stdout/stderr would go nowhere. */
  AndroidPlatform_TeeConsole(NULL);

  const char *base = SDL_GetAndroidExternalStoragePath();
  if (!base || !*base) base = SDL_GetAndroidInternalStoragePath();
  if (!base || !*base) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "no app storage path: %s",
                        SDL_GetError());
    return false;
  }
  char root[1024];
  snprintf(root, sizeof root, "%s/game", base);
  if (mkdir(root, 0755) != 0 && errno != EEXIST) return false;
  if (chdir(root) != 0) return false;

  size_t list_size = 0;
  char *list = SDL_LoadFile(kAssetList, &list_size);
  if (!list) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "APK has no %s", kAssetList);
    return false;
  }
  /* First line: "stamp <id>". The rest: one asset path per line. */
  char *save = NULL;
  char *line = strtok_r(list, "\r\n", &save);
  const char *stamp = line && !strncmp(line, "stamp ", 6) ? line + 6 : "";
  char *installed = ReadSmallFile(kStampFile);
  const bool refresh = !installed || strcmp(installed, stamp) != 0;
  free(installed);
  int copied = 0, failed = 0;
  if (refresh) {
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
      if (!*line || strstr(line, "..")) continue;
      if (IsPlayerOwned(line) && access(line, F_OK) == 0) continue;
      if (CopyAsset(line, line)) copied++;
      else failed++;
    }
    if (!failed) {
      FILE *file = fopen(kStampFile, "wb");
      if (file) { fputs(stamp, file); fclose(file); }
    }
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag,
                      "data root %s; assets %s (copied %d, failed %d)", root,
                      refresh ? "refreshed" : "current", copied, failed);
  SDL_free(list);
  if (failed) return false;

  if (SDL_setenv_unsafe("AR_USER_DATA_DIR", root, 1) != 0) return false;
  return snprintf(rom_path, rom_path_size, "%s/user-rom.sfc", root) <
         (int)rom_path_size;
}

enum { kDesktopMainStackBytes = 8 * 1024 * 1024 };

typedef struct StackedEntry {
  int (*entry)(int, char **);
  int argc;
  char **argv;
  int result;
} StackedEntry;

static void *RunStackedEntry(void *argument) {
  StackedEntry *call = argument;
  call->result = call->entry(call->argc, call->argv);
  return NULL;
}

int AndroidPlatform_RunWithDesktopStack(int (*entry)(int, char **), int argc,
                                        char **argv) {
  StackedEntry call = {entry, argc, argv, 1};
  pthread_attr_t attributes;
  pthread_t thread;
  if (pthread_attr_init(&attributes) != 0 ||
      pthread_attr_setstacksize(&attributes, kDesktopMainStackBytes) != 0 ||
      pthread_create(&thread, &attributes, RunStackedEntry, &call) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "could not start the game thread; running on SDLThread");
    return entry(argc, argv);
  }
  pthread_attr_destroy(&attributes);
  pthread_join(thread, NULL);
  return call.result;
}

typedef struct ConsoleTee {
  int read_fd;
  FILE *log;
} ConsoleTee;

static void *ConsoleTeeThread(void *argument) {
  ConsoleTee *tee = argument;
  char buffer[2048];
  size_t used = 0;
  for (;;) {
    ssize_t n = read(tee->read_fd, buffer + used, sizeof buffer - 1 - used);
    if (n <= 0) break;
    if (tee->log) { fwrite(buffer + used, 1, (size_t)n, tee->log); fflush(tee->log); }
    used += (size_t)n;
    buffer[used] = 0;
    char *start = buffer;
    char *newline;
    while ((newline = strchr(start, '\n')) != NULL) {
      *newline = 0;
      __android_log_write(ANDROID_LOG_INFO, kLogTag, start);
      start = newline + 1;
    }
    used = strlen(start);
    if (used == sizeof buffer - 1) {   /* overlong line: flush as-is */
      __android_log_write(ANDROID_LOG_INFO, kLogTag, start);
      used = 0;
    } else {
      memmove(buffer, start, used);
    }
  }
  return NULL;
}

void AndroidPlatform_TeeConsole(const char *log_path) {
  static ConsoleTee tee;
  int fds[2];
  if (pipe(fds) != 0) return;
  tee.read_fd = fds[0];
  tee.log = log_path ? fopen(log_path, "w") : NULL;
  fflush(stdout);
  fflush(stderr);
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  pthread_t thread;
  if (pthread_create(&thread, NULL, ConsoleTeeThread, &tee) == 0)
    pthread_detach(thread);
}

#endif /* __ANDROID__ */
