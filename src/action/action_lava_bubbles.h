#ifndef AR_ACTION_LAVA_BUBBLES_H
#define AR_ACTION_LAVA_BUBBLES_H
/* Native lava mound animation, in presentation space only. No state advances
 * on lookup: camera movement, repeated captures and reverse seeking agree. */
#include "action_room_scene.h"
#include "action_surface_field.h"
#include "deterministic_hash.h"
#include <string.h>

typedef struct ActionLavaBubbles {
  const uint8_t *characters; /* Immutable original animation, never live VRAM. */
  uint32_t seed;
  uint16_t clock, pause_min, pause_max;
  uint8_t cadence, room;
  float chance;
} ActionLavaBubbles;

/* Both character selection and captured reactions use this timing setup.
 * Reactions need no borrowed art; Prepare additionally validates the CHR layout. */
static inline bool ActionLavaBubbles_Configure(ActionLavaBubbles *out,
    const ActionSurfaceField *field, uint8_t group, uint8_t room, uint16_t clock) {
  *out=(ActionLavaBubbles){0};
  if (!field || group!=4 || room<4 || room>6 ||
      field->Kind[0]!=1 || field->NativeBubbles[0]!=1) return false;
  *out=(ActionLavaBubbles){.clock=clock,
      .chance=field->NativeBubbles[1],.pause_min=(uint16_t)field->NativeBubbles[2],
      .pause_max=(uint16_t)field->NativeBubbles[3],.cadence=(uint8_t)field->NativeBubbles[4],
      .seed=(uint32_t)field->NativeBubbles[5],.room=room};
  return true;
}
static inline void ActionLavaBubbles_Prepare(ActionLavaBubbles *out,
    const ActionRoomScene *scene, const ActionSurfaceField *field, uint16_t clock) {
  *out=(ActionLavaBubbles){0};
  if (!scene || ActionRoomScene_CharacterAnimationPhaseCount(scene)!=4 ||
      ActionRoomScene_CharacterAnimationStride(scene)!=256 ||
      ActionRoomScene_CharacterAnimationTarget(scene)!=0) return;
  if (ActionLavaBubbles_Configure(out,field,scene->group,scene->map,clock))
    out->characters=scene->characters;
}

/* One complete 16x16 mound owns one seed. Pauses vary between consecutive
 * bursts as well as between sites; skipped cycles create changing gaps. */
static inline int ActionLavaBubbles_Age(const ActionLavaBubbles *s, int cell_x, int cell_y) {
  const uint32_t seed=DeterministicHash_Mix32(s->seed ^ (uint32_t)cell_x*0x9e3779b9u ^
      (uint32_t)cell_y*0x85ebca6bu ^ (uint32_t)s->room*0xc2b2ae35u);
  const unsigned range=s->pause_max-s->pause_min, margin=range/4;
  const unsigned quiet=s->pause_min+margin+(seed% (range-2*margin+1));
  const unsigned left=quiet-s->pause_min,right=s->pause_max-quiet;
  const unsigned jitter=(left<right?left:right)/2;
  const unsigned active=4u*s->cadence, period=quiet+active;
  const uint32_t time=(uint32_t)s->clock+DeterministicHash_Mix32(seed^17u)%period;
  const uint32_t cycle=DeterministicHash_Mix32(seed^(time/period)*0x27d4eb2du);
  if ((float)(cycle>>8)*(1.f/16777216.f)>=s->chance) return -1;
  const unsigned onset=DeterministicHash_Mix32(cycle^53u)%(2*jitter+1);
  const int age=(int)(time%period)-(int)onset;
  return age>=0 && (unsigned)age<active ? age : -1;
}
static inline int ActionLavaBubbles_Phase(const ActionLavaBubbles *s, int cell_x, int cell_y) {
  const int age=ActionLavaBubbles_Age(s,cell_x,cell_y);
  return age<0 ? -1 : age/s->cadence;
}

/* Only the original decorative mound (02), not the flowing lip (01) or the
 * stepping-platform art (03/04). The caller keeps palette, flips and depth. */
static inline bool ActionLavaBubbles_Character(const ActionLavaBubbles *s,
    unsigned metatile, uint16_t entry, int tile_x, int tile_y, uint8_t character[32]) {
  if (!s || !s->characters || metatile!=2) return false;
  const unsigned tile=(tile_y&1)*4+(tile_x&1);
  if ((entry&1023u)!=tile) return false;
  const int phase=ActionLavaBubbles_Phase(s,tile_x/2,tile_y/2);
  if (phase<0) memset(character,0,32);
  else memcpy(character,s->characters+(unsigned)phase*256+tile*32,32);
  return true;
}
#endif
