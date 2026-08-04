#include "action_effects.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kMagicalFireKind = 1,
  kMagicalFireAnimationAddress = 0xC000,
  /* Fire's animation definition lives at $07:C000 — the bank is a single byte
   * at +$18 (see ReadActionObject). */
  kMagicalFireAnimationBank = 0x07,
  kMagicalFireFirstVisual = 5,
  kMagicalFireLastVisual = 43,
  kMagicalFireIgnitionState = 2,
  kMagicalFireBloomState = 3,
  /* Fire's decoded OAM attributes are $02/$82: OBJ priority 0. The value is
   * presentation metadata, not inferred in present.c from tile graphics. */
  kMagicalFireObjPriority = 0,
};

typedef struct ActionObjectSnapshot {
  uint16_t status;
  int16_t world_x, world_y;
  int16_t velocity_x, velocity_y;
  uint16_t left_extent, top_extent, right_extent, bottom_extent;
  uint16_t animation_address;
  uint8_t animation_bank;
  uint16_t animation_state, animation_index;
  uint16_t composition, visual, flip_attributes;
} ActionObjectSnapshot;

typedef struct MagicalFireSlotRule {
  uint8_t cohort_index;
  uint16_t expected_flips;
} MagicalFireSlotRule;

static const MagicalFireSlotRule kMagicalFireSlots[] = {
  { 0, 0x0000 },
  { 1, kActRaiserObjectFlip_Horizontal },
  { 2, kActRaiserObjectFlip_Vertical },
  { 3, kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical },
};

_Static_assert(kActionEffectObserverTrackCount ==
                   kActRaiserActionMagicCohortCount,
               "observer needs one tracker per action-magic cohort slot");
_Static_assert(sizeof(kMagicalFireSlots) / sizeof(kMagicalFireSlots[0]) <=
                   kActionEffectMaxInstances,
               "Magical Fire exceeds the published effect frame capacity");

