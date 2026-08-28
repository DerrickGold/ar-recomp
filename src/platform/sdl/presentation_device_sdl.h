#ifndef AR_PLATFORM_SDL_PRESENTATION_DEVICE_H
#define AR_PLATFORM_SDL_PRESENTATION_DEVICE_H

#include <stdbool.h>

#include "render/render_device.h"

/* Apply host logical-presentation policy to the renderer owned by `device`.
 * This keeps the native renderer handle inside the SDL platform adapter. */
bool ArSdlPresentationDevice_ApplyLogical(
    ArRenderDevice *device, bool stretch, bool crt_pixel_aspect,
    int visible_width, int visible_height);

#endif /* AR_PLATFORM_SDL_PRESENTATION_DEVICE_H */
