#ifndef AR_PRESENTATION_DEVICE_SDL_H
#define AR_PRESENTATION_DEVICE_SDL_H

#include <stdbool.h>

#include "render/render_device.h"

/* Apply host logical-presentation policy to the renderer owned by `device`.
 * This keeps the native renderer handle inside the SDL platform adapter. */
bool ArSdlPresentationDevice_ApplyLogical(
    ArRenderDevice *device, bool stretch, bool crt_pixel_aspect,
    int visible_width, int visible_height);

#endif /* AR_PRESENTATION_DEVICE_SDL_H */
