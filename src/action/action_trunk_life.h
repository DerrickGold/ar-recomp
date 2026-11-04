#ifndef AR_ACTION_TRUNK_LIFE_H
#define AR_ACTION_TRUNK_LIFE_H
#include "action_effects.h"
#include "action_trunk.h"
#include <math.h>
void ActionTrunk_Capture(ActionTrunkObserver *,ActionSceneEffectFrame *,
    const ActionTrunkAssets *,DioramaTrunkSettings,unsigned clock,bool particles);
static inline const ActionEffectInstance *ActionTrunk_Opening(const ActionSceneEffectFrame *f,float x,float y) {
  if(!f||!f->trunk.enabled)return NULL;
  const ActionEffectInstance *best=NULL;float distance=180*180;
  for(unsigned i=0;i<f->authored_count;++i) {
    const ActionEffectInstance *e=&f->authored[i];
    if(!e->field_style.opening||e->kind!=kActionEffect_AuthoredFan||e->tuning.intensity<=0)continue;
    const float dx=e->world_x-x,dy=e->world_y-y,d=dx*dx+dy*dy;
    if(d<distance){best=e;distance=d;}
  }
  return best;
}
/* A smooth local gust with a travel delay. Source clocks, not the camera or
 * wall time, drive it. Seeds differ by opening; no simultaneous room-wide pulse. */
static inline float ActionTrunk_Gust(const ActionEffectInstance *opening,float distance) {
  if(!opening)return 0;
  const float phase=(opening->pulse_ticks-opening->world_y*.73f-distance*.65f)*.0071f;
  float s=fmaxf(0,sinf(phase));s*=s;
  return s*s;
}
#endif
