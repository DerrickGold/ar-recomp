#include "host/gamepad_poll.h"

#include <SDL3/SDL.h>
#include <stdatomic.h>

static struct {
  SDL_Thread *thread;
  SDL_Semaphore *stop;
  char *previous_hint;
  bool owns_hint;
  atomic_uint_fast64_t selection, activity;
} s_poll;

static int SDLCALL PollGamepads(void *unused) {
  (void)unused;
  do {
    /* SDL documents joystick updates as safe on any thread. Some HID drivers
     * perform blocking feature reports here (the Deck watchdog takes ~8 ms).
     * Window/keyboard pumping and application event handlers stay on main. */
    SDL_UpdateJoysticks();
    const uint64_t selection = atomic_load_explicit(&s_poll.selection, memory_order_acquire);
    const SDL_JoystickID id = (uint32_t)selection;
    const int deadzone = (selection >> 32) & 0xffff;
    const int trigger_threshold = selection >> 48;
    bool active = false;
    if (id) {
      /* Keep the handle valid across a concurrent hot-unplug/close. Main must
       * not call device getters every frame: their joystick lock would move
       * the same HID wait straight back onto the presentation thread. */
      SDL_LockJoysticks();
      SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
      if (pad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !active; ++b)
          active = SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !active; ++a) {
          const int value = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
          const bool trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                               a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
          active = trigger ? value > trigger_threshold
                           : value < -deadzone || value > deadzone;
        }
      }
      SDL_UnlockJoysticks();
    }
    atomic_store_explicit(&s_poll.activity, (uint64_t)id | ((uint64_t)active << 32),
                          memory_order_release);
    /* 250 Hz matches the Deck's 4 ms reports, independent of rendering rate.
     * The semaphore interrupts the delay during session teardown. */
  } while (!SDL_WaitSemaphoreTimeout(s_poll.stop, 4));
  return 0;
}

bool HostGamepadPoll_Init(void) {
  if (s_poll.thread) return true;
  if (!(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) return false;
  const char *hint = SDL_GetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS);
  if (hint) {
    s_poll.previous_hint = SDL_strdup(hint);
    if (!s_poll.previous_hint) return false;
  }
  s_poll.stop = SDL_CreateSemaphore(0);
  if (s_poll.stop && SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0")) {
    s_poll.owns_hint = true;
    s_poll.thread = SDL_CreateThread(PollGamepads, "Gamepad polling", NULL);
  }
  if (!s_poll.thread) HostGamepadPoll_Shutdown();
  return s_poll.thread != NULL;
}

void HostGamepadPoll_Shutdown(void) {
  if (s_poll.thread) {
    SDL_SignalSemaphore(s_poll.stop);
    SDL_WaitThread(s_poll.thread, NULL);
    s_poll.thread = NULL;
  }
  if (s_poll.stop) SDL_DestroySemaphore(s_poll.stop);
  s_poll.stop = NULL;
  if (s_poll.owns_hint) {
    if (s_poll.previous_hint)
      SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, s_poll.previous_hint);
    else
      SDL_ResetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS);
  }
  SDL_free(s_poll.previous_hint);
  s_poll.previous_hint = NULL;
  s_poll.owns_hint = false;
  atomic_store_explicit(&s_poll.selection, 0, memory_order_relaxed);
  atomic_store_explicit(&s_poll.activity, 0, memory_order_relaxed);
}

bool HostGamepadPoll_Enabled(void) { return s_poll.thread != NULL; }

bool HostGamepadPoll_PhysicalActive(uint32_t id, unsigned deadzone, unsigned threshold) {
  if (!s_poll.thread) return false;
  atomic_store_explicit(&s_poll.selection, (uint64_t)id |
      ((uint64_t)(deadzone & 0xffff) << 32) | ((uint64_t)(threshold & 0xffff) << 48),
      memory_order_release);
  const uint64_t activity = atomic_load_explicit(&s_poll.activity, memory_order_acquire);
  return (uint32_t)activity == id && (activity >> 32) != 0;
}
