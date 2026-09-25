#ifndef ACTRAISER_ACTION_PLACEMENTS_H
#define ACTRAISER_ACTION_PLACEMENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { kActionPlacementCapacity = 128 };
typedef enum ActionPlacementKind {
  kActionPlacement_Object,
  kActionPlacement_Reserve,
  kActionPlacement_Wave,
  kActionPlacement_End
} ActionPlacementKind;

/* Values, not foreign ROM pointers. Object coordinates use 16-pixel cells;
 * waves use trigger and retry coordinates. Reservation counts are explicit.
 * The room owner supplies native type initialization and bounded actor slots.
 * A stable ID identifies a row within a room, not a live actor incarnation. */
typedef struct ActionPlacement {
  uint16_t id;
  uint8_t kind, x, y, parameter, type, retry_x, retry_y, reserve;
} ActionPlacement;
typedef struct ActionPlacementProgram {
  size_t count;
  ActionPlacement rows[kActionPlacementCapacity];
} ActionPlacementProgram;

/* Validate structure and the format's capacity ceiling, not live free space.
 * The room owner must preflight its entry-specific pool, including a sentinel,
 * before mutating actors. One wave at most; the native later-wave loader accepts
 * only objects and the final terminator. */
bool ActionPlacements_Validate(const ActionPlacementProgram *program);

#endif
