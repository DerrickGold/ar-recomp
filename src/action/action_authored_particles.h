#ifndef AR_ACTION_AUTHORED_PARTICLES_H
#define AR_ACTION_AUTHORED_PARTICLES_H
#include "action_effects.h"
#include <math.h>

/* Effective, immutable particle parameters. Older recipes keep their existing
 * size distribution, source-derived seed and one-field-height rise per cycle. */
static inline ActionEffectParticleStyle ActionAuthoredParticles_Default(float height, uint32_t seed) {
  return (ActionEffectParticleStyle){.size_min=.35f,.size_max=.75f,
      .travel_y=-height,.wander=2,.spread=1,.seed=seed,.active=1};
}
static inline bool ActionAuthoredParticles_Valid(const ActionEffectParticleStyle *s) {
  return s && isfinite(s->size_min) && isfinite(s->size_max) &&
      s->size_min >= .1f && s->size_max <= 4 && s->size_min <= s->size_max &&
      isfinite(s->travel_x) && s->travel_x >= -512 && s->travel_x <= 512 &&
      isfinite(s->travel_y) && s->travel_y >= -512 && s->travel_y <= 512 &&
      isfinite(s->wander) && s->wander >= 0 && s->wander <= 32 &&
      isfinite(s->spread) && s->spread >= 0 && s->spread <= 1;
}
/* Emission bounds and visible motion bounds are separate. Culling a field by
 * its birth area would lose drifting motes before they scroll out of view. */
static inline ActionEffectLocalRect ActionAuthoredParticles_Bounds(
    const ActionEffectLocalRect *r, const ActionEffectParticleStyle *s) {
  const float radius=fmaxf(.6f,s->size_max)+1;
  const float half=(r->x1-r->x0)*s->spread*.5f;
  const float center=(r->x0+r->x1)*.5f;
  const float start_y=s->travel_y < 0 ? r->y1 : r->y0;
  return (ActionEffectLocalRect){
    center-half+fminf(0,s->travel_x)-s->wander-radius,
    fminf(r->y0,start_y+s->travel_y)-radius,
    center+half+fmaxf(0,s->travel_x)+s->wander+radius,
    fmaxf(r->y1,start_y+s->travel_y)+radius};
}
#endif
