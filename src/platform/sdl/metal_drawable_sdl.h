#ifndef AR_METAL_DRAWABLE_SDL_H
#define AR_METAL_DRAWABLE_SDL_H

/* Main thread, after claiming the window for a Metal GPU device.
 * Resolve SDL's current public Cocoa view on each call; no native object is
 * retained across window/layer replacement. Zero queries without changing it.
 * Returns the actual pool size, or zero when native access is unavailable.
 * Other backends/platforms must keep their own swapchain policy. */
#include <SDL3/SDL.h>

unsigned ArSdlMetalDrawable_Configure(SDL_Window *window, unsigned count);

#endif
