#ifndef AR_SIM_CLOUD_EFFECT_BACKEND_H
#define AR_SIM_CLOUD_EFFECT_BACKEND_H

#include "render/render_device.h"

enum { kSimCloudEffectSamples = 3 };
typedef struct SimCloudEffectSample {
  float scale, offset_x, offset_y, opacity;
} SimCloudEffectSample;
typedef struct SimCloudEffectParams {
  SimCloudEffectSample samples[kSimCloudEffectSamples];
} SimCloudEffectParams;

/* Optional ordered cloud-bank composition. Input geometry carries unscaled
 * base UVs, white RGB and coverage in vertex alpha. The presenter owns sampling policy;
 * the adapter copies uniforms and owns shader formats/state. All calls are
 * owner-thread-only. Always Unbind after a Bind attempt before any fallback.
 * No fallback draw is allowed after a successfully bound draw was attempted. */
bool SimCloudEffectBackend_IsAvailable(ArRenderDevice *device);
bool SimCloudEffectBackend_Bind(ArRenderDevice *device, const SimCloudEffectParams *params);
bool SimCloudEffectBackend_Unbind(ArRenderDevice *device);
void SimCloudEffectBackend_Reset(ArRenderDevice *device);

#endif
