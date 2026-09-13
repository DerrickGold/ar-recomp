#ifndef AR_RENDER_PREPARATION_H
#define AR_RENDER_PREPARATION_H

#include "render_capabilities.h"
#include "render/render_device.h"

/* Owner-thread boot/reset work. Probes only scratch resources, never game
 * state. False means caller render state could not be safely established or
 * restored; individual unsupported features are reported in the mask. */
bool RenderPreparation_Prepare(ArRenderDevice *device, RenderFeatureMask *supported);

#endif
