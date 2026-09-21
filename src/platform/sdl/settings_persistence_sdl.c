#include "settings_persistence_sdl.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct SettingsWrite {
  char *path, *text;
  size_t size;
} SettingsWrite;

struct SettingsPersistence {
  SDL_Mutex *mutex;
  SDL_Condition *changed;
  SDL_Thread *thread;
  char *destination;
  SettingsWrite pending;
  bool active, stop, failed;
  SettingsPersistenceReport report;
};

static void FreeWrite(SettingsWrite *write) {
  free(write->path);
  free(write->text);
  *write = (SettingsWrite){0};
}

static int SDLCALL RunWriter(void *context) {
  SettingsPersistence *writer = context;
  SDL_LockMutex(writer->mutex);
  for (;;) {
    while (!writer->pending.text && !writer->stop)
      SDL_WaitCondition(writer->changed, writer->mutex);
    if (!writer->pending.text && writer->stop) break;
    SettingsWrite write = writer->pending;
    writer->pending = (SettingsWrite){0};
    writer->active = true;
    SDL_UnlockMutex(writer->mutex);
    const uint64_t start = SDL_GetTicksNS();
    const bool ok = Settings_WriteSnapshot(write.path, write.text, write.size);
    const uint64_t elapsed = SDL_GetTicksNS() - start;
    FreeWrite(&write);
    SDL_LockMutex(writer->mutex);
    writer->active = false;
    writer->failed = !ok;
    /* A newer durable success resolves any failure not yet collected by the
     * owner, including a synchronous retry through this same queue. */
    writer->report.failed = !ok;
    ++writer->report.writes;
    writer->report.elapsed_ns += elapsed;
    if (elapsed > writer->report.maximum_ns) writer->report.maximum_ns = elapsed;
    SDL_BroadcastCondition(writer->changed);
  }
  SDL_UnlockMutex(writer->mutex);
  return 0;
}

static bool Submit(void *context, const char *path, const char *text, size_t size) {
  SettingsPersistence *writer = context;
  if (!writer || !path || !path[0] || !text || !size || size == SIZE_MAX)
    return false;
  SettingsWrite write = {.size = size};
  write.path = malloc(strlen(path) + 1);
  write.text = malloc(size + 1);
  if (!write.path || !write.text) { FreeWrite(&write); return false; }
  strcpy(write.path, path);
  memcpy(write.text, text, size);
  write.text[size] = 0;
  SDL_LockMutex(writer->mutex);
  /* One destination per writer lifetime: a success for another file must
   * never mask a failed write to this settings file. */
  if (writer->stop || (writer->destination && strcmp(writer->destination, path))) {
    SDL_UnlockMutex(writer->mutex);
    FreeWrite(&write);
    return false;
  }
  if (!writer->destination) {
    writer->destination = malloc(strlen(path) + 1);
    if (!writer->destination) {
      SDL_UnlockMutex(writer->mutex);
      FreeWrite(&write);
      return false;
    }
    strcpy(writer->destination, path);
  }
  SettingsWrite replaced = writer->pending;
  writer->pending = write;
  SDL_SignalCondition(writer->changed);
  SDL_UnlockMutex(writer->mutex);
  FreeWrite(&replaced);
  return true;
}

static bool Drain(void *context) {
  SettingsPersistence *writer = context;
  if (!writer) return true;
  SDL_LockMutex(writer->mutex);
  while (writer->active || writer->pending.text)
    SDL_WaitCondition(writer->changed, writer->mutex);
  const bool ok = !writer->failed;
  SDL_UnlockMutex(writer->mutex);
  return ok;
}

SettingsPersistence *SettingsPersistence_Create(void) {
  SettingsPersistence *writer = calloc(1, sizeof(*writer));
  if (!writer) return NULL;
  writer->mutex = SDL_CreateMutex();
  writer->changed = SDL_CreateCondition();
  if (writer->mutex && writer->changed)
    writer->thread = SDL_CreateThread(RunWriter, "settings-save", writer);
  if (writer->thread) return writer;
  SDL_DestroyCondition(writer->changed);
  SDL_DestroyMutex(writer->mutex);
  free(writer);
  return NULL;
}

SettingsSaveHost SettingsPersistence_Host(SettingsPersistence *writer) {
  return writer ? (SettingsSaveHost){writer, Submit, Drain} : (SettingsSaveHost){0};
}

SettingsPersistenceReport SettingsPersistence_TakeReport(SettingsPersistence *writer) {
  if (!writer) return (SettingsPersistenceReport){0};
  SDL_LockMutex(writer->mutex);
  const SettingsPersistenceReport report = writer->report;
  writer->report = (SettingsPersistenceReport){0};
  SDL_UnlockMutex(writer->mutex);
  return report;
}

bool SettingsPersistence_Destroy(SettingsPersistence *writer) {
  if (!writer) return true;
  const bool ok = Drain(writer);
  SDL_LockMutex(writer->mutex);
  writer->stop = true;
  SDL_SignalCondition(writer->changed);
  SDL_UnlockMutex(writer->mutex);
  SDL_WaitThread(writer->thread, NULL);
  free(writer->destination);
  SDL_DestroyCondition(writer->changed);
  SDL_DestroyMutex(writer->mutex);
  free(writer);
  return ok;
}
