#ifndef AR_ACTION_LIGHT_KINDS_H
#define AR_ACTION_LIGHT_KINDS_H
#include "action_effects.h"
#include <math.h>
enum { kActionEffectOpacityIntensityMax = 4, kActionLightIntensityMax = 16 };
/* Preserve the established opacity curve through 4x. Stronger sources gain
 * radiance instead of flattening their falloff against an alpha clamp. */
static inline float ActionLightIntensity_Opacity(float intensity) {
  return fminf(intensity, kActionEffectOpacityIntensityMax);
}
static inline float ActionLightIntensity_Gain(float intensity) {
  return fmaxf(1, intensity / kActionEffectOpacityIntensityMax);
}
/* Families that actually submit lighting geometry. Keep data validation and
 * receiver sampling on the same catalog; particle-only sources stay excluded. */
static inline bool ActionLightKind_Supported(unsigned kind) {
  switch(kind) {
#define ACTION_LIGHT_RESPONSE(symbol,name,gain,intensity,reach,receivers) case kActionEffect_##symbol: return true;
#include "action_light_response.inc"
#undef ACTION_LIGHT_RESPONSE
    default:return false;
  }
}
static inline float ActionLightKind_Illumination(unsigned kind) {
  switch(kind) {
#define ACTION_LIGHT_RESPONSE(symbol,name,gain,intensity,reach,receivers) case kActionEffect_##symbol: return gain;
#include "action_light_response.inc"
#undef ACTION_LIGHT_RESPONSE
    default:return 1;
  }
}

static inline float ActionLightKind_DefaultIntensity(unsigned kind) {
  switch(kind) {
#define ACTION_LIGHT_RESPONSE(symbol,name,gain,intensity,reach,receivers) case kActionEffect_##symbol: return intensity;
#include "action_light_response.inc"
#undef ACTION_LIGHT_RESPONSE
    default:return 1;
  }
}
static inline float ActionLightKind_DefaultReach(unsigned kind) {
  switch(kind) {
#define ACTION_LIGHT_RESPONSE(symbol,name,gain,intensity,reach,receivers) case kActionEffect_##symbol: return reach;
#include "action_light_response.inc"
#undef ACTION_LIGHT_RESPONSE
    default:return 1;
  }
}

/* Default receiving surfaces are data, shared with the editor. */
static inline uint8_t ActionLightKind_DefaultReceivers(unsigned kind) {
  switch(kind) {
#define ACTION_LIGHT_RESPONSE(symbol,name,gain,intensity,reach,receivers) case kActionEffect_##symbol: return receivers;
#include "action_light_response.inc"
#undef ACTION_LIGHT_RESPONSE
    default:return kActionReceiver_Scenery;
  }
}

static inline float ActionLightKind_MaxIntensity(unsigned kind) {
  return ActionLightKind_Supported(kind) ? kActionLightIntensityMax : kActionEffectOpacityIntensityMax;
}
#endif
