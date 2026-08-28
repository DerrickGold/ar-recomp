#include "host/host_clock.h"

#include <SDL3/SDL.h>

uint64_t HostClock_Milliseconds(void) {
  return SDL_GetTicks();
}

uint64_t HostClock_Nanoseconds(void) {
  return SDL_GetTicksNS();
}
