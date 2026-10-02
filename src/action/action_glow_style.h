#ifndef AR_ACTION_GLOW_STYLE_H
#define AR_ACTION_GLOW_STYLE_H
#include "render/render_types.h"
typedef struct ActionEffectGlowStyle {
  float radius_x, radius_y;
  float ring_scale[3];  /* fraction of the outer radius */
  ArRenderColorF centre;
  ArRenderColorF ring[3];
  float flare;   /* outer-ring silhouette amplitude, fraction of radius */
  float rise;    /* aura offset along (lift_x,lift_y), fraction of radius */
  /* Unit vector for the ellipse's local +X. (1,0) leaves the body screen-
   * aligned, which is right for a fire standing in place. A projectile must
   * instead be oriented along its own heading, or its glow reads as a
   * screen-axis blob stuck to a sprite that is plainly travelling diagonally. */
  float axis_x, axis_y;
  /* Unit vector the outer rings shift toward. (0,-1) is screen-up, i.e. hot
   * gas rising; an aligned body instead trails backwards along its heading. */
  float lift_x, lift_y;
  unsigned seed; /* silhouette phase; distinct per flame so they churn apart */
} ActionEffectGlowStyle;
#endif
