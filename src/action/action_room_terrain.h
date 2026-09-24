#ifndef ACTION_ROOM_TERRAIN_H
#define ACTION_ROOM_TERRAIN_H
#include "action_room_scene.h"
/* Project a loaded US scene's BG1 through a room-pinned regional value.
 * Base/reference scene loading is deliberately independent of game settings. */
bool ActionRoomTerrain_Project(ActionRoomScene *scene, uint8_t profile);
#endif
