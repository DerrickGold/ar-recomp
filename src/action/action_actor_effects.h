#ifndef AR_ACTION_ACTOR_EFFECTS_H
#define AR_ACTION_ACTOR_EFFECTS_H
#include <stdint.h>
#include <stdbool.h>
/* Presentation facts, captured once on the game thread. Selectors use retained
 * family identity and optional animation filters, never mutable pool slots. */
enum {kActionEffectActorMax=80,kActionActorBindingMaxInstances=16,kActionActorWakeTicks=90};
/* One short, world-space air disturbance per body. No gameplay allocation or
 * renderer history: captured frames own this cosmetic state. */
typedef struct ActionActorWake {
  float x, y, dx, strength;
  uint8_t age;
} ActionActorWake;
typedef struct ActionEffectActor {
  uint32_t generation;
  uint16_t source, parent_source, animation, state, visual, handler, resume;
  uint16_t age, phase_ticks, address;
  int16_t x,y,vx,vy;
  uint8_t bank, priority, visible, flip, player;
  int16_t feet_y;
  uint8_t body_radius;
  ActionActorWake wake;
} ActionEffectActor;
void ActionEffectActor_UpdateWake(ActionEffectActor *,const ActionEffectActor *previous,
                                  unsigned elapsed_ticks);
typedef struct ActionEffectActorTrack {
  ActionEffectActor actor;
  /* Provenance is sampled once per actor generation, never refreshed from a
   * parent slot that can retire or be reused while the child still exists. */
  uint16_t parent_address;
  uint8_t active;
} ActionEffectActorTrack;
typedef struct ActionEffectActorSelector {
  uint16_t source, parent, state_first, state_last, visual_first, visual_last;
  uint16_t animation, handler, resume;
  uint8_t target, limit; /* 0=placed, 1=family, 2=player */
  uint16_t fields;
} ActionEffectActorSelector;
bool ActionEffectActorSelector_Set(ActionEffectActorSelector *,const char *,const char *);
bool ActionEffectActorSelector_Valid(const ActionEffectActorSelector *);
bool ActionEffectActorSelector_Matches(const ActionEffectActorSelector *,const ActionEffectActor *);
#endif
