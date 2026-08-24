#ifndef HD_REPLACEMENT_HOST_H
#define HD_REPLACEMENT_HOST_H

#include <stdbool.h>
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

/* Session-only demand gate for the independent authentic PPU scanout. */
void ActRaiser_SetAuthenticCaptureEnabled(bool enabled);
bool ActRaiser_AuthenticCaptureEnabled(void);
/* Zero means no complete pass exists for the current surface/geometry. The
 * serial advances only after the final scanline of a bound native pass. */
void ActRaiser_AuthenticCaptureFrameCompleted(bool frame_valid);
uint64_t ActRaiser_AuthenticFrameSerial(void);

#endif /* HD_REPLACEMENT_HOST_H */
