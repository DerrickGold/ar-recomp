#ifndef AR_REGIONAL_PLACEMENTS_H
#define AR_REGIONAL_PLACEMENTS_H

#include "regional/action/regional_placement_policy.h"
#include "regional/action/regional_difficulty.h"
#include "action/action_placements.h"

/* Enemy/wave placement and pickups remain separately selectable. Difficulty
 * markers belong only to European authored enemy rows; ordinary US/JP params
 * and universal pickup IDs must never be interpreted as markers. */
/* No allocation or I/O. Unknown scenes/policies leave output unchanged.
 * Filtering and parameter normalization happen before randomization/spawn.
 * The caller owns the room/retry snapshot; never rebuild a live actor pool
 * merely because the requested settings changed. */
bool ArRegionalPlacements_Copy(const ArRegionalPlacementPolicy *policy,
    uint16_t scene, bool action_mode, ArRegionalDifficulty difficulty,
    ActionPlacementProgram *out);

#endif
