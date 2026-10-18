#ifndef AR_ACTION_EFFECT_RECEIVERS_H
#define AR_ACTION_EFFECT_RECEIVERS_H
#include "action_effect_render.h"
/* RGB illumination range shared by object sampling and the RGBA8 scenery map.
 * scenery_light.frag.glsl decodes the same range. */
enum { kActionReceiverMaxIllumination = 4 };
/* Receiver lighting is sampled at the native object's hot point, consistently
 * across all its OAM parts. It is an object tint, not per-pixel shadow tracing. */
bool ActionEffectReceivers_IsLight(unsigned kind);
uint8_t ActionEffectReceivers_Mask(const ActionEffectInstance *effect);
uint8_t ActionEffectReceivers_Layer(const ActionEffectInstance *effect);
/* Material attachment is independent of the source's projection/volume depth. */
uint8_t ActionEffectReceivers_SurfaceLayer(const ActionEffectInstance *effect);
bool ActionEffectReceivers_SurfaceLight(const ActionEffectInstance *effect);
bool ActionEffectReceivers_HasSurfaceLights(const ActionSceneEffectFrame *frame);
/* Marks world-space receiver geometry so its radiance calibration matches
 * the scenery light map, independently of visible glow/particle passes. */
bool ActionEffectReceivers_ProjectPoint(void *context, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point);
typedef struct ActionReceiverLighting {
  ActionSceneEffectRenderBatch player, enemies;
  float dimming;
  ArRenderRectF ramp;
  uint8_t dim_receivers;
  const ActionSceneEffectFrame *frame;
} ActionReceiverLighting;
bool ActionEffectReceivers_Prepare(const ActionSceneEffectFrame *frame,
    unsigned group,unsigned room,int camera_x,int camera_y,int bg2_x,int bg2_y,bool lighting_enabled,
    ActionReceiverLighting *out);
/* Dimming establishes ambient illumination. Local RGB light adds to that
 * illumination before it multiplies the original sprite palette; it never
 * adds a flat color over darkened pixels. The runner's additive term is zero. */
void ActionEffectReceivers_Sample(const ActionReceiverLighting *lighting,
    unsigned receiver,float world_x,float world_y,float multiply[3],float add[3]);
#endif
