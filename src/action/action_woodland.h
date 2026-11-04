#ifndef AR_ACTION_WOODLAND_H
#define AR_ACTION_WOODLAND_H
#include "action_field_motion.h"
typedef struct ActionFlowerMotion {float phase,wind;ActionFieldWakes wakes;} ActionFlowerMotion;
typedef struct ActionFlowerPose {
  uint32_t seed,color;
  unsigned species;
  float x,root,back,height,tip_x,tip_y,floor_x0,floor_x1;
} ActionFlowerPose;
typedef struct ActionButterflyPose {float x,y,flight;uint32_t seed;} ActionButterflyPose;
ActionFlowerMotion ActionWoodland_FlowerMotion(const ActionSceneEffectFrame *,const ActionEffectInstance *);
bool ActionWoodland_Flower(const ActionEffectInstance *,const ActionEffectFloorField *,unsigned,
    const ActionFlowerMotion *,ActionFlowerPose *);
ActionButterflyPose ActionWoodland_Butterfly(const ActionEffectInstance *,unsigned,
    const ActionSceneEffectFrame *);
const ActionWoodlandEvent *ActionWoodland_Event(const ActionSceneEffectFrame *,const ActionEffectInstance *,unsigned);
void ActionWoodland_Capture(ActionWoodlandObserver *,ActionSceneEffectFrame *,unsigned group,
    unsigned room,unsigned terrain,unsigned clock,bool enabled);
#endif
