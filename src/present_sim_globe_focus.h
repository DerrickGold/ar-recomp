/* Presentation-private focus policy. The GPU sees only normalized source-mask
 * coordinates and a color treatment; no town identity or setting leaks into
 * its material contract. Model colors use the same transfer at publication. */
#ifndef PRESENT_SIM_GLOBE_FOCUS_H
#define PRESENT_SIM_GLOBE_FOCUS_H
#include <math.h>
#include "present_sim_globe_mapping.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_render_metadata.h"

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

/* Restore the ground cue at the captured emitter window, not at the full
 * town boundary. Native town XY -> biased sprite anchors is the same mapping
 * used by SimCullProximityAt. Camera pitch/zoom and framebuffer size do not
 * enter this policy; only native pan, captured margins and lift compensation.
 * Mask units are chart cells / 128, shared with the coastal water source. */
static inline Sim3DDepthSurfaceFocus PresentSimGlobeFocus_ResolveVisibility(
    const SimGlobeMapping *map, const SimFrameData *sim, ArRenderRectI source) {
  if (!map || !map->town || !sim || !(sim->effective_features & kSimFeature_CullHaze))
    return (Sim3DDepthSurfaceFocus){0};
  const unsigned lifted = kSimFeature_ObjectBillboards | kSimFeature_VirtualHeight;
  const float lift = sim->cull_lift_inset && (sim->effective_features & lifted) == lifted
      ? Sim3D_MaxDrawLift(sim->height_scale_x100) : 0;
  const float width = kSimSpriteWindowBiasedWidth + sim->sprite_margin_left + sim->sprite_margin_right;
  const float height = kSimSpriteWindowBiasedHeight + sim->sprite_margin_top + sim->sprite_margin_bottom;
  const float lead = sim->cull_haze_lead_px ? sim->cull_haze_lead_px : kSimCullHazeLeadDefaultPx;
  const float units = 16.0f * 128;
  return (Sim3DDepthSurfaceFocus){
    .clear_rect = {
      map->origin_x/128 + (sim->camera_x-16.0f-sim->sprite_margin_left)/units,
      map->origin_y/128 + (sim->camera_y+source.y-17.0f-sim->sprite_margin_top)/units,
      width/units, (height-fminf(lift,height*.5f))/units},
    .feather = lead/units, .inset = lead/units,
    .corner_radius = sim->cull_corner_px/units,
    .dim = fminf(1,sim->cull_dim_pct/100.0f),
    .haze = {.24f,.37f,.56f,fminf(1,sim->cull_haze_pct/100.0f)}};
}

static inline float PresentSimGlobeFocus_Weight(const Sim3DDepthSurfaceFocus *focus,
    float x, float y) {
  if (!focus->dim && !focus->haze.a) return 0;
  const ArRenderRectF r=focus->clear_rect;
  const float dx=fmaxf(0,fmaxf(r.x-x,x-r.x-r.w));
  const float dy=fmaxf(0,fmaxf(r.y-y,y-r.y-r.h));
  float distance=hypotf(dx,dy);
  if (focus->corner_radius>0 || focus->inset>0) {
    const float hx=r.w*.5f, hy=r.h*.5f;
    const float radius=fminf(focus->corner_radius,fminf(hx,hy));
    const float qx=fabsf(x-r.x-hx)-(hx-radius), qy=fabsf(y-r.y-hy)-(hy-radius);
    distance=hypotf(fmaxf(qx,0),fmaxf(qy,0))+fminf(fmaxf(qx,qy),0)-radius+focus->inset;
  }
  const float t=focus->feather>0 ? fmaxf(0,fminf(1,distance/focus->feather)) : 1;
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
