#ifndef AR_ACTION_SCENE_SNAPSHOT_H
#define AR_ACTION_SCENE_SNAPSHOT_H

#include "action_room_scene.h"

/* Portable stable-room comparison fixture. This is deliberately not a saved
 * FrameSlot or emulator state: no pointers, padding, GPU handles or actors.
 * Version 1 stores original background assets and explicit raster/time inputs.
 * Edited surfaces, environmental sources and the Diorama compositor are later
 * snapshot capabilities, not implied by successful baseline replay. */
enum {
  kActionSceneSnapshotVersion = 1,
  kActionSceneSnapshotHeaderBytes = 112,
  kActionSceneSnapshotMaxBytes = 80 * 1024,
};

typedef struct ActionSceneSnapshot {
  ActionRoomScene scene;
  ActionRoomSceneFrameRequest frame;
  uint32_t terrain_profile;
} ActionSceneSnapshot;

/* Little-endian fixed-width header and byte assets, checksum over all bytes
 * except the checksum word. Invalid input leaves the destination untouched.
 * Source and destination storage must not overlap. No allocation or IO. */
bool ActionSceneSnapshot_Encode(const ActionSceneSnapshot *snapshot,
                                uint8_t *bytes, size_t capacity, size_t *size);
bool ActionSceneSnapshot_Decode(const uint8_t *bytes, size_t size,
                                ActionSceneSnapshot *snapshot);

#endif
