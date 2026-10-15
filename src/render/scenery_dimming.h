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
/* Captures have an apron/extended top; convert world ramps once at the draw
 * boundary. Both the native host and WASM preview use this same conversion. */
static inline ArRenderRectF SceneryDimming_ToUv(ArRenderRectF ramp,float origin_x,float origin_y,float width,float height) {
  if(ramp.w>0&&ramp.h>0) {
    ramp.x=(ramp.x-origin_x)/width;ramp.y=(ramp.y-origin_y)/height;
    ramp.w/=width;ramp.h/=height;
  }
  return ramp;
}
#endif
