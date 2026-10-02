#ifndef AR_ACTION_ENVIRONMENT_EXPOSURE_H
#define AR_ACTION_ENVIRONMENT_EXPOSURE_H
#include "action_effects.h"
#include "actraiser_game.h"
#include "render/render_types.h"
/* Visual strengths and ramps belong to retained recipes. These checks only
 * establish that the corresponding observed/validated source is active. */
typedef struct ActionEnvironmentExposure {float amount;ArRenderRectF ramp;} ActionEnvironmentExposure;
static inline ActionEnvironmentExposure ActionEnvironment_Exposure(const ActionSceneEffectFrame *frame,unsigned group,unsigned room){
  ActionEnvironmentExposure result={0};
  if(!frame||frame->decoration_overflow||frame->decoration_count>kActionSceneDecorationMaxInstances)return result;
  for(unsigned i=0;i<frame->decoration_count;++i){const ActionEffectInstance *e=&frame->decorations[i];
    if(!(e->flags&kActionEffectFlag_Visible)||e->environment_room!=room||e->projection_plane!=kActionEffectProjectionPlane_Bg1)continue;
    const float *amount=NULL,*ramp=NULL;
    if(e->kind==kActionEffect_CaveAmbientLight&&e->phase==kActionEffectPhase_CaveEnvironment&&e->render_layer==kActionEffectRenderLayer_ForegroundLight){
      const ActionAtmosphereField *f=frame->atmosphere_field_valid?&frame->atmosphere_field:
          group==kActRaiserMapGroup_Fillmore?ActionAtmosphereField_Bundled(room):NULL;
      if(f){amount=f->Dimming;ramp=f->DimmingRamp;}
    }else if(e->kind==kActionEffect_CastleLight&&e->phase==kActionEffectPhase_CastleEnvironment&&e->render_layer==kActionEffectRenderLayer_Bg1Plane&&e->source_mask){
      const ActionCastleField *f=frame->castle_field_valid?&frame->castle_field:
          group==kActRaiserMapGroup_Bloodpool?ActionCastleField_Bundled(room):NULL;
      if(f){amount=f->Dimming;ramp=f->DimmingRamp;}
    }
    if(amount&&*amount>result.amount)result=(ActionEnvironmentExposure){*amount,{ramp[0],ramp[1],ramp[2],ramp[3]}};
  }
  return result;
}
static inline float ActionEnvironment_NativeBg1Dimming(const ActionSceneEffectFrame *frame,unsigned group,unsigned room){
  return ActionEnvironment_Exposure(frame,group,room).amount;
}

static inline float ActionEnvironment_Bg1Dimming(const ActionSceneEffectFrame *frame,unsigned group,unsigned room) {
  const float amount=ActionEnvironment_NativeBg1Dimming(frame,group,room);
  if(frame && !frame->decoration_overflow && frame->decoration_count<=kActionSceneDecorationMaxInstances)for(unsigned i=0;i<frame->decoration_count;++i) {
    const ActionEffectInstance *e=&frame->decorations[i];
    if((e->kind==kActionEffect_CaveAmbientLight||e->kind==kActionEffect_CastleLight)&&
        e->tuning.dim_receivers_set&&!(e->tuning.dim_receivers&kActionReceiver_Scenery))return 0;
  }
  return amount;
}

static inline ArRenderRectF ActionEnvironment_Bg1DimmingRamp(const ActionSceneEffectFrame *frame,unsigned group,unsigned room){
  return ActionEnvironment_Exposure(frame,group,room).ramp;
}
#endif
