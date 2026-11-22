#ifndef AR_SIM_BACKGROUND_VOXEL_MODEL_KEY_H
#define AR_SIM_BACKGROUND_VOXEL_MODEL_KEY_H

#include "deterministic_hash.h"
#include "sim/voxels/sim_background_voxel_models.h"
#include "sim/voxels/sim_background_voxel_region.h"

/* Geometry identity is shared by visible meshes and conservative bounds. */
typedef struct SimBackgroundVoxelModelKey {
  uint16_t group;
  uint8_t kind, flags;
  uint8_t town, development_level;
  uint8_t house_style, rock_metatile;
  uint8_t cell_x, cell_y;
  uint8_t tree_edges, record_slot;
  uint8_t source_cells_w, source_cells_h;
  uint8_t bridge_axis;
  uint8_t bridge_bank_a_x, bridge_bank_a_y;
  uint8_t bridge_bank_b_x, bridge_bank_b_y;
  uint8_t detail, style;
  /* Animated families compile one model per frame. Leaving this out of the
   * key froze a windmill on whichever blade position compiled first. */
  uint8_t animation_phase, visual_state;
} SimBackgroundVoxelModelKey;

static inline SimBackgroundVoxelModelKey SimBackgroundVoxelModelKey_Make(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style) {
  /* Basic house meshes are independent of placement. Keep palette/shading
   * inputs and native family in the key while sharing identical geometry. */
  bool shared_house = object->kind == kSimBackgroundVoxel_House &&
      style == kSimBackgroundVoxelStyle_Basic;
  return (SimBackgroundVoxelModelKey){
    .group = shared_house ? 0 : object->group,
    .kind = object->kind,
    .flags = object->flags,
    .town = object->town,
    .development_level = object->development_level,
    .rock_metatile = object->kind == kSimBackgroundVoxel_Rocks
        ? object->visual_metatile : 0,
    .house_style = object->kind == kSimBackgroundVoxel_House
        ? (uint8_t)SimBackgroundVoxelRegion_ObjectHouseStyle(object) : 0,
    .cell_x = shared_house ? 0 : object->cell_x,
    .cell_y = shared_house ? 0 : object->cell_y,
    .tree_edges = object->tree_edges,
    .record_slot = shared_house ? 0 : object->record_slot,
    .source_cells_w = object->source_cells_w,
    .source_cells_h = object->source_cells_h,
    .bridge_axis = object->bridge_axis,
    .bridge_bank_a_x = object->bridge_bank_a_x,
    .bridge_bank_a_y = object->bridge_bank_a_y,
    .bridge_bank_b_x = object->bridge_bank_b_x,
    .bridge_bank_b_y = object->bridge_bank_b_y,
    .detail = (uint8_t)detail,
    .style = (uint8_t)style,
    .animation_phase = object->animation_phase,
    .visual_state = object->visual_state,
  };
}

static inline bool SimBackgroundVoxelModelKey_Equals(const SimBackgroundVoxelModelKey *left,
                      const SimBackgroundVoxelModelKey *right) {
  return left->group == right->group && left->kind == right->kind &&
      left->flags == right->flags && left->cell_x == right->cell_x &&
      left->town == right->town &&
      left->development_level == right->development_level &&
      left->house_style == right->house_style &&
      left->rock_metatile == right->rock_metatile &&
      left->cell_y == right->cell_y && left->tree_edges == right->tree_edges &&
      left->record_slot == right->record_slot && left->detail == right->detail &&
      left->source_cells_w == right->source_cells_w &&
      left->source_cells_h == right->source_cells_h &&
      left->bridge_axis == right->bridge_axis &&
      left->bridge_bank_a_x == right->bridge_bank_a_x &&
      left->bridge_bank_a_y == right->bridge_bank_a_y &&
      left->bridge_bank_b_x == right->bridge_bank_b_x &&
      left->bridge_bank_b_y == right->bridge_bank_b_y &&
      left->style == right->style &&
      left->animation_phase == right->animation_phase &&
      left->visual_state == right->visual_state;
}

static inline uint32_t SimBackgroundVoxelModelKey_Hash(const SimBackgroundVoxelModelKey *key) {
  /* FNV-1a over explicit fields avoids hashing struct padding, whose bytes are
  * unspecified and can differ between compilers and architectures. */
  uint32_t hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  hash = DeterministicHash_Fnv1a32Byte(hash, (uint8_t)key->group);
  hash = DeterministicHash_Fnv1a32Byte(
      hash, (uint8_t)(key->group >> 8));
  hash = DeterministicHash_Fnv1a32Byte(hash, key->kind);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->flags);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->town);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->development_level);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->house_style);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->rock_metatile);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->cell_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->cell_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->tree_edges);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->record_slot);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->source_cells_w);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->source_cells_h);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_axis);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_a_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_a_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_b_x);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->bridge_bank_b_y);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->detail);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->style);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->animation_phase);
  hash = DeterministicHash_Fnv1a32Byte(hash, key->visual_state);
  /* The set count is a power of two, so avalanche the structured coordinates
   * before selecting its low bits. This keeps adjacent town cells and the new
   * regional identity bytes from clustering into a handful of four-way sets. */
  return DeterministicHash_Mix32(hash);
}

#endif
