#ifndef ACTRAISER_ACTION_ROOM_MOSAIC_H
#define ACTRAISER_ACTION_ROOM_MOSAIC_H
#include "action_room_scene.h"

/* Three normalized, measured patterns, not foreign executable bytes. The
 * native 64 phases consume 112 bands each: indices 0 through 174 inclusive.
 * Pattern 0 is US/German, 1 Japanese, 2 European English/French. */
enum { kActionRoomMosaicWindow = 175, kActionRoomMosaicPatterns = 3 };
bool ActionRoomMosaic_Bit(uint8_t pattern,unsigned index,uint8_t *out);
/* Room-load projection only; unrelated effects and the entry frame are left
 * alone. The frame composer and native HDMA keep their existing timing. */
bool ActionRoomMosaic_Project(ActionRoomScene *scene,uint8_t pattern);
#endif
