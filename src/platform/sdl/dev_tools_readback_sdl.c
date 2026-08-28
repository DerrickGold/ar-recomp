#include "dev_tools_readback_sdl.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "platform/sdl/render_sdl_internal.h"

static void ReleaseSurface(void *owner) {
  SDL_DestroySurface((SDL_Surface *)owner);
}

bool ArSdlDevTools_CaptureRgb24(
    void *device_context, DevToolsRgb24Capture *capture) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(
      (ArRenderDevice *)device_context);
  if (!renderer || !capture) return false;
  memset(capture, 0, sizeof(*capture));

  SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
  if (!raw) return false;
  SDL_Surface *rgb = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGB24);
  SDL_DestroySurface(raw);
  if (!rgb) return false;

  *capture = (DevToolsRgb24Capture){
    .pixels = (const uint8_t *)rgb->pixels,
    .width = rgb->w,
    .height = rgb->h,
    .pitch_bytes = rgb->pitch,
    .owner = rgb,
    .release = ReleaseSurface,
  };
  return true;
}
