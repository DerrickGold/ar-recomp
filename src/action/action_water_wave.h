#ifndef AR_ACTION_WATER_WAVE_H
#define AR_ACTION_WATER_WAVE_H
#include "action_effects.h"

/* Surface shimmer and its authored reflected light share the same cycle and
 * phase control. No accumulated animation state, including when seeking. */
static inline float ActionWaterWave_Phase(const ActionEffectInstance *e) {
  return e->particle_lifetime ? (e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime : 0;
}
#endif
