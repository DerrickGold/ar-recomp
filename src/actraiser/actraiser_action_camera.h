#ifndef AR_ACTRAISER_ACTION_CAMERA_H
#define AR_ACTRAISER_ACTION_CAMERA_H
#include <stdbool.h>
#include <stdint.h>

/* Last resolved horizontal travel, valid only for the current room and native
 * width. Read at the game-owner boundary when capturing presentation state. */
bool ActRaiser_ActionCameraTravel(uint16_t *minimum, uint16_t *maximum);
#endif
