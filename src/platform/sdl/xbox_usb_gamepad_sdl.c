#include "host/xbox_usb_gamepad.h"

#if defined(AR_XBOX_USB_BRIDGE)
#include "xbox_sdl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xbox_sdl *s_bridge;
static char s_last_error[256];

void HostXboxUsbGamepad_Start(void) {
  if (s_bridge || !(SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) return;
  const char *enabled = getenv("AR_XBOX_USB");
  if (enabled && !strcmp(enabled, "0")) return;
  char error[256];
  s_bridge = xbox_sdl_start(error, sizeof error);
  if (!s_bridge) fprintf(stderr, "[input] Xbox USB bridge unavailable: %s\n", error);
  else fprintf(stderr, "[input] Xbox USB bridge enabled for 045e:02d1\n");
}

void HostXboxUsbGamepad_Update(void) {
  if (!s_bridge) return;
  char error[256];
  if (xbox_sdl_update(s_bridge, error, sizeof error) < 0) {
    if (strcmp(s_last_error, error)) {
      fprintf(stderr, "[input] Xbox USB bridge: %s\n", error);
      snprintf(s_last_error, sizeof s_last_error, "%s", error);
    }
  } else s_last_error[0] = 0;
}

void HostXboxUsbGamepad_Stop(void) {
  xbox_sdl_stop(s_bridge);
  s_bridge = NULL;
  s_last_error[0] = 0;
}
#else
void HostXboxUsbGamepad_Start(void) {}
void HostXboxUsbGamepad_Update(void) {}
void HostXboxUsbGamepad_Stop(void) {}
#endif
