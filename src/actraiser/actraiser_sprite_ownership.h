#ifndef ACTRAISER_SPRITE_OWNERSHIP_H
#define ACTRAISER_SPRITE_OWNERSHIP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ActRaiserSpriteRole {
  kActRaiserSprite_Unowned,
  kActRaiserSprite_HudIcon,
  kActRaiserSprite_StatueEyes,
  kActRaiserSprite_WorldLabel,
  kActRaiserSprite_WorldPlaque,
  kActRaiserSprite_WorldPalace,
} ActRaiserSpriteRole;

enum { kActRaiserSpriteSlots = 128, kActRaiserSpriteShadowBytes = 544 };
typedef struct ActRaiserSpriteOwnership {
  bool valid, unowned_emitted;
  uint8_t group, map;
  uint16_t location;
  uint8_t slots[kActRaiserSpriteSlots];
} ActRaiserSpriteOwnership;

/* Emission describes identity; the completed native OAM DMA makes it visible.
 * These facts are independent of regional art selection and donor residency. */
void ActRaiserSpriteOwnership_Reset(void);
void ActRaiserSpriteOwnership_Begin(uint8_t group, uint8_t map,
                                   uint16_t location);
void ActRaiserSpriteOwnership_Record(ActRaiserSpriteRole role,
                                    unsigned first_byte, unsigned end_byte);
void ActRaiserSpriteOwnership_RecordAction(uint16_t source,
    unsigned first_byte, unsigned end_byte);
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
