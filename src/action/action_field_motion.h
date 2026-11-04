#ifndef AR_ACTION_FIELD_MOTION_H
#define AR_ACTION_FIELD_MOTION_H
#include "action_effects.h"
/* Captured world-space motion shared by the renderer and reactive emitters. */
typedef struct ActionFieldWake {float x,y,dx,radius,strength,age;uint32_t seed;} ActionFieldWake;
typedef struct ActionFieldWakes {ActionFieldWake bodies[kActionFieldWakeCount];} ActionFieldWakes;
float ActionFieldMotion_Wind(const ActionEffectInstance *);
float ActionFieldMotion_GroundHeight(const ActionEffectFieldStyle *,float);
ActionFieldWakes ActionFieldMotion_Gather(const ActionSceneEffectFrame *,const ActionEffectInstance *,
    const ActionEffectLocalRect *,bool ground);
#endif
