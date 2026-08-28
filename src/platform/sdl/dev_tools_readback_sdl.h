#ifndef AR_PLATFORM_SDL_DEV_TOOLS_READBACK_H
#define AR_PLATFORM_SDL_DEV_TOOLS_READBACK_H

#include "dev/dev_tools_readback.h"
#include "render/render_device.h"

/* Capture the current SDL renderer output as row-pitched RGB24 bytes. The
 * returned capture owns a converted surface and must be released by caller. */
bool ArSdlDevTools_CaptureRgb24(void *device_context,
                                DevToolsRgb24Capture *capture);

#endif /* AR_PLATFORM_SDL_DEV_TOOLS_READBACK_H */
