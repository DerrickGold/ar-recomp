#ifndef AR_PRESENT_SIM3D_CANVAS_H
#define AR_PRESENT_SIM3D_CANVAS_H

#include "render/render_device.h"

/* Render-thread cache of the raw town canvas. Call before releasing the frame
 * producer, while its pixels are stable. A skipped consumer invalidates
 * publication so returning to it uploads the complete current CPU image. */
void PresentSim3DCanvas_Upload(ArRenderDevice *device, bool needed);
ArRenderTexture PresentSim3DCanvas_Texture(uint32_t serial);
void PresentSim3DCanvas_Reset(ArRenderDevice *device);

#endif
