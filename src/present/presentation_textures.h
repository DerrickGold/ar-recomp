#ifndef AR_PRESENTATION_TEXTURES_H
#define AR_PRESENTATION_TEXTURES_H
/* PresentationTextures: the GPU textures each frame's host surfaces are
 * uploaded into and the present stages sample: the base framebuffer, the
 * authentic comparison frame. Diorama, HUD and SIM resources are feature-owned.
 * Created once the render device exists and destroyed just before it; in between the handles
 * only change across a render device reset. They are presentation resources,
 * not live game state, so the D6 boundary does not fence them off.
 * Phase: host (main thread). */

#include <stdbool.h>

#include "render/render_types.h"

extern ArRenderTexture g_texture;
extern ArRenderTexture g_authentic_texture;
/* Creates every texture. Dies when an enabled SIM 3D feature cannot get the
 * textures it needs. */
void PresentationTextures_Create(void);
/* After SDL_EVENT_RENDER_DEVICE_RESET: re-creates the textures whose content
 * is not re-uploaded every frame (the zero-filled diorama planes). */
void PresentationTextures_HandleDeviceReset(void);
void PresentationTextures_Destroy(void);

#endif  /* AR_PRESENTATION_TEXTURES_H */