static uint16_t Read16(const uint8_t *wram, size_t wram_size,
                       size_t address) {
  if (!wram || address + 1 >= wram_size) return 0;
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

static uint8_t Read8(const uint8_t *wram, size_t wram_size, size_t address) {
  if (!wram || address >= wram_size) return 0;
  return wram[address];
}

static uint16_t AddSaturated16(uint16_t value, unsigned amount) {
  if (amount > UINT16_MAX - value) return UINT16_MAX;
  return (uint16_t)(value + amount);
}

static uint32_t AllocateSequence(uint32_t *next) {
  if (!next) return 0;
  if (!*next) *next = 1;
  uint32_t sequence = (*next)++;
  if (!*next) *next = 1;
  return sequence;
}

void ActionEffectObserver_Reset(ActionEffectObserver *observer) {
  if (!observer) return;
  memset(observer, 0, sizeof(*observer));
  observer->next_generation = 1;
  observer->next_pulse_generation = 1;
}

static void RetireAll(ActionEffectObserver *observer) {
  if (!observer) return;
  memset(observer->tracks, 0, sizeof(observer->tracks));
}

static bool IsActionMap(const uint8_t *wram, size_t wram_size) {
  if (!wram || wram_size <= kActRaiserWram_MapGroup) return false;
  uint8_t group = wram[kActRaiserWram_MapGroup];
  return group >= kActRaiserActionMapGroup_First &&
      group <= kActRaiserActionMapGroup_Last;
}

static bool ReadActionObject(const uint8_t *wram, size_t wram_size,
                             uint16_t address,
                             ActionObjectSnapshot *object) {
  if (!object || !wram ||
      (size_t)address + kActRaiserActionObjectStride > wram_size)
    return false;
  *object = (ActionObjectSnapshot){
    .status = Read16(wram, wram_size,
                     address + kActRaiserActionObject_Status),
    .world_x = (int16_t)Read16(wram, wram_size,
                               address + kActRaiserActionObject_WorldX),
    .world_y = (int16_t)Read16(wram, wram_size,
                               address + kActRaiserActionObject_WorldY),
    .velocity_x = (int16_t)Read16(
        wram, wram_size, address + kActRaiserActionObject_VelocityX),
    .velocity_y = (int16_t)Read16(
        wram, wram_size, address + kActRaiserActionObject_VelocityY),
    .left_extent = Read16(wram, wram_size,
                          address + kActRaiserActionObject_LeftExtent),
    .top_extent = Read16(wram, wram_size,
                         address + kActRaiserActionObject_TopExtent),
    .right_extent = Read16(wram, wram_size,
                           address + kActRaiserActionObject_RightExtent),
    .bottom_extent = Read16(wram, wram_size,
                            address + kActRaiserActionObject_BottomExtent),
    .animation_address = Read16(
        wram, wram_size, address + kActRaiserActionObject_AnimationAddress),
    /* BYTE, not word. +$16..+$18 is the 24-bit animation pointer (addr16 then
     * bank8) and +$19 is a separate field — the record's base OAM attribute
     * byte, mirroring the live copy at +$29. A 16-bit read here returns
     * bank | attributes<<8 ($3907 for live Magical Fire, not $0007), so the
     * identity test never matched and no spell was ever captured. Every other
     * consumer of this field already reads it 8-bit
     * (actraiser_widescreen_sprites.c). */
    .animation_bank = Read8(
        wram, wram_size, address + kActRaiserActionObject_AnimationBank),
    .animation_state = Read16(
        wram, wram_size, address + kActRaiserActionObject_AnimationState),
    .animation_index = Read16(
        wram, wram_size, address + kActRaiserActionObject_AnimationIndex),
    .composition = Read16(wram, wram_size,
                          address + kActRaiserActionObject_Composition),
    .visual = Read16(wram, wram_size,
                     address + kActRaiserActionObject_Visual),
    .flip_attributes = Read16(
        wram, wram_size, address + kActRaiserActionObject_FlipAttributes),
  };
  return true;
}

static bool MagicControllerKind(const uint8_t *wram, size_t wram_size,
                                uint16_t *kind) {
  ActionObjectSnapshot controller;
  if (!ReadActionObject(wram, wram_size, kActRaiserWram_MagicController,
                        &controller) ||
      (controller.status & kActRaiserObjectStatus_InactiveMask))
    return false;
  if (kind) {
    *kind = Read16(wram, wram_size,
                   kActRaiserWram_MagicController +
                       kActRaiserActionObject_LocalCounter);
  }
  return true;
}

static bool MagicalFireObjectIsActive(const ActionObjectSnapshot *object) {
  if (!object ||
      (object->status & kActRaiserObjectStatus_InactiveMask))
    return false;
  return object->animation_address == kMagicalFireAnimationAddress &&
      object->animation_bank == kMagicalFireAnimationBank &&
      object->composition != 0 &&
      object->visual >= kMagicalFireFirstVisual &&
      object->visual <= kMagicalFireLastVisual &&
      (object->animation_state == kMagicalFireIgnitionState ||
       object->animation_state == kMagicalFireBloomState);
}

static void BeginOrAdvanceTrack(ActionEffectObserver *observer,
                                unsigned cohort_index, uint8_t kind,
                                uint8_t phase, uint16_t pulse_key,
                                unsigned elapsed_ticks,
                                ActionEffectInstance *effect) {
  ActionEffectObserverTrack *track = &observer->tracks[cohort_index];
  bool new_actor = !track->active || track->kind != kind;
  if (new_actor) {
    memset(track, 0, sizeof(*track));
    track->active = 1;
    track->kind = kind;
    track->phase = phase;
    track->pulse_key = pulse_key;
    track->generation = AllocateSequence(&observer->next_generation);
    track->pulse_generation =
        AllocateSequence(&observer->next_pulse_generation);
  } else {
    track->age_ticks = AddSaturated16(track->age_ticks, elapsed_ticks);
    if (track->phase != phase) {
      track->phase = phase;
      track->phase_ticks = 0;
    } else {
      track->phase_ticks = AddSaturated16(
          track->phase_ticks, elapsed_ticks);
    }
    if (track->pulse_key != pulse_key) {
      track->pulse_key = pulse_key;
      track->pulse_ticks = 0;
      track->pulse_generation =
          AllocateSequence(&observer->next_pulse_generation);
    } else {
      track->pulse_ticks = AddSaturated16(
          track->pulse_ticks, elapsed_ticks);
    }
  }
  effect->generation = track->generation;
  effect->pulse_generation = track->pulse_generation;
  effect->age_ticks = track->age_ticks;
  effect->phase_ticks = track->phase_ticks;
  effect->pulse_ticks = track->pulse_ticks;
}

static void CaptureMagicalFire(ActionEffectObserver *observer,
                               ActionEffectFrame *dst,
                               const uint8_t *wram, size_t wram_size,
                               unsigned elapsed_ticks, bool seen[]) {
  for (size_t i = 0;
       i < sizeof(kMagicalFireSlots) / sizeof(kMagicalFireSlots[0]); i++) {
    const MagicalFireSlotRule *rule = &kMagicalFireSlots[i];
    uint16_t address = (uint16_t)(kActRaiserWram_ActionObjectTable +
        rule->cohort_index * kActRaiserActionObjectStride);
    ActionObjectSnapshot object;
    if (!ReadActionObject(wram, wram_size, address, &object) ||
        !MagicalFireObjectIsActive(&object) ||
        (object.flip_attributes & kActRaiserObjectFlip_Mask) !=
            rule->expected_flips)
      continue;
    if (dst->effect_count >= kActionEffectMaxInstances) break;

    seen[rule->cohort_index] = true;
    ActionEffectInstance *effect = &dst->effects[dst->effect_count++];
    effect->record_address = address;
    effect->world_x = object.world_x;
    effect->world_y = object.world_y;
    effect->velocity_x = object.velocity_x;
    effect->velocity_y = object.velocity_y;
    effect->left_extent = object.left_extent;
    effect->top_extent = object.top_extent;
    effect->right_extent = object.right_extent;
    effect->bottom_extent = object.bottom_extent;
    effect->composition = object.composition;
    effect->visual = object.visual;
    effect->animation_state = object.animation_state;
    effect->animation_index = object.animation_index;
    effect->flip_attributes = object.flip_attributes;
    effect->kind = kActionEffect_MagicalFire;
    effect->phase = object.animation_state == kMagicalFireIgnitionState
        ? kActionEffectPhase_FireIgnition : kActionEffectPhase_FireBloom;
    effect->obj_priority = kMagicalFireObjPriority;
    effect->render_layer = kActionEffectRenderLayer_WorldOverlay;
    effect->geometry = (ActionEffectGeometry){
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {
        -(float)object.left_extent,
        -(float)object.top_extent,
        (float)object.right_extent,
        (float)object.bottom_extent,
      },
    };
    /* Fire is one continuous emitter across its two visual phases. Stardust
     * will instead pass its launch counter/state as pulse_key. */
    BeginOrAdvanceTrack(observer, rule->cohort_index, effect->kind,
                        effect->phase, 0, elapsed_ticks, effect);
    if (!(object.status & (kActRaiserObjectStatus_IneligibleMask |
                           kActRaiserObjectStatus_NoDraw))) {
      effect->flags |= kActionEffectFlag_Visible;
      dst->visible_count++;
    }
    if (object.flip_attributes & kActRaiserObjectFlip_Horizontal)
      effect->flags |= kActionEffectFlag_FlipHorizontal;
    if (object.flip_attributes & kActRaiserObjectFlip_Vertical)
      effect->flags |= kActionEffectFlag_FlipVertical;
  }
}

void ActionEffects_CaptureFrame(ActionEffectObserver *observer,
                                ActionEffectFrame *dst,
                                const uint8_t *wram, size_t wram_size,
                                unsigned elapsed_ticks) {
  if (!dst) return;
  memset(dst, 0, sizeof(*dst));
  if (!observer) return;
  if (!observer->next_generation || !observer->next_pulse_generation)
    ActionEffectObserver_Reset(observer);
  if (wram && wram_size > kActRaiserWram_GameFrame + 1)
    dst->game_frame = Read16(wram, wram_size, kActRaiserWram_GameFrame);

  uint16_t controller_kind = 0;
  if (!IsActionMap(wram, wram_size) ||
      !MagicControllerKind(wram, wram_size, &controller_kind)) {
    RetireAll(observer);
    return;
  }
  dst->controller_kind = (uint8_t)controller_kind;

  bool seen[kActionEffectObserverTrackCount] = {false};
  switch (controller_kind) {
    case kMagicalFireKind:
      CaptureMagicalFire(observer, dst, wram, wram_size,
                         elapsed_ticks, seen);
      break;
    default:
      break;
  }
  for (unsigned i = 0; i < kActionEffectObserverTrackCount; i++) {
    if (!seen[i]) memset(&observer->tracks[i], 0, sizeof(observer->tracks[i]));
  }
}
