#ifndef AR_ACTION_HEAT_H
#define AR_ACTION_HEAT_H
#include "action_effects.h"
#include "diorama/diorama_heat.h"

static inline DioramaHeat ActionHeat_Prepare(const ActionSceneEffectFrame *frame,
    ActionEffectProjectionPlane plane,float origin_x,float origin_y,float width,float height) {
  DioramaHeat out={.source={origin_x,origin_y,width,height}};
  unsigned n=0;
  if(!frame||frame->authored_count>kActionAuthoredMaxInstances)return out;
  for(unsigned i=0;i<frame->authored_count&&n<kDioramaHeatRegions;++i) {
    const ActionEffectInstance *e=&frame->authored[i];
    if(e->kind!=kActionEffect_AuthoredHeat||e->projection_plane!=plane||
        !(e->flags&kActionEffectFlag_Visible)||!e->particle_lifetime)continue;
    const ActionEffectLocalRect *r=&e->geometry.data.rect;
    out.bounds[n][0]=e->world_x+r->x0;out.bounds[n][1]=e->world_y+r->y0;
    out.bounds[n][2]=e->world_x+r->x1;out.bounds[n][3]=e->world_y+r->y1;
    /* Cull the full BG1 footprint, never its centre. BG2 skyboxes may remap
     * outside the captured interval, so keep their existing unbounded list. */
    if(plane==kActionEffectProjectionPlane_Bg1 &&
        (out.bounds[n][2]<=origin_x||out.bounds[n][0]>=origin_x+width||
         out.bounds[n][3]<=origin_y||out.bounds[n][1]>=origin_y+height))continue;
    out.motion[n][0]=fminf(4,e->field_style.amplitude*e->tuning.intensity);
    out.motion[n][1]=(e->pulse_ticks%e->particle_lifetime)*6.2831853f/e->particle_lifetime;
    ++n;
  }
  return out;
}
#endif
