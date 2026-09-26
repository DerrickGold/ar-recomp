#ifndef AR_HOST_VIDEO_H
#define AR_HOST_VIDEO_H
/* HostVideo: the game window and the render device created on it. Boot
 * creates both once, after SDL_Init; shutdown destroys them last, after
 * everything that owns a texture or render state. In between every host and
 * present stage renders through them on the main thread. A pure-headless run
 * creates neither: the window stays NULL and the device never becomes ready.
 * Phase: host (main thread). */

#include <stdbool.h>

#include "render/render_device.h"

struct SDL_Window;

extern struct SDL_Window *g_window;
extern ArRenderDevice g_render_device;
/* The SDL GPU renderer is the presentation backend. Individual optional
 * shader effects still check their own AR_GPU_FX_* toggles; this flag reports
 * that the mandatory GPU device and renderer were created successfully. */
extern bool g_gpu_shaders_active;

/* Creates the window, the SDL GPU render device on it, runs the renderer's
 * startup preparation, and applies the display settings that need a renderer.
 * A hidden_capture window (AR_HEADLESS_VIDEO) stays hidden, runs without vsync
 * and never goes exclusive fullscreen. Dies on failure. */
void HostVideo_Create(const char *title, bool hidden_capture);
/* Raises the window and takes keyboard focus; a refusal is only reported. */
void HostVideo_TakeFocus(void);
void HostVideo_Destroy(void);

#endif  /* AR_HOST_VIDEO_H */
