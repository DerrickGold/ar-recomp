#include "host/gamepad_poll.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct Device {
  SDL_Semaphore *entered, *release;
  atomic_bool block;
} Device;

static void SDLCALL Update(void *context) {
  Device *device = context;
  if (atomic_exchange(&device->block, false)) {
    SDL_SignalSemaphore(device->entered);
    assert(SDL_WaitSemaphoreTimeout(device->release, 2000));
  }
}

static void WaitActivity(SDL_JoystickID id, bool expected) {
  const uint64_t timeout = SDL_GetTicksNS() + 2000000000;
  while (HostGamepadPoll_PhysicalActive(id, 10000, 20000) != expected) {
    assert(SDL_GetTicksNS() < timeout);
    SDL_Delay(1);
  }
}

static void WaitEvent(SDL_JoystickID id, unsigned type) {
  const uint64_t timeout = SDL_GetTicksNS() + 2000000000;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      if (e.type == type && e.gdevice.which == id) return;
    assert(SDL_GetTicksNS() < timeout);
    SDL_Delay(1);
  }
}

int main(void) {
  assert(!HostGamepadPoll_Init());
  assert(SDL_Init(SDL_INIT_GAMEPAD));
  /* A higher-priority caller hint must cause a clean synchronous fallback. */
  assert(SDL_SetHintWithPriority(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1", SDL_HINT_OVERRIDE));
  assert(!HostGamepadPoll_Init());
  assert(!HostGamepadPoll_Enabled());
  assert(SDL_GetHintBoolean(SDL_HINT_AUTO_UPDATE_JOYSTICKS, false));
  SDL_ResetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS);
  assert(HostGamepadPoll_Init());
  HostGamepadPoll_Shutdown();
  assert(SDL_GetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS) == NULL);
  assert(SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1"));
  for (int lifetime = 0; lifetime < 3; ++lifetime) {
    Device device = {0};
    device.entered = SDL_CreateSemaphore(0);
    device.release = SDL_CreateSemaphore(0);
    assert(device.entered && device.release);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "Asynchronous input test";
    desc.userdata = &device;
    desc.Update = Update;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id);
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    assert(pad);
    SDL_Joystick *joystick = SDL_GetGamepadJoystick(pad);
    assert(HostGamepadPoll_Init());
    assert(HostGamepadPoll_Init());
    assert(!SDL_GetHintBoolean(SDL_HINT_AUTO_UPDATE_JOYSTICKS, true));
    HostGamepadPoll_PhysicalActive(id, 10000, 20000);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    WaitEvent(id, SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    WaitActivity(id, true);
    assert(!HostGamepadPoll_PhysicalActive(id + 1000, 10000, 20000));
    WaitActivity(id, true);

    /* Emulate a blocked HID feature report while the main thread pumps input
     * and reads arbitration state. Neither is allowed to join the HID wait. */
    atomic_store(&device.block, true);
    assert(SDL_WaitSemaphoreTimeout(device.entered, 2000));
    const uint64_t start = SDL_GetTicksNS();
    SDL_PumpEvents();
    assert(HostGamepadPoll_PhysicalActive(id, 10000, 20000));
    assert(SDL_GetTicksNS() - start < 100000000);
    SDL_SignalSemaphore(device.release);

    assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false));
    WaitEvent(id, SDL_EVENT_GAMEPAD_BUTTON_UP);
    WaitActivity(id, false);
    assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -25000));
    WaitEvent(id, SDL_EVENT_GAMEPAD_AXIS_MOTION);
    WaitActivity(id, true);
    assert(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 0));
    WaitActivity(id, false);

    /* Removal during polling must release physical activity and the handle. */
    assert(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    WaitActivity(id, true);
    assert(SDL_DetachVirtualJoystick(id));
    WaitEvent(id, SDL_EVENT_GAMEPAD_REMOVED);
    SDL_CloseGamepad(pad);
    WaitActivity(id, false);
    HostGamepadPoll_Shutdown();
    HostGamepadPoll_Shutdown();
    assert(!HostGamepadPoll_Enabled());
    assert(SDL_GetHintBoolean(SDL_HINT_AUTO_UPDATE_JOYSTICKS, false));
    SDL_DestroySemaphore(device.entered);
    SDL_DestroySemaphore(device.release);
  }
  SDL_Quit();
  puts("gamepad_poll_test: PASS");
  return 0;
}
