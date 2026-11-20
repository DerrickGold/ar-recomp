#ifndef AR_ACTION_EFFECT_VOLUME_H
#define AR_ACTION_EFFECT_VOLUME_H
#include "action_effects.h"

/* Depth is normalized between the rear and front BG1 sheets. Keep admission
 * and geometry on the same envelope, independent of camera and frame time.
 * Attached moss, ground paths, vents and native high clouds own their planes. */
static inline bool ActionEffectVolume_Range(const ActionEffectInstance *e, float *rear, float *front) {
  if(e->projection_plane!=kActionEffectProjectionPlane_Bg1)return false;
  const ActionEffectFieldStyle *s=&e->field_style;
  if(e->kind==kActionEffect_AuthoredFireflies) {
    if(s->glow_style==kActionGlow_Moss)return false;
    *rear=.12f;*front=.88f;return true;
  }
  if(e->kind!=kActionEffect_AuthoredCloud||!s->textured||s->point_count||s->plume)return false;
  if(s->placement==1) {
    /* Explicit floor coverage is independent of the rounded-cloud art style. */
    const float span=s->voxel_depth>0?s->voxel_depth:.2f;
    *rear=.5f-span*.5f;*front=.5f+span*.5f;return true;
  }
  if(s->volume>0) {
    *rear=.08f*s->volume;*front=.6f*s->volume;
    if(s->placement==2) {*rear=1-*front;*front=1-.08f*s->volume;}
  } else {*rear=.12f;*front=.88f;}
  return true;
}
#endif
