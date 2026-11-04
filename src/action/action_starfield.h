#ifndef AR_ACTION_STARFIELD_H
#define AR_ACTION_STARFIELD_H
#include "action_effects.h"
#include "deterministic_hash.h"
#include "render/render_types.h"
#include <math.h>

static inline float ActionStarfield_Unit(uint32_t seed) {
  return (DeterministicHash_Mix32(seed)&65535u)/65535.f;
}
/* Neither star positions nor haze depend on time or particle births. Keep the
 * distribution shared by the capture mask and the native/WASM geometry. */
static inline ArRenderPointF ActionStarfield_Point(const ActionEffectInstance *e,unsigned i) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const uint32_t seed=e->particle_style.seed^i*0x9e3779b9u;
  return (ArRenderPointF){r->x0+(r->x1-r->x0)*ActionStarfield_Unit(seed^71),
      r->y0+(r->y1-r->y0)*ActionStarfield_Unit(seed^91)};
}
static inline ArRenderPointF ActionStarfield_HazePoint(const ActionEffectInstance *e,unsigned col,unsigned row) {
  const ActionEffectLocalRect *r=&e->geometry.data.rect;
  const float u=col/(float)kActionSkyHazeSegments,w=r->x1-r->x0,h=r->y1-r->y0;
  const float phase=ActionStarfield_Unit(e->particle_style.seed^137)*6.2831853f;
  const float centre=r->y0+h*(.48f+.13f*sinf(u*6.2831853f+phase))+
      tanf(e->field_style.angle*.01745329252f)*w*(u-.5f)*.2f;
  return (ArRenderPointF){r->x0+w*u,centre+(row-1.f)*h*.28f};
}
static inline float ActionStarfield_Visibility(const ActionEffectInstance *e,unsigned sample) {
  return !e->field_style.sky.colors?1:e->sky_visibility.valid?e->sky_visibility.alpha[sample]/255.f:0;
}
#endif
