#ifndef AR_ACTION_WOODLAND_STATE_H
#define AR_ACTION_WOODLAND_STATE_H
#include <stdbool.h>
#include <stdint.h>
enum {kActionWoodlandMaxEvents=16,kActionFlowerPetals=2,
      kActionWoodlandFlightTicks=150,kActionWoodlandPetalTicks=108,
      kActionWoodlandCooldownTicks=360};
/* Bounded cosmetic events, owned by completed game ticks and copied into the
 * frame. Retired visible events remain until cooldown, preventing repeated
 * bursts from overlapping enemies or a player pacing through the same bed. */
typedef struct ActionWoodlandEvent {
  uint32_t source, definition_seed, seed, color;
  float x,y,end_x,ground_y,direction;
  int16_t owner_x,owner_y;
  uint16_t born;
  uint8_t kind,index;
  bool valid;
} ActionWoodlandEvent;
typedef struct ActionWoodlandObserver {
  ActionWoodlandEvent events[kActionWoodlandMaxEvents];
  uint16_t clock;
  uint8_t group,room,terrain;
  bool valid;
} ActionWoodlandObserver;
#endif
