#include "host/imported_game_session.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static SDL_SharedObject *s_module;
static const ArImportedGameApi *s_api;
enum Command { kCommand_Step, kCommand_Snapshot };
static struct {
  SDL_Thread *thread;
  SDL_Semaphore *request, *complete;
  SDL_AtomicInt state, cancel;
  const uint8_t *rom;
  size_t rom_size;
  uint8_t stage;
  int extra;
  bool diorama, result;
  uint32_t input;
  unsigned startup_ticks;
  enum Command command;
  const char *snapshot_prefix;
  ArImportedGameFrame frame;
  char error[512], module_path[4096];
} s_session;

static bool OpenModule(void) {
  if (s_module) return true;
  SDL_SharedObject *object = SDL_LoadObject(s_session.module_path);
  if (!object) return false;
  ArImportedGameGetApi get =
      (ArImportedGameGetApi)SDL_LoadFunction(object, "ArImportedGame_GetApi");
  const ArImportedGameApi *api = get ? get(AR_IMPORTED_GAME_ABI) : NULL;
  if (!api || api->size != sizeof(*api) || api->version != AR_IMPORTED_GAME_ABI ||
      !api->open || !api->step || !api->close || !api->error || !api->snapshot) {
    SDL_UnloadObject(object);
    SDL_SetError("Incompatible imported-game module; rebuild the game");
    return false;
  }
  s_module = object;
  s_api = api;
  return true;
}

static void Failure(const char *message) {
  SDL_strlcpy(s_session.error, message, sizeof(s_session.error));
  SDL_SetAtomicInt(&s_session.state, kImportedGame_Failed);
}

static int Run(void *unused) {
  (void)unused;
  const uint64_t begin = SDL_GetTicksNS();
  bool ready = false;
  if (!OpenModule()) Failure(SDL_GetError());
  else if (!s_api->open(s_session.rom, s_session.rom_size, s_session.stage))
    Failure(s_api->error());
  else {
    while (!SDL_GetAtomicInt(&s_session.cancel) && s_session.startup_ticks < 900) {
      ++s_session.startup_ticks;
      if (!s_api->step(0, s_session.extra, s_session.diorama, &s_session.frame)) {
        Failure(s_api->error());
        break;
      }
      ready = s_session.frame.event == kArImportedGameEvent_None &&
          s_session.frame.scene == s_session.stage && s_session.frame.video.level;
      if (ready) break;
    }
    if (ready) {
      fprintf(stderr, "[imported-world] prepared %u startup frames in %.1f ms\n",
          s_session.startup_ticks, (SDL_GetTicksNS() - begin) / 1e6);
      SDL_SetAtomicInt(&s_session.state, kImportedGame_Ready);
    } else if (!SDL_GetAtomicInt(&s_session.cancel) &&
               SDL_GetAtomicInt(&s_session.state) != kImportedGame_Failed) {
      Failure("AR2 level did not become ready");
    }
  }
  /* Stay on this thread after startup: coroutine stacks and Windows fibers
   * cannot be transferred to the presentation thread. Failed execution also
   * stays available for a diagnostic snapshot until the host closes it. */
  while (!SDL_GetAtomicInt(&s_session.cancel)) {
    SDL_WaitSemaphore(s_session.request);
    if (SDL_GetAtomicInt(&s_session.cancel)) break;
    if (s_session.command == kCommand_Snapshot) {
      s_session.result = s_api && s_api->snapshot(s_session.snapshot_prefix);
    } else {
      s_session.result = s_api && s_api->step(s_session.input, s_session.extra,
          s_session.diorama, &s_session.frame);
      if (!s_session.result) Failure(s_api ? s_api->error() : "Imported runtime unavailable");
    }
    SDL_SignalSemaphore(s_session.complete);
  }
  if (s_api) s_api->close();
  return 0;
}

bool ImportedGameSession_Start(const uint8_t *rom, size_t size, uint8_t stage,
                                int extra, bool diorama) {
  if (s_session.thread) return false;
  memset(&s_session, 0, sizeof(s_session));
#ifdef _WIN32
  const char *name = "actraiser2-native.dll";
#elif defined(__APPLE__)
  const char *name = "actraiser2-native.dylib";
#else
  const char *name = "actraiser2-native.so";
#endif
  const char *base = SDL_GetBasePath();
  if (!base || snprintf(s_session.module_path, sizeof(s_session.module_path), "%s%s", base, name)
      >= (int)sizeof(s_session.module_path)) {
    Failure("Cannot locate imported-game module");
    return false;
  }
  s_session.rom = rom;
  s_session.rom_size = size;
  s_session.stage = stage;
  s_session.extra = extra;
  s_session.diorama = diorama;
  s_session.request = SDL_CreateSemaphore(0);
  s_session.complete = SDL_CreateSemaphore(0);
  if (s_session.request && s_session.complete)
    s_session.thread = SDL_CreateThread(Run, "imported-game", NULL);
  if (s_session.thread) return true;
  Failure(SDL_GetError());
  ImportedGameSession_Close();
  return false;
}

ImportedGameState ImportedGameSession_Poll(ArImportedGameFrame *frame, unsigned *startup_ticks) {
  const ImportedGameState state = SDL_GetAtomicInt(&s_session.state);
  if (state == kImportedGame_Ready) {
    *frame = s_session.frame;
    *startup_ticks = s_session.startup_ticks;
  }
  return state;
}

bool ImportedGameSession_Step(uint32_t input, int extra, bool diorama, ArImportedGameFrame *frame) {
  if (!s_session.thread || SDL_GetAtomicInt(&s_session.state) != kImportedGame_Ready) return false;
  s_session.input = input;
  s_session.extra = extra;
  s_session.diorama = diorama;
  s_session.command = kCommand_Step;
  SDL_SignalSemaphore(s_session.request);
  SDL_WaitSemaphore(s_session.complete);
  if (s_session.result) *frame = s_session.frame;
  return s_session.result;
}

bool ImportedGameSession_Snapshot(const char *prefix) {
  if (!prefix || !s_session.thread ||
      SDL_GetAtomicInt(&s_session.state) == kImportedGame_Loading) return false;
  s_session.snapshot_prefix = prefix;
  s_session.command = kCommand_Snapshot;
  SDL_SignalSemaphore(s_session.request);
  SDL_WaitSemaphore(s_session.complete);
  return s_session.result;
}

const char *ImportedGameSession_Error(void) { return s_session.error; }

void ImportedGameSession_Cancel(void) {
  SDL_SetAtomicInt(&s_session.cancel, 1);
  if (s_session.request) SDL_SignalSemaphore(s_session.request);
}

void ImportedGameSession_Close(void) {
  ImportedGameSession_Cancel();
  if (s_session.thread) SDL_WaitThread(s_session.thread, NULL);
  SDL_DestroySemaphore(s_session.request);
  SDL_DestroySemaphore(s_session.complete);
  s_session.thread = NULL;
  s_session.request = s_session.complete = NULL;
}

void ImportedGameSession_EndSession(void) {
  ImportedGameSession_Close();
  if (s_module) SDL_UnloadObject(s_module);
  s_module = NULL;
  s_api = NULL;
}
