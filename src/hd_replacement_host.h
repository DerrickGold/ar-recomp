#ifndef HD_REPLACEMENT_HOST_H
#define HD_REPLACEMENT_HOST_H

#include <stdint.h>

#include <SDL3/SDL.h>

enum { kHdMode7Scale = 4 };

/* Present-time Mode-7 resources. They are host-created and synchronously
 * consumed by present.c; emulated state never owns these allocations. */
extern uint8_t *g_m7_overlay_pixels;
extern SDL_Texture *g_m7_texture;

void HdReplacementHost_LoadTextures(void);
void HdReplacementHost_BindSurfaces(void);
void HdReplacementHost_ReloadTextures(void);
void HdReplacementHost_Shutdown(void);

#endif /* HD_REPLACEMENT_HOST_H */
