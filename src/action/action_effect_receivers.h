#ifndef AR_ACTION_EFFECT_RECEIVERS_H
#define AR_ACTION_EFFECT_RECEIVERS_H
#include "action_effect_render.h"
/* Receiver lighting is sampled at the native object's hot point, consistently
 * across all its OAM parts. It is an object tint, not per-pixel shadow tracing. */
bool ActionEffectReceivers_IsLight(unsigned kind);
uint8_t ActionEffectReceivers_Layer(const ActionEffectInstance *effect);
typedef struct ActionReceiverLighting {
  ActionSceneEffectRenderBatch player, enemies;
  int light_start[2]; /* Additive prefix, multiplicative-light suffix. */
  float dimming;
  ArRenderRectF ramp;
  uint8_t dim_receivers;
  const ActionSceneEffectFrame *frame;
} ActionReceiverLighting;
bool ActionEffectReceivers_Prepare(const ActionSceneEffectFrame *frame,
    unsigned group,unsigned room,int camera_x,int camera_y,int bg2_x,int bg2_y,bool lighting_enabled,
    ActionReceiverLighting *out);
void ActionEffectReceivers_Sample(const ActionReceiverLighting *lighting,
    unsigned receiver,float world_x,float world_y,float multiply[3],float add[3]);
#endif
