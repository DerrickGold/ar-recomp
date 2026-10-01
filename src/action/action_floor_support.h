#ifndef AR_ACTION_FLOOR_SUPPORT_H
#define AR_ACTION_FLOOR_SUPPORT_H
#include "action_environment_scene.h"
/* Original gameplay collision, not painted scenery. Low nibble is the four
 * collision quadrants; bit 4 marks an exposed floor supported by this resolver. */
unsigned ActionFloorSupport_Cell(const ActionEnvironmentScene *scene, int x, int y);
void ActionFloorSupport_Resolve(const ActionEnvironmentScene *scene,
    const ActionEffectLocalRect *world_area, float height, ActionEffectFloorField *out);
#endif
