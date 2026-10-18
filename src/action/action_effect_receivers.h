#ifndef AR_ACTION_EFFECT_RECEIVERS_H
#define AR_ACTION_EFFECT_RECEIVERS_H
#include "action_effect_render.h"
/* RGB illumination range shared by object sampling and the RGBA8 scenery map.
 * scenery_light.frag.glsl decodes the same range. */
enum { kActionReceiverMaxIllumination = 4 };
/* Ambient exposure is sampled at the native object's hot point, consistently
 * across its OAM parts. Diorama adds projected light per sprite pixel; flat
 * presentation retains the world-space object sampling fallback. */
bool ActionEffectReceivers_IsLight(unsigned kind);
uint8_t ActionEffectReceivers_Mask(const ActionEffectInstance *effect);
/* Conservative family routing: a member can opt into a receiver even when
 * its parent opts out. Individual geometry still uses the inherited mask. */
uint8_t ActionEffectReceivers_FamilyMask(const ActionEffectInstance *effect,
    const ActionNativeMembers *members);
bool ActionEffectReceivers_Member(const ActionEffectInstance *effect,
    const ActionNativeMember *member, unsigned receiver);
uint8_t ActionEffectReceivers_Layer(const ActionEffectInstance *effect);
/* Material attachment is independent of the source's projection/volume depth. */
uint8_t ActionEffectReceivers_SurfaceLayer(const ActionEffectInstance *effect);
bool ActionEffectReceivers_SurfaceLight(const ActionEffectInstance *effect);
bool ActionEffectReceivers_HasSurfaceLights(const ActionSceneEffectFrame *frame);
/* Resolve after recipe application, so native defaults and member overrides
 * cannot be accidentally excluded by a separate manifest-only fast path. */
bool ActionEffectReceivers_MapsMatch(const ActionSceneEffectFrame *, unsigned a, unsigned b);
bool ActionEffectReceivers_HasActors(const ActionSceneEffectFrame *frame, bool lighting_enabled);
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
/* Compact companion pixel: ambient in R (1/64 steps), role weights G/B.
 * Alpha is coverage, allowing the existing OBJ warp to transport it unchanged. */
uint32_t ActionEffectReceivers_Attribute(const ActionReceiverLighting *, unsigned receiver,
    float world_x, float world_y);
float ActionEffectReceivers_Ambient(const ActionReceiverLighting *, unsigned receiver,
    float world_x, float world_y);
void ActionEffectReceivers_Sample(const ActionReceiverLighting *lighting,
    unsigned receiver,float world_x,float world_y,float multiply[3],float add[3]);
#endif
