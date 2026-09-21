#include "platform/sdl/settings_persistence_sdl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static SDL_Semaphore *s_started, *s_release;
static unsigned s_writes;
static char s_last[32];

bool Settings_WriteSnapshot(const char *path, const char *text, size_t size) {
  CHECK(!strcmp(path, "settings-probe.ini"));
  CHECK(size < sizeof(s_last));
  if (!strcmp(text, "first")) {
    SDL_SignalSemaphore(s_started);
    CHECK(SDL_WaitSemaphoreTimeout(s_release, 3000));
  }
  memcpy(s_last, text, size);
  s_last[size] = 0;
  ++s_writes;
  return strcmp(text, "fail") != 0;
}

int main(void) {
  CHECK(SDL_Init(0));
  s_started = SDL_CreateSemaphore(0);
  s_release = SDL_CreateSemaphore(0);
  CHECK(s_started && s_release);
  SettingsPersistence *writer = SettingsPersistence_Create();
  CHECK(writer);
  const SettingsSaveHost host = SettingsPersistence_Host(writer);
  CHECK(host.submit(host.context, "settings-probe.ini", "first", 5));
  CHECK(SDL_WaitSemaphoreTimeout(s_started, 3000));
  /* The first disk operation is deliberately blocked. Submitting newer
   * snapshots must not wait, expose caller memory, or grow an unbounded queue. */
  CHECK(host.submit(host.context, "settings-probe.ini", "superseded", 10));
  char newest[] = "latest";
  CHECK(host.submit(host.context, "settings-probe.ini", newest, 6));
  memset(newest, 'x', 6);
  CHECK(!host.submit(host.context, "another-file.ini", "wrong", 5));
  SDL_SignalSemaphore(s_release);
  CHECK(host.drain(host.context));
  CHECK(s_writes == 2 && !strcmp(s_last, "latest"));
  CHECK(!host.submit(host.context, "another-file.ini", "wrong", 5));
  const SettingsPersistenceReport report = SettingsPersistence_TakeReport(writer);
  CHECK(!report.failed && report.writes == 2 && report.elapsed_ns >= report.maximum_ns);
  CHECK(host.submit(host.context, "settings-probe.ini", "fail", 4));
  CHECK(!host.drain(host.context));
  CHECK(SettingsPersistence_TakeReport(writer).failed);
  CHECK(!SettingsPersistence_TakeReport(writer).failed);
  CHECK(host.submit(host.context, "settings-probe.ini", "recovered", 9));
  CHECK(host.drain(host.context));
  CHECK(host.submit(host.context, "settings-probe.ini", "shutdown", 8));
  CHECK(SettingsPersistence_Destroy(writer));
  CHECK(s_writes == 5 && !strcmp(s_last, "shutdown"));
  CHECK(SettingsPersistence_Destroy(NULL));
  SDL_DestroySemaphore(s_started);
  SDL_DestroySemaphore(s_release);
  SDL_Quit();
  puts("settings persistence: immutable/coalesced/nonblocking, failure and shutdown PASS");
  return 0;
}
