#ifndef AR_ACTRAISER_SPRITE_OWNERSHIP_H
#define AR_ACTRAISER_SPRITE_OWNERSHIP_H
/* ActRaiserSpriteOwnership: records which game object emitted each OAM range
 * (action and SIM), visible once the native OAM DMA completes, so presenters
 * can find one object's sprites.
 * Phase: capture.
 * Tests: tests/actraiser_sprite_ownership_test.c */

#include <stdbool.h>
#include <stdint.h>

typedef enum ActRaiserSpriteRole {
  kActRaiserSprite_Unowned,
  kActRaiserSprite_HudIcon,
  kActRaiserSprite_StatueEyes,
  kActRaiserSprite_WorldLabel,
  kActRaiserSprite_WorldPlaque,
  kActRaiserSprite_WorldPalace,
  kActRaiserSprite_Player,
  /* Non-player action objects, including enemies, pickups and scenery props.
   * Their receiver membership must not depend on transient combat flags. */
  kActRaiserSprite_Enemy,
  kActRaiserSprite_ChurchVisitor,
} ActRaiserSpriteRole;

enum { kActRaiserSpriteSlots = 128, kActRaiserSpriteShadowBytes = 544 };
enum { kActRaiserWallTorchPartLimit = 16 };
typedef struct ActRaiserWallTorchPart {
  uint16_t owner, tile_attr;
  int16_t x, y, origin_x, origin_y, world_x, world_y;
  uint8_t size, owner_tag;
} ActRaiserWallTorchPart;
typedef struct ActRaiserSpriteOwnership {
  bool valid, unowned_emitted;
  uint8_t group, map;
  uint16_t location;
  uint8_t slots[kActRaiserSpriteSlots];
  int16_t world_x[kActRaiserSpriteSlots], world_y[kActRaiserSpriteSlots];
  /* Verified visual props, paired with the exact completed OAM build. */
  uint16_t prop_owner[kActRaiserSpriteSlots];
  uint8_t prop_shape[kActRaiserSpriteSlots];
  /* All non-player action compositions are eligible for explicit visual
   * authoring. These immutable facts alone do not activate modeling. */
  uint16_t visual_owner[kActRaiserSpriteSlots], visual_source[kActRaiserSpriteSlots];
  uint16_t visual_animation[kActRaiserSpriteSlots];
  /* Native composition origin before viewport clipping, from the same OAM
   * emission. Large props keep stable sections as their visible parts change. */
  int16_t prop_origin_x[kActRaiserSpriteSlots], prop_origin_y[kActRaiserSpriteSlots];
  /* Exact native apron parts from the same completed build. These can exist
   * without an OAM slot; recording them must never widen native OAM. */
  uint8_t wall_torch_part_count;
  ActRaiserWallTorchPart wall_torch_parts[kActRaiserWallTorchPartLimit];
} ActRaiserSpriteOwnership;

static inline unsigned ActRaiserSpriteOwnership_VisualTag(
    const ActRaiserSpriteOwnership *ownership,uint16_t owner) {
  if(ownership && ownership->valid && owner)
    for(unsigned i=0;i<kActRaiserSpriteSlots;++i)
      if(ownership->visual_owner[i]==owner)return i+1;
  return 0;
}

/* A transient capture tag, independent of color, tile identity and object
 * address layout. All parts of one verified prop share its first OAM slot.
 * Zero means no verified owner; 255 remains ordinary receiver coverage. */
static inline unsigned ActRaiserSpriteOwnership_PropTag(
    const ActRaiserSpriteOwnership *ownership, uint16_t owner) {
  if (ownership && ownership->valid && owner) {
    for (unsigned i=0;i<kActRaiserSpriteSlots;++i)
      if (ownership->prop_owner[i]==owner) return i+1;
    for (unsigned i=0;i<ownership->wall_torch_part_count;++i)
      if (ownership->wall_torch_parts[i].owner==owner)
        return ownership->wall_torch_parts[i].owner_tag;
  }
  return 0;
}

/* Emission describes identity; the completed native OAM DMA makes it visible.
 * These facts are independent of regional art selection and donor residency. */
void ActRaiserSpriteOwnership_Reset(void);
void ActRaiserSpriteOwnership_Begin(uint8_t group, uint8_t map,
                                   uint16_t location);
void ActRaiserSpriteOwnership_Record(ActRaiserSpriteRole role,
                                    unsigned first_byte, unsigned end_byte);
void ActRaiserSpriteOwnership_RecordAction(uint16_t source,
    unsigned first_byte, unsigned end_byte);
ActRaiserSpriteRole ActRaiserSpriteOwnership_ActionRole(uint16_t record);
void ActRaiserSpriteOwnership_RecordActionObject(uint16_t source,uint16_t record,
    int16_t world_x,int16_t world_y,unsigned first_byte,unsigned end_byte);
enum {
  kActRaiserSpriteProp_None, kActRaiserSpriteProp_Log, kActRaiserSpriteProp_RockPlatform,
  kActRaiserSpriteProp_TempleBridge, kActRaiserSpriteProp_TemplePlatform,
  kActRaiserSpriteProp_TempleCrusher, kActRaiserSpriteProp_TempleSpikes,
  kActRaiserSpriteProp_TempleSpikeRail,
  /* Native animated flame attached to the room's rear wall, never extruded. */
  kActRaiserSpriteProp_WallTorch,
  kActRaiserSpriteProp_Generic,
  kActRaiserSpriteProp_IcePlatform,
};
void ActRaiserSpriteOwnership_RecordActionVisual(uint16_t source,uint16_t record,
    uint16_t animation,uint8_t bank,uint16_t composition,
    int16_t origin_x,int16_t origin_y,
    unsigned first_byte,unsigned end_byte);
void ActRaiserSpriteOwnership_RecordWallTorchPart(uint16_t source,uint16_t record,
    uint16_t animation,uint8_t bank,int16_t origin_x,int16_t origin_y,
    int16_t world_x,int16_t world_y,int16_t x,int16_t y,uint16_t tile_attr,uint8_t size);
void ActRaiserSpriteOwnership_RecordSim(uint16_t record, uint16_t family,
    unsigned first_byte, unsigned end_byte);
void ActRaiserSpriteOwnership_Complete(const uint8_t *shadow);
void ActRaiserSpriteOwnership_Upload(uint8_t group, uint8_t map,
                                    const uint8_t *shadow);
ActRaiserSpriteOwnership ActRaiserSpriteOwnership_Presented(
    uint8_t group, uint8_t map);
/* A consumer requiring one range must not accidentally include another owner
 * between two pieces. Disjoint ranges fail closed. */
bool ActRaiserSpriteOwnership_Range(const ActRaiserSpriteOwnership *ownership,
    ActRaiserSpriteRole role, uint8_t *first, uint8_t *count);

#endif
