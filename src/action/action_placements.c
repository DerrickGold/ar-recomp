#include "action_placements.h"

bool ActionPlacements_Validate(const ActionPlacementProgram *program) {
  if (!program || !program->count || program->count > kActionPlacementCapacity)
    return false;
  /* 80 native slots: eight magic slots, player, eight reserved slots and an
   * end sentinel. The later wave reuses the pool but retains its controller
   * and player. Conservatively bound it to the same 62 available slots. */
  unsigned slots = 0, waves = 0;
  for (size_t i = 0; i < program->count; ++i) {
    const ActionPlacement *row = &program->rows[i];
    if (!row->id) return false;
    for (size_t j = 0; j < i; ++j)
      if (program->rows[j].id == row->id) return false;
    switch (row->kind) {
      case kActionPlacement_Object:
        if (row->x >= 0xfc) return false;
        ++slots;
        break;
      case kActionPlacement_Reserve:
        if (waves || !row->reserve) return false;
        slots += row->reserve;
        break;
      case kActionPlacement_Wave:
        if (++waves > 1 || ++slots > 62) return false;
        slots = 0;
        break;
      case kActionPlacement_End:
        return i + 1 == program->count && slots <= 62;
      default:
        return false;
    }
    if (slots > 62) return false;
  }
  return false;
}
