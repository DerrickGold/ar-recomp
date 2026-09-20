#ifndef ACTRAISER_ROOM_PROFILES_H
#define ACTRAISER_ROOM_PROFILES_H

#include "actraiser_game.h"

/* Shared room identities for scene decoding, effects and presentation. These
 * identify a room family, not whether an effect can be drawn: each consumer
 * still checks the captured video profile, geometry or native actor phase.
 * Waterfall: BG2 page animation, flow/mist detection and Diorama continuation.
 * Act 2 lava: lake detection and the presentation heat pass. Heat belongs to
 * the room even when no complete reservoir signature is visible on screen. */
typedef enum ActRaiserRoomProfile {
  kActRaiserRoomProfile_Default,
  kActRaiserRoomProfile_AitosWaterfall,
  kActRaiserRoomProfile_AitosAct2Lava,
} ActRaiserRoomProfile;

static inline ActRaiserRoomProfile ActRaiserRoom_ProfileFor(
    uint8_t map_group, uint8_t map_number) {
  if (map_group == kActRaiserMapGroup_Aitos) {
    if (map_number >= 0x02 && map_number <= 0x03)
      return kActRaiserRoomProfile_AitosWaterfall;
    if (map_number >= 0x04 && map_number <= 0x06)
      return kActRaiserRoomProfile_AitosAct2Lava;
  }
  return kActRaiserRoomProfile_Default;
}

#endif
