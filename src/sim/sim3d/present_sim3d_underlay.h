#ifndef AR_PRESENT_SIM3D_UNDERLAY_H
#define AR_PRESENT_SIM3D_UNDERLAY_H

#include "render/render_device.h"

typedef struct SimUnderlayTextures {
  ArRenderTexture sharp, blurred;
} SimUnderlayTextures;

/* Presentation-owned, single-device cache. Call on the render owner thread;
 * reset before retiring that device. Only current-revision textures escape.
 * An unused blur is neither allocated nor refreshed. Enabling it later must
 * first catch up to the requested revision, even if sharp is already current. */
SimUnderlayTextures PresentSim3DUnderlay_Prepare(
    ArRenderDevice *device, uint32_t serial, bool want_blur);
void PresentSim3DUnderlay_ResetResources(ArRenderDevice *device);

#endif
