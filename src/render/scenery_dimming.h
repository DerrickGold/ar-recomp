#ifndef AR_SCENERY_DIMMING_H
#define AR_SCENERY_DIMMING_H
#include "render/render_device.h"
#include <math.h>

/* A zero-sized ramp preserves uniform room dimming. Coordinates may be world
 * pixels or source UVs, provided the point and ramp use the same space. */
static inline float SceneryDimming_Amount(float amount, ArRenderRectF ramp, float x, float y) {
  if (ramp.w <= 0 || ramp.h <= 0) return amount;
  const float u = fminf(1, fmaxf(0, (x-ramp.x)/ramp.w));
  const float v = fminf(1, fmaxf(0, (y-ramp.y)/ramp.h));
  return amount*u*v;
}
#endif
