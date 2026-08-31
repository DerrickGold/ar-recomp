#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_audio.h"
#include "snesrecomp/game/runtime.h"

static int s_failures;
static SDL_AtomicInt s_render_calls;
static SDL_AtomicInt s_lock_probe_requested;
static SDL_AtomicInt s_lock_probe_entered;
static SDL_AtomicInt s_lock_probe_hold;
static SDL_AtomicInt s_lock_probe_acquired;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failures++; \
  } \
} while (0)

/* host_audio.c's runtime collaborators. The test needs only a producing audio
 * callback; zero PCM is sufficient to prove whether SDL is advancing it. */
bool RtlApuProfileIsEnabled(void) { return false; }
void RtlApuProfileRecordHostWait(uint64_t wait_ns, bool lock_wait) {
  (void)wait_ns;
  (void)lock_wait;
}
void RtlSetAudioOutputRate(int rate) { (void)rate; }
void RtlRenderAudio(int16 *buffer, int frames, int channels) {
  memset(buffer, 0, (size_t)frames * (size_t)channels * sizeof(*buffer));
  SDL_AddAtomicInt(&s_render_calls, 1);
  if (SDL_GetAtomicInt(&s_lock_probe_requested) != 0) {
    /* Match the real renderer's bounded ownership: it releases its own lock
     * before host gain/mute processing and SDL submission. */
    RtlApuLock();
    RtlApuUnlock();
    SDL_SetAtomicInt(&s_lock_probe_entered, 1);
    while (SDL_GetAtomicInt(&s_lock_probe_hold) != 0) SDL_Delay(1);
    SDL_SetAtomicInt(&s_lock_probe_requested, 0);
  }
}

static int SDLCALL AcquireApuLock(void *unused) {
  (void)unused;
  RtlApuLock();
  SDL_SetAtomicInt(&s_lock_probe_acquired, 1);
  RtlApuUnlock();
  return 0;
}

static bool WaitForAtomicNonzero(SDL_AtomicInt *value, uint32_t timeout_ms) {
  const uint64_t deadline = SDL_GetTicks() + timeout_ms;
  while (SDL_GetTicks() < deadline) {
    if (SDL_GetAtomicInt(value) != 0) return true;
    SDL_Delay(1);
  }
  return SDL_GetAtomicInt(value) != 0;
}

static bool WaitForRenderAfter(int baseline, uint32_t timeout_ms) {
  const uint64_t deadline = SDL_GetTicks() + timeout_ms;
  while (SDL_GetTicks() < deadline) {
    if (SDL_GetAtomicInt(&s_render_calls) > baseline) return true;
    SDL_Delay(5);
  }
  return SDL_GetAtomicInt(&s_render_calls) > baseline;
}

static void CheckRenderCallsStayStopped(void) {
  /* Let an already-running callback finish before sampling the stable count. */
  SDL_Delay(20);
  const int paused_calls = SDL_GetAtomicInt(&s_render_calls);
  SDL_Delay(80);
  CHECK(SDL_GetAtomicInt(&s_render_calls) == paused_calls);
}

int main(void) {
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_AUDIO));
  CHECK(HostAudio_Init(44100, 256, 100, true));

  CHECK(WaitForRenderAfter(0, 1000));

  /* The callback must not retain an outer APU lock after RtlRenderAudio has
   * released its bounded region. Otherwise the game thread is excluded during
   * host-only gain/mute work and SDL stream submission. */
  SDL_SetAtomicInt(&s_lock_probe_hold, 1);
  SDL_SetAtomicInt(&s_lock_probe_requested, 1);
  CHECK(WaitForAtomicNonzero(&s_lock_probe_entered, 1000));
  SDL_Thread *lock_thread =
      SDL_CreateThread(AcquireApuLock, "audio-lock-probe", NULL);
  CHECK(lock_thread != NULL);
  if (lock_thread != NULL) {
    CHECK(WaitForAtomicNonzero(&s_lock_probe_acquired, 1000));
    SDL_SetAtomicInt(&s_lock_probe_hold, 0);
    SDL_WaitThread(lock_thread, NULL);
  } else {
    SDL_SetAtomicInt(&s_lock_probe_hold, 0);
  }

  HostAudio_SetHostPaused(true);
  CheckRenderCallsStayStopped();

  int before_resume = SDL_GetAtomicInt(&s_render_calls);
  HostAudio_SetHostPaused(false);
  CHECK(WaitForRenderAfter(before_resume, 1000));

  /* Changing the output mute while a host pause is active must not bypass the
   * transport pause. */
  HostAudio_SetHostPaused(true);
  HostAudio_SetEnabled(false);
  HostAudio_SetEnabled(true);
  CheckRenderCallsStayStopped();

  before_resume = SDL_GetAtomicInt(&s_render_calls);
  HostAudio_SetHostPaused(false);
  CHECK(WaitForRenderAfter(before_resume, 1000));

  /* Audio-off is a final-output mute, not a transport pause. Clearing the host
   * pause must resume silent rendering so authentic and replacement cursors
   * continue to advance together. */
  HostAudio_SetHostPaused(true);
  HostAudio_SetEnabled(false);
  HostAudio_SetHostPaused(false);
  before_resume = SDL_GetAtomicInt(&s_render_calls);
  CHECK(WaitForRenderAfter(before_resume, 1000));

  /* Toggling the mute in either direction while unpaused never interrupts the
   * producer callback. */
  before_resume = SDL_GetAtomicInt(&s_render_calls);
  HostAudio_SetEnabled(true);
  CHECK(WaitForRenderAfter(before_resume, 1000));
  before_resume = SDL_GetAtomicInt(&s_render_calls);
  HostAudio_SetEnabled(false);
  CHECK(WaitForRenderAfter(before_resume, 1000));
  HostAudio_SetEnabled(true);

  HostAudio_Shutdown();

  /* Booting with output disabled must still open the producer stream. This is
   * the path that prevents a muted session from advancing only the authentic
   * CPU-side APU while leaving host-streamed music behind. */
  before_resume = SDL_GetAtomicInt(&s_render_calls);
  CHECK(HostAudio_Init(44100, 256, 100, false));
  CHECK(WaitForRenderAfter(before_resume, 1000));
  HostAudio_Shutdown();

  SDL_Quit();
  if (s_failures) {
    fprintf(stderr, "host_audio_pause_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("host_audio_pause_test: all tests passed");
  return 0;
}
