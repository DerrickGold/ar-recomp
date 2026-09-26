#ifndef AR_PRESENT_SIM3D_UNDERLAY_H
#define AR_PRESENT_SIM3D_UNDERLAY_H
/* PresentSim3DUnderlay: textures of the baked Mode-7 world map drawn under the
 * town as distant ground, a sharp copy and an optional 4 x 4 box-blurred one,
 * uploaded again only when the world map's serial changes.
 * Phase: present.
 * Tests: tests/present_sim3d_underlay_test.c */

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
