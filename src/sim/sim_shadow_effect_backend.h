#ifndef AR_SIM_SHADOW_EFFECT_BACKEND_H
#define AR_SIM_SHADOW_EFFECT_BACKEND_H

#include <stdbool.h>

#include "render/render_device.h"

/* Optional separable blur for the SIM shadow mask. The scene owns the blur
 * radius and axis; the platform backend owns native shader formats, state,
 * and lifetime. A declined effect retains the established multi-draw fallback. */
typedef struct SimShadowBlurEffectParams {
  float texel_x;
  float texel_y;
  float radius;
} SimShadowBlurEffectParams;

bool SimShadowEffectBackend_IsAvailable(ArRenderDevice *device);
bool SimShadowEffectBackend_BindBlur(
    ArRenderDevice *device, const SimShadowBlurEffectParams *params);
bool SimShadowEffectBackend_Unbind(ArRenderDevice *device);
void SimShadowEffectBackend_Reset(ArRenderDevice *device);

#endif /* AR_SIM_SHADOW_EFFECT_BACKEND_H */
