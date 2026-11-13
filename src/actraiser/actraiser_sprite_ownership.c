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
  for (unsigned slot = first_byte / 4; slot < end_byte / 4; ++slot) {
    s_build.slots[slot] = (uint8_t)role;
    s_build.prop_owner[slot] = 0;
    s_build.prop_shape[slot] = 0;
    s_build.visual_owner[slot]=s_build.visual_source[slot]=s_build.visual_animation[slot]=0;
    s_build.prop_origin_x[slot] = s_build.prop_origin_y[slot] = 0;
  }
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

ActRaiserSpriteRole ActRaiserSpriteOwnership_ActionRole(uint16_t record) {
  const bool aligned=record>=kActRaiserWram_ActionObjectTable &&
      record<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectStride*kActRaiserActionObjectCount &&
      (record-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride==0;
  return record==kActRaiserWram_PlayerObject?kActRaiserSprite_Player:
      aligned && record>kActRaiserWram_PlayerObject?
      kActRaiserSprite_Enemy:kActRaiserSprite_Unowned;
}
void ActRaiserSpriteOwnership_RecordActionObject(uint16_t source,uint16_t record,
    int16_t x,int16_t y,unsigned first_byte,unsigned end_byte) {
  ActRaiserSpriteOwnership_RecordAction(source,first_byte,end_byte);
  if(!s_build.valid)return;
  const ActRaiserSpriteRole role=ActRaiserSpriteOwnership_ActionRole(record);
  if(role==kActRaiserSprite_Unowned || (end_byte>first_byte && s_build.slots[first_byte/4]==kActRaiserSprite_StatueEyes))return;
  ActRaiserSpriteOwnership_Record(role,first_byte,end_byte);
  for(unsigned slot=first_byte/4;slot<end_byte/4;++slot){s_build.world_x[slot]=x;s_build.world_y[slot]=y;}
}

static bool WallTorch(uint16_t source,uint16_t animation,uint8_t bank) {
  return s_build.group==kActRaiserMapGroup_Bloodpool &&
      (s_build.map==3 || s_build.map==5) && source==0xba01 &&
      bank==0x7e && animation==0x4000;
}

void ActRaiserSpriteOwnership_RecordWallTorchPart(uint16_t source,uint16_t record,
    uint16_t animation,uint8_t bank,int16_t origin_x,int16_t origin_y,
    int16_t world_x,int16_t world_y,int16_t x,int16_t y,uint16_t tile_attr,uint8_t size) {
  /* Each reviewed native flame picture is one 16x16 part. An unknown shape or
   * full metadata list stays native; never publish an inferred replacement. */
  if (!s_build.valid || !WallTorch(source,animation,bank) || size!=16 ||
      ActRaiserSpriteOwnership_ActionRole(record)!=kActRaiserSprite_Enemy ||
      s_build.wall_torch_part_count==kActRaiserWallTorchPartLimit) return;
  s_build.wall_torch_parts[s_build.wall_torch_part_count++]=(ActRaiserWallTorchPart){
      .owner=record,.tile_attr=tile_attr,.x=x,.y=y,.origin_x=origin_x,.origin_y=origin_y,
      .world_x=world_x,.world_y=world_y,.size=size};
}

void ActRaiserSpriteOwnership_RecordActionVisual(uint16_t source,uint16_t record,
    uint16_t animation,uint8_t bank,uint16_t composition,
    int16_t origin_x,int16_t origin_y,
    unsigned first_byte,unsigned end_byte) {
  if (!s_build.valid || first_byte > end_byte || end_byte > 512 ||
      (first_byte & 3) || (end_byte & 3) ||
      ActRaiserSpriteOwnership_ActionRole(record) != kActRaiserSprite_Enemy)
    return;
  /* Bloodpool Act 1 places two native log families (US rows $0A:B4CB-B590,
   * group table $00:B449). Type $01 starts $B4C0: single 16x16 logs laid in
   * contiguous bridge runs at cell rows $0F/$16/$18. Those bridge logs are
   * modeled, including after they begin falling with unchanged artwork.
   * Type $00 starts $B499: spaced 48x16 logs on the row-$1F waterline. Those
   * floating logs keep their native flat artwork in every state. $A686
   * collision companions emit no OAM and are never modeled. */
  const bool bridge_log = s_build.group==kActRaiserMapGroup_Bloodpool && s_build.map==1 &&
      source==0xb4c0 && bank==0x7e && animation==0x4000 && composition==0x45bc;
  /* The lava crossing's $D601 emitter keeps composition $4750 while falling
   * under $8661. Its linked $A686 collision record emits no OAM. This is an
   * observed composition identity, independent of pose, position and handler. */
  const bool rock = s_build.group==kActRaiserMapGroup_Aitos && s_build.map==5 &&
      source==0xd601 && bank==0x7e && animation==0x4000 && composition==0x4750;
  unsigned shape=rock?kActRaiserSpriteProp_RockPlatform:
      bridge_log?kActRaiserSpriteProp_Log:0;
  if (s_build.group==kActRaiserMapGroup_Northwall && bank==0x7e && animation==0x4000) {
    /* Match the native ice bridge's solid pose and the two rideable ice
     * families. The linked $A686 collision records emit no artwork. */
    const bool ice_bridge=(s_build.map==1 || s_build.map==2) &&
        source==0xecd4 && (composition==0x4742 || composition==0x474e);
    const bool floating_ice=s_build.map==2 && (source==0xec22 || source==0xec30) &&
        composition==0x474e;
    const bool rising_ice=s_build.map==3 && source==0xec8c && composition==0x478e;
    if (ice_bridge || floating_ice || rising_ice) shape=kActRaiserSpriteProp_IcePlatform;
  }
  /* Bloodpool's stationary $BA01 decoration loops in $BA0D with animation
   * state 8 (three native flame pictures). Its sconce and existing glow live
   * on BG1. Source descriptors are canonical US native identities even when
   * regional terrain/artwork is selected. Do not match fire enemies, their
   * projectiles, or the neighbouring $BA12/$BA23 scenery descriptors. */
  if (WallTorch(source,animation,bank))
    shape=kActRaiserSpriteProp_WallTorch;
  /* Observed temple emitters, including their falling $8661 states. The
   * source/animation/composition contract excludes collision helpers and the
   * similarly moving $E3A1 skull. Native parts still provide pixels/priority. */
  if (s_build.group==kActRaiserMapGroup_Marahna && bank==0x7e) {
    if (animation==0x4000) {
      if (s_build.map==4 && source==0xe42a && composition==0x49f4)
        shape=kActRaiserSpriteProp_TempleBridge;
      if ((s_build.map==5 && source==0xe2d1) || (s_build.map==7 && source==0xe2a6)) {
        if (composition==0x48be) shape=kActRaiserSpriteProp_TemplePlatform;
      }
      if (s_build.map==7 && source==0xe292 && composition==0x488f)
        shape=kActRaiserSpriteProp_TempleCrusher;
      if (s_build.map==7 && source==0xe2bd && composition==0x48d1)
        shape=kActRaiserSpriteProp_TempleSpikes;
    } else if (s_build.map==8 && animation==0x5000) {
      if (source==0xe5cf && composition==0x5ccd) shape=kActRaiserSpriteProp_TemplePlatform;
      if (source==0xe606 && composition==0x6022) shape=kActRaiserSpriteProp_TempleSpikeRail;
    }
  }
  for (unsigned slot = first_byte / 4; slot < end_byte / 4; ++slot) {
    if (s_build.slots[slot] != kActRaiserSprite_Enemy) continue;
    s_build.visual_owner[slot]=record;
    s_build.visual_source[slot]=source;
    s_build.visual_animation[slot]=animation;
    s_build.prop_owner[slot] = shape ? record : 0;
    s_build.prop_shape[slot] = shape;
    s_build.prop_origin_x[slot] = origin_x;
    s_build.prop_origin_y[slot] = origin_y;
  }
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
    if ((ActRaiser_IsSimulationTown(s_build.group, s_build.map) ||
         s_build.map == kActRaiserNonActionMap_Temple) &&
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
  /* The temple's two world records are the entire church UI block list.
   * Fixed menu records, including Yes/No, remain ordinary foreground sprites
   * even when their pixels overlap a visitor's presentation cell. */
  if (s_build.group == kActRaiserMapGroup_NonAction &&
      s_build.map == kActRaiserNonActionMap_Temple &&
      (record == 0x0f0c || record == 0x0f32))
    role = kActRaiserSprite_ChurchVisitor;
  ActRaiserSpriteOwnership_Record(role, first_byte, end_byte);
}

void ActRaiserSpriteOwnership_Complete(const uint8_t *shadow) {
  /* OAM owners retain their existing first-slot tags. Apron-only torches use
   * unused tags from the same bounded domain, so the existing winner mask can
   * distinguish their pixels from every other modeled owner. */
  bool used[kActRaiserSpriteSlots+1]={false};
  for(unsigned i=0;i<kActRaiserSpriteSlots;++i) {
    const unsigned tag=ActRaiserSpriteOwnership_PropTag(&s_build,s_build.prop_owner[i]);
    if(tag)used[tag]=true;
    const unsigned visual=ActRaiserSpriteOwnership_VisualTag(&s_build,s_build.visual_owner[i]);
    if(visual)used[visual]=true;
  }
  for(unsigned i=0;i<s_build.wall_torch_part_count;++i) {
    ActRaiserWallTorchPart *part=&s_build.wall_torch_parts[i];
    unsigned tag=ActRaiserSpriteOwnership_PropTag(&s_build,part->owner);
    if(!tag)for(tag=kActRaiserSpriteSlots;tag && used[tag];--tag) {}
    part->owner_tag=(uint8_t)tag;
    if(tag)used[tag]=true;
  }
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
