#ifndef AR_DIORAMA_OBJECT_SNAPSHOT_H
#define AR_DIORAMA_OBJECT_SNAPSHOT_H
/* Portable, finite native object capture. Raw winners and immutable ownership
 * attributes remain paired; modeled props are handed off only by the consumer.
 * This transports presentation facts, never actor simulation or live pointers. */
#include "action/action_actor_effects.h"
#include "action/action_voxel_sprites.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kDioramaObjectSnapshotVersion = 3,
  kDioramaObjectSnapshotWidth = 640,
  kDioramaObjectSnapshotHeight = 352,
  kDioramaObjectSnapshotHeaderBytes = 128,
  kDioramaObjectSnapshotCapacity = 8 * 1024 * 1024,
};

typedef struct DioramaObjectSnapshotView {
  uint8_t map_group, map_number, terrain_profile, layer_section;
  uint32_t game_frame;
  int32_t world_x, world_y, camera_x, camera_y;
  uint16_t extra_x, extra_top, obj_apron, width, height;
  bool owner_tags;
  uint8_t additive_priority_mask;
  /* Host-endian ARGB words; all pitches are in uint32_t pixels, not bytes.
   * NULL means an entirely zero plane; otherwise pitch must cover width. */
  const uint32_t *obj[4], *attributes[4];
  size_t obj_pitch[4], attribute_pitch[4];
  ActionVoxelSpriteFrame voxel_sprites;
  /* Bounded native actor facts feed the existing lighting/attached recipes. */
  unsigned actor_count;
  ActionEffectActor actors[kActionEffectActorMax];
  /* Atlas width is264. Input height may be the occupied cell rows or136;
   * decoding always owns a complete264x136 atlas when props are present. */
  const uint32_t *atlas;
  size_t atlas_pitch;
  unsigned atlas_height;
} DioramaObjectSnapshotView;

typedef struct DioramaObjectSnapshot {
  DioramaObjectSnapshotView view;
  uint32_t *storage; /* Owned by this instance; do not free or shallow-copy it. */
} DioramaObjectSnapshot;

/* Sparse per-row runs preserve every32-bit value exactly. All integer and
 * binary32 fields are explicitly little endian, never a raw structure dump.
 * On failure packet is unchanged and *size is zero. */
bool DioramaObjectSnapshot_Encode(const DioramaObjectSnapshotView *view,
    uint8_t *packet, size_t capacity, size_t *size);
/* Zero-initialize out before first use. Successful decode replaces its old
 * owned storage; any failure leaves out untouched. Packet may be released
 * immediately afterward. Maximum decoded pixel storage is below7.5MiB. */
bool DioramaObjectSnapshot_Decode(const uint8_t *packet, size_t size,
    DioramaObjectSnapshot *out);
void DioramaObjectSnapshot_Release(DioramaObjectSnapshot *snapshot);

#endif
