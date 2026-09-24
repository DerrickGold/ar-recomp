#include "regional_placements.h"

typedef struct PlacementValue {
  uint8_t kind, x, y, parameter, type, retry_x, retry_y, reserve;
} PlacementValue;
typedef struct PlacementSlot { uint16_t variant[4]; } PlacementSlot;
typedef struct PlacementRoom { uint16_t scene, first, count; } PlacementRoom;
#include "regional_placements_data.inc"

static unsigned Variant(ArRegionalSource source, bool action_mode) {
  return source == kArRegionalSource_Europe && action_mode ? 3 : (unsigned)source;
}
static bool Pickup(const PlacementValue *row) {
  return row->kind == kActionPlacement_Object && row->type == 0x80;
}
bool ArRegionalPlacements_Copy(const ArRegionalPlacementPolicy *policy,
    uint16_t scene, bool action_mode, ArRegionalDifficulty difficulty,
    ActionPlacementProgram *out) {
  if (!ArRegionalPlacements_Valid(policy) || !out ||
      (unsigned)difficulty >= kArRegionalDifficulty_Count) return false;
  const PlacementRoom *room = NULL;
  for (size_t i = 0; i < sizeof(kRooms)/sizeof(kRooms[0]); ++i)
    if (kRooms[i].scene == scene) { room = &kRooms[i]; break; }
  if (!room) return false;
  const unsigned enemies = Variant(policy->enemies, action_mode);
  const unsigned pickups = Variant(policy->pickups, action_mode);
  ActionPlacementProgram next = {0};
  for (unsigned i = 0; i < room->count; ++i) {
    const PlacementSlot *slot = &kSlots[room->first+i];
    const PlacementValue *enemy = &kValues[slot->variant[enemies]];
    const PlacementValue *item = &kValues[slot->variant[pickups]];
    const PlacementValue *row = slot->variant[enemies] && !Pickup(enemy) ? enemy :
        slot->variant[pickups] && Pickup(item) ? item : NULL;
    if (!row) continue;
    uint8_t parameter = row->parameter;
    if (enemies >= 2 && row->kind == kActionPlacement_Object && !(row->type & 0x80) &&
        (parameter == 1 || parameter == 2)) {
      if (difficulty == kArRegionalDifficulty_Beginner ||
          (parameter == 2 && difficulty != kArRegionalDifficulty_Expert)) continue;
      parameter = 0;
    }
    if (next.count == kActionPlacementCapacity) return false;
    next.rows[next.count++] = (ActionPlacement){(uint16_t)(i+1), row->kind,
        row->x, row->y, parameter, row->type, row->retry_x, row->retry_y, row->reserve};
  }
  if (!ActionPlacements_Validate(&next)) return false;
  *out = next;
  return true;
}
