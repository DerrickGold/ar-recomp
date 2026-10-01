#ifndef AR_ACTION_ENVIRONMENT_EXPOSURE_H
#define AR_ACTION_ENVIRONMENT_EXPOSURE_H
#include "action_effects.h"
#include "actraiser_game.h"
#include "render/render_types.h"
static inline float ActionEnvironment_NativeBg1Dimming(const ActionSceneEffectFrame *frame, unsigned group, unsigned room) {
  if (!frame ||
      frame->decoration_overflow ||
      frame->decoration_count > kActionSceneDecorationMaxInstances)
    return 0;
  for (unsigned i = 0; i < frame->decoration_count; i++) {
    const ActionEffectInstance *effect = &frame->decorations[i];
    if (group == kActRaiserMapGroup_Fillmore &&
        (room == 2 || room == 3) &&
        effect->kind == kActionEffect_CaveAmbientLight &&
        effect->environment_room == room &&
        effect->phase == kActionEffectPhase_CaveEnvironment &&
        effect->render_layer == kActionEffectRenderLayer_ForegroundLight &&
        effect->projection_plane == kActionEffectProjectionPlane_Bg1 &&
        (effect->flags & kActionEffectFlag_Visible)) return .45f;
    if (group == kActRaiserMapGroup_Bloodpool &&
        effect->environment_room == room &&
        (effect->environment_room == 3 || effect->environment_room == 4 || effect->environment_room == 5 ||
         effect->environment_room == 7 || effect->environment_room == 8) &&
        effect->kind == kActionEffect_CastleLight &&
        effect->phase == kActionEffectPhase_CastleEnvironment &&
        effect->render_layer == kActionEffectRenderLayer_Bg1Plane &&
        effect->projection_plane == kActionEffectProjectionPlane_Bg1 &&
        effect->source_mask && (effect->flags & kActionEffectFlag_Visible))
      return effect->environment_room == 5 ? .42f : effect->environment_room == 8 ? .30f : .36f;
  }
  return 0;
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

static inline ArRenderRectF ActionEnvironment_Bg1DimmingRamp(unsigned group, unsigned room) {
  if (group == kActRaiserMapGroup_Fillmore &&
      room == 2)
    return (ArRenderRectF){800,640,320,448};
  return (ArRenderRectF){0};
}


#endif
