#include "actraiser_sprite_ownership.h"

#include <string.h>
#include "actraiser_game.h"

static ActRaiserSpriteOwnership s_build, s_staged, s_presented;
static uint8_t s_shadow[kActRaiserSpriteShadowBytes];

void ActRaiserSpriteOwnership_Reset(void) {
  s_build = s_staged = s_presented = (ActRaiserSpriteOwnership){0};
}

void ActRaiserSpriteOwnership_Begin(uint8_t group, uint8_t map,
                                   uint16_t location) {
  s_build = (ActRaiserSpriteOwnership){
    .valid = true, .group = group, .map = map, .location = location,
  };
  s_staged.valid = false;
}

void ActRaiserSpriteOwnership_Record(ActRaiserSpriteRole role,
                                    unsigned first_byte, unsigned end_byte) {
  if (!s_build.valid) return;
  if (first_byte > end_byte || end_byte > 512 ||
      (first_byte & 3) || (end_byte & 3)) {
    s_build.valid = false;
    return;
  }
  if (role == kActRaiserSprite_Unowned && end_byte > first_byte)
    s_build.unowned_emitted = true;
  for (unsigned slot = first_byte / 4; slot < end_byte / 4; ++slot)
    s_build.slots[slot] = (uint8_t)role;
}

void ActRaiserSpriteOwnership_RecordAction(uint16_t source,
    unsigned first_byte, unsigned end_byte) {
  ActRaiserSpriteRole role = kActRaiserSprite_Unowned;
  if (s_build.group == kActRaiserMapGroup_DeathHeim &&
      s_build.map == kActRaiserDeathHeimMap_Hub) {
    /* Native source descriptors in $00:F39A's Death Heim actor table. The
     * emitter may substitute regional composition parts after this identity
     * was assigned; record the actual emitted extent, never a fixed count. */
    switch (source) {
      case 0xf3fa: case 0xf408: case 0xf430: case 0xf458:
      case 0xf486: case 0xf494: case 0xf4b2:
        role = kActRaiserSprite_StatueEyes;
        break;
    }
  }
  ActRaiserSpriteOwnership_Record(role, first_byte, end_byte);
}

void ActRaiserSpriteOwnership_RecordSim(uint16_t record, uint16_t family,
    unsigned first_byte, unsigned end_byte) {
  ActRaiserSpriteRole role = kActRaiserSprite_Unowned;
  const bool fixed = record >= kActRaiserWram_SimFixedRecords &&
      record < kActRaiserWram_SimFixedRecords +
          kActRaiserSimFixedRecordStride * kActRaiserSimFixedRecordCount &&
      (record - kActRaiserWram_SimFixedRecords) %
          kActRaiserSimFixedRecordStride == 0;
  if (fixed && s_build.group == kActRaiserMapGroup_NonAction) {
    /* +$0E is the native $01:A227 spawn family, not an OAM tile or an
     * animation frame. $01:9128 owns the selected-magic record at $083E;
     * other records can use the same spell families inside menus. */
    if (ActRaiser_IsSimulationTown(s_build.group, s_build.map) &&
        record == 0x083e && family >= 2 && family <= 4)
      role = kActRaiserSprite_HudIcon;
    if (s_build.map == kActRaiserNonActionMap_SkyPalace &&
        record == 0x083e && family >= 0x25 && family <= 0x28)
      role = kActRaiserSprite_HudIcon;
    if (s_build.map == kActRaiserNonActionMap_WorldMap) {
      switch (family) {
        case 0x31: role = kActRaiserSprite_WorldPalace; break;
        case 0x32: role = kActRaiserSprite_WorldPlaque; break;
        case 0x33: role = kActRaiserSprite_WorldLabel; break;
      }
    }
  }
  ActRaiserSpriteOwnership_Record(role, first_byte, end_byte);
}

void ActRaiserSpriteOwnership_Complete(const uint8_t *shadow) {
  s_staged = s_build;
  s_build.valid = false;
  if (!shadow) s_staged.valid = false;
  if (s_staged.valid) memcpy(s_shadow, shadow, sizeof(s_shadow));
}

void ActRaiserSpriteOwnership_Upload(uint8_t group, uint8_t map,
                                    const uint8_t *shadow) {
  /* This equality only detects an unobserved overwrite of an already owned
   * build. No contents, positions, attributes or colours choose an owner. */
  if (!shadow || !s_staged.valid || s_staged.group != group ||
      s_staged.map != map || memcmp(s_shadow, shadow, sizeof(s_shadow))) {
    s_presented = (ActRaiserSpriteOwnership){0};
    return;
  }
  s_presented = s_staged;
}

ActRaiserSpriteOwnership ActRaiserSpriteOwnership_Presented(
    uint8_t group, uint8_t map) {
  if (s_presented.group != group || s_presented.map != map)
    return (ActRaiserSpriteOwnership){0};
  return s_presented;
}

bool ActRaiserSpriteOwnership_Range(const ActRaiserSpriteOwnership *ownership,
    ActRaiserSpriteRole role, uint8_t *first, uint8_t *count) {
  if (first) *first = 0;
  if (count) *count = 0;
  if (!ownership || !ownership->valid || role == kActRaiserSprite_Unowned)
    return false;
  unsigned begin = 0;
  while (begin < kActRaiserSpriteSlots && ownership->slots[begin] != role)
    ++begin;
  unsigned end = begin;
  while (end < kActRaiserSpriteSlots && ownership->slots[end] == role) ++end;
  for (unsigned slot = end; slot < kActRaiserSpriteSlots; ++slot)
    if (ownership->slots[slot] == role) return false;
  if (begin == end) return false;
  if (first) *first = (uint8_t)begin;
  if (count) *count = (uint8_t)(end - begin);
  return true;
}
