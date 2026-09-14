/* Presentation-private focus policy. The GPU sees only normalized source-mask
 * coordinates and a color treatment; no town identity or setting leaks into
 * its material contract. Model colors use the same transfer at publication. */
#ifndef PRESENT_SIM_GLOBE_FOCUS_H
#define PRESENT_SIM_GLOBE_FOCUS_H
#include <math.h>
#include "present_sim_globe_mapping.h"
#include "sim/sim3d_depth_pass.h"

static inline Sim3DDepthSurfaceFocus PresentSimGlobeFocus_Resolve(
    const SimGlobeMapping *map, bool enabled, unsigned dim, unsigned haze,
    unsigned lead_pixels) {
  if (!enabled || !map || !map->town)
    return (Sim3DDepthSurfaceFocus){0};
  /* Four chart cells minimum makes a soft geographic transition outside the
   * complete playable town, independent of camera pan/zoom or viewport size. */
  return (Sim3DDepthSurfaceFocus){
    .clear_rect={map->origin_x/128,map->origin_y/128,32.0f/128,32.0f/128},
    .feather=fmaxf(4,lead_pixels/16.0f)/128,
    .dim=fminf(1,dim/100.0f), .haze={.24f,.37f,.56f,fminf(1,haze/100.0f)}};
}

static inline float PresentSimGlobeFocus_Weight(const Sim3DDepthSurfaceFocus *focus,
    float x, float y) {
  if (!focus->dim && !focus->haze.a) return 0;
  const ArRenderRectF r=focus->clear_rect;
  const float dx=fmaxf(0,fmaxf(r.x-x,x-r.x-r.w));
  const float dy=fmaxf(0,fmaxf(r.y-y,y-r.y-r.h));
  const float t=focus->feather>0 ? fminf(1,hypotf(dx,dy)/focus->feather) : 1;
  return t*t*(3-2*t);
}

static inline ArRenderColorF PresentSimGlobeFocus_Color(
    const Sim3DDepthSurfaceFocus *focus, float weight, ArRenderColorF color) {
  const float haze=focus->haze.a*weight;
  const float gain=(1-focus->dim*weight)*(1-haze);
  return (ArRenderColorF){color.r*gain+focus->haze.r*haze,
    color.g*gain+focus->haze.g*haze,color.b*gain+focus->haze.b*haze,color.a};
}
#endif
