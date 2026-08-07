#include "action_effects.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "actraiser_game.h"

/* ── Spell rule table ──────────────────────────────────────────────────────
 *
 * Every spell is declared as data rather than as code. The shape comes from
 * docs/effects-hook-investigation.md "Spell catalogue", which statically maps
 * all four casts: $00:9F13 dispatches controller +$38 (the spell ID) to
 * $9F25/$9F71/$9FBB/$9FFA, and the cohort slots $06A0-$0820 are the emitter
 * instances for whichever one is running.
 *
 * PROVENANCE, because it is not uniform and matters for trust:
 *   - Magical Fire's rules are MEASURED. Every field below was checked against
 *     runs/20260803-162833's mid-cast WRAM snapshot and is pinned by
 *     TestLiveWramRecordIsRecognized.
 *   - Magical Stardust's rules are MEASURED too, from runs/20260805-073012
 *     and -074959 (see its block below).
 *   - Aura and Light are still TRANSCRIBED from the ROM analysis and have
 *     never been seen against live WRAM. They are written to fail closed — an
 *     active slot that matches nothing is captured into the frame's
 *     `unmatched` census instead of being rendered on a guess — so the first
 *     real cast of each either confirms the rule or prints exactly what it
 *     should have been. See frame_slot.c's [action-fx census] line. */

enum {
  kAnyState = 0xFFFFu,
  kAnyVisual = 0xFFFFu,
};

typedef enum SpellFlipMode {
  kFlipExact = 0,   /* the slot's flip bits must equal expected_flips */
  kFlipAny,         /* the ROM does not assign flips per slot for this spell */
} SpellFlipMode;

typedef struct SpellSlotRule {
  uint8_t cohort_index;
  uint16_t expected_flips;
  uint8_t flip_mode;
  uint8_t role;
} SpellSlotRule;

/* Ordered: the first rule matching (role, state, visual) wins, so a specific
 * rule can precede a catch-all for the same role. */
typedef struct SpellPhaseRule {
  uint16_t animation_state;              /* kAnyState = do not test */
  uint16_t first_visual, last_visual;    /* kAnyVisual = do not test */
  uint8_t role;
  uint8_t phase;
  /* Require a non-zero velocity to match. Some stages are distinguishable
   * ONLY by motion: a Stardust actor sitting on the player before its launch
   * handler has run carries the same state and visual as one in flight. */
  bool requires_motion;
} SpellPhaseRule;

typedef struct SpellRule {
  uint8_t controller_kind;               /* controller $0860 + $38 */
  uint8_t kind;
  uint16_t animation_address;
  uint8_t animation_bank;
  uint8_t obj_priority;
  /* Stardust relaunches each actor four times. Slot +$38 is polymorphic and
   * the spell handlers use it as a repeat count, so it is exactly the value
   * that should restart a particle clock without ending the actor's outer
   * generation. Off for spells whose actors launch once. */
  bool pulse_from_local_counter;
  const SpellSlotRule *slots;
  uint8_t slot_count;
  const SpellPhaseRule *phases;
  uint8_t phase_count;
} SpellRule;

/* --- 1 Magical Fire (MEASURED) -------------------------------------------
 * Four clones born at the player with every flip combination; together a
 * four-way sweep. State 2 is the 9-tick ignition, state 3 the 32-tick bloom
 * (repeated twice). Visual range 5..43 spans both. */
static const SpellSlotRule kFireSlots[] = {
  { 0, 0x0000, kFlipExact, kActionEffectRole_Body },
  { 1, kActRaiserObjectFlip_Horizontal, kFlipExact, kActionEffectRole_Body },
  { 2, kActRaiserObjectFlip_Vertical, kFlipExact, kActionEffectRole_Body },
  { 3, kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical,
    kFlipExact, kActionEffectRole_Body },
};
static const SpellPhaseRule kFirePhases[] = {
  { 2, 5, 43, kActionEffectRole_Body, kActionEffectPhase_FireIgnition },
  { 3, 5, 43, kActionEffectRole_Body, kActionEffectPhase_FireBloom },
};

/* --- 2 Magical Stardust (MEASURED) ---------------------------------------
 * Same four cohort slots, staggered by 0/20/40/60 ticks, each actor launching
 * four times (16 launch/burst opportunities). Shares Fire's $07:C000 bank —
 * the controller kind is what separates them.
 *
 * MEASURED 2026-08-05 from runs/20260805-073012's three mid-cast snapshots,
 * which is where these numbers stop being transcription:
 *   flight = state 0, visual 0, extents 8/8/8/8 (a 16x16 box), comp $C13F,
 *            velocity exactly (-8,+8) — a true 45-degree descent, and never
 *            mirrored: the flip bits were 0 on every slot in every snapshot.
 *   burst  = state 1, visuals 1..4, growing 8x8 -> 32x32 (comp $C14B at
 *            visual 1, $C199 at visual 4), velocity (0,0).
 *
 * Both stages are exact rules rather than catch-alls, so an unexpected
 * Stardust stage still reaches the census instead of being silently absorbed
 * into whichever rule happened to be last. */
static const SpellSlotRule kStardustSlots[] = {
  { 0, 0x0000, kFlipAny, kActionEffectRole_Body },
  { 1, 0x0000, kFlipAny, kActionEffectRole_Body },
  { 2, 0x0000, kFlipAny, kActionEffectRole_Body },
  { 3, 0x0000, kFlipAny, kActionEffectRole_Body },
};
static const SpellPhaseRule kStardustPhases[] = {
  { 1, 1, 4, kActionEffectRole_Body, kActionEffectPhase_StardustBurst, false },
  /* Order matters: a moving star is in flight, a still one has not launched
   * yet. Both are state 0 / visual 0 — measured at spawn as world (308,520)
   * with velocity (0,0), exactly the player's position, which is the state
   * the catalogue means by "launch position is NOT retained at the player". */
  { 0, 0, 0, kActionEffectRole_Body, kActionEffectPhase_StardustLaunch, true },
  { 0, 0, 0, kActionEffectRole_Body,
    kActionEffectPhase_StardustPreLaunch, false },
};

/* --- 3 Magical Aura (TRANSCRIBED) ----------------------------------------
 * Four player-born slots with all flip combinations, like Fire, but on the
 * $07:C800 bank. State 3 runs 116 ticks over 60 entries alternating visuals
 * 10/11, each a four-part 32x32 orb. These are MOVING emitters — the
 * catalogue is explicit that they must follow slot +02/+04 every tick rather
 * than be treated as a stationary halo, which the per-instance world position
 * already does. */
static const SpellSlotRule kAuraSlots[] = {
  { 0, 0x0000, kFlipExact, kActionEffectRole_Body },
  { 1, kActRaiserObjectFlip_Horizontal, kFlipExact, kActionEffectRole_Body },
  { 2, kActRaiserObjectFlip_Vertical, kFlipExact, kActionEffectRole_Body },
  { 3, kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical,
    kFlipExact, kActionEffectRole_Body },
};
static const SpellPhaseRule kAuraPhases[] = {
  { 3, 10, 11, kActionEffectRole_Body, kActionEffectPhase_AuraOrb },
};

/* --- 4 Magical Light (TRANSCRIBED) ---------------------------------------
 * The one spell whose parts are not interchangeable: a stationary centre
 * flare at $07A0 plus two mirrored 16x224 beam columns at $07E0/$0820 that
 * separate horizontally late in the cast. Centre visuals 5..9 grow to 9
 * parts; column visuals 1..4 are the 14 stacked 16x16 parts that form the
 * beam. A column showing anything else is the pre-beam stage, which the
 * catalogue explicitly says must not receive full intensity — hence the
 * catch-all AFTER the beam rule rather than no rule at all. */
static const SpellSlotRule kLightSlots[] = {
  { 4, 0x0000, kFlipAny, kActionEffectRole_Centre },
  { 5, 0x0000, kFlipAny, kActionEffectRole_Column },
  { 6, 0x0000, kFlipAny, kActionEffectRole_Column },
};
static const SpellPhaseRule kLightPhases[] = {
  { kAnyState, 5, 9, kActionEffectRole_Centre,
    kActionEffectPhase_LightFlare },
  { kAnyState, 1, 4, kActionEffectRole_Column,
    kActionEffectPhase_LightBeam },
  { kAnyState, kAnyVisual, kAnyVisual, kActionEffectRole_Column,
    kActionEffectPhase_LightBeamCharge },
};

#define SPELL_RULE(kind_enum, controller, addr, bank, pulse, slots, phases) \
  { (uint8_t)(controller), (uint8_t)(kind_enum), (uint16_t)(addr),          \
    (uint8_t)(bank), 0, (pulse), (slots),                                   \
    (uint8_t)(sizeof(slots) / sizeof((slots)[0])), (phases),                \
    (uint8_t)(sizeof(phases) / sizeof((phases)[0])) }

/* obj_priority is 0 for every spell: the investigation decodes the action
 * spell compositions into the same OBJ band, and the value is presentation
 * metadata carried alongside the instance rather than re-derived in
 * present.c from tile graphics. */
static const SpellRule kSpellRules[] = {
  SPELL_RULE(kActionEffect_MagicalFire, 1, 0xC000, 0x07, false,
             kFireSlots, kFirePhases),
  SPELL_RULE(kActionEffect_MagicalStardust, 2, 0xC000, 0x07, true,
             kStardustSlots, kStardustPhases),
  SPELL_RULE(kActionEffect_MagicalAura, 3, 0xC800, 0x07, false,
             kAuraSlots, kAuraPhases),
  SPELL_RULE(kActionEffect_MagicalLight, 4, 0xC800, 0x07, false,
             kLightSlots, kLightPhases),
};

_Static_assert(kActionEffectObserverTrackCount ==
                   kActRaiserActionMagicCohortCount,
               "observer needs one tracker per action-magic cohort slot");

typedef struct ActionObjectSnapshot {
  uint16_t status;
  int16_t world_x, world_y;
  int16_t velocity_x, velocity_y;
  uint16_t left_extent, top_extent, right_extent, bottom_extent;
  uint16_t animation_address;
  uint8_t animation_bank;
  uint16_t animation_state, animation_index;
  uint16_t composition, visual, flip_attributes;
  uint16_t local_counter;
} ActionObjectSnapshot;

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
     * bank8) and +$19 is a separate field. A 16-bit read here returns
     * bank | next<<8 ($3907 for live Magical Fire, not $0007), so the identity
     * test never matched and no spell was ever captured. Every other consumer
     * of this field already reads it 8-bit
     * (actraiser_widescreen_sprites.c). See docs/bug-ledger.md §32. */
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
    .local_counter = Read16(
        wram, wram_size, address + kActRaiserActionObject_LocalCounter),
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
  if (kind) *kind = controller.local_counter;
  return true;
}

static const SpellRule *FindSpellRule(uint16_t controller_kind) {
  for (size_t i = 0; i < sizeof(kSpellRules) / sizeof(kSpellRules[0]); i++)
    if (kSpellRules[i].controller_kind == controller_kind)
      return &kSpellRules[i];
  return NULL;
}

static const SpellSlotRule *FindSlotRule(const SpellRule *rule,
                                         unsigned cohort_index) {
  for (uint8_t i = 0; i < rule->slot_count; i++)
    if (rule->slots[i].cohort_index == cohort_index) return &rule->slots[i];
  return NULL;
}

static uint8_t MatchPhase(const SpellRule *rule, uint8_t role,
                          uint16_t state, uint16_t visual, bool moving) {
  for (uint8_t i = 0; i < rule->phase_count; i++) {
    const SpellPhaseRule *phase = &rule->phases[i];
    if (phase->role != role) continue;
    if (phase->animation_state != kAnyState &&
        phase->animation_state != state)
      continue;
    if (phase->first_visual != kAnyVisual &&
        (visual < phase->first_visual || visual > phase->last_visual))
      continue;
    if (phase->requires_motion && !moving) continue;
    return phase->phase;
  }
  return kActionEffectPhase_None;
}

static void RecordUnmatched(ActionEffectFrame *dst, uint16_t address,
                            const ActionObjectSnapshot *object) {
  if (dst->unmatched_count >= kActionEffectMaxInstances) return;
  dst->unmatched[dst->unmatched_count++] = (ActionEffectUnmatched){
    .record_address = address,
    .status = object->status,
    .animation_address = object->animation_address,
    .animation_state = object->animation_state,
    .visual = object->visual,
    .composition = object->composition,
    .flip_attributes = object->flip_attributes,
    .animation_bank = object->animation_bank,
  };
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
  const SpellRule *rule = FindSpellRule(controller_kind);

  /* Walk the whole cohort rather than only the rule's declared slots, so an
   * active slot the table does not describe is still SEEN. That is the
   * difference between "this spell is not implemented yet" and silence. */
  bool seen[kActionEffectObserverTrackCount] = {false};
  for (unsigned cohort = 0; cohort < kActionEffectObserverTrackCount;
       cohort++) {
    uint16_t address = (uint16_t)(kActRaiserWram_ActionObjectTable +
        cohort * kActRaiserActionObjectStride);
    ActionObjectSnapshot object;
    if (!ReadActionObject(wram, wram_size, address, &object) ||
        (object.status & kActRaiserObjectStatus_InactiveMask) ||
        !object.composition)
      continue;

    const SpellSlotRule *slot = rule ? FindSlotRule(rule, cohort) : NULL;
    if (!rule || !slot ||
        object.animation_address != rule->animation_address ||
        object.animation_bank != rule->animation_bank ||
        (slot->flip_mode == kFlipExact &&
         (object.flip_attributes & kActRaiserObjectFlip_Mask) !=
             slot->expected_flips)) {
      RecordUnmatched(dst, address, &object);
      continue;
    }
    uint8_t phase = MatchPhase(rule, slot->role, object.animation_state,
                               object.visual,
                               object.velocity_x || object.velocity_y);
    if (phase == kActionEffectPhase_None) {
      RecordUnmatched(dst, address, &object);
      continue;
    }
    if (dst->effect_count >= kActionEffectMaxInstances) break;

    seen[cohort] = true;
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
    effect->kind = rule->kind;
    effect->phase = phase;
    effect->role = slot->role;
    effect->obj_priority = rule->obj_priority;
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
    BeginOrAdvanceTrack(observer, cohort, effect->kind, effect->phase,
                        rule->pulse_from_local_counter ? object.local_counter
                                                       : 0,
                        elapsed_ticks, effect);
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

  for (unsigned i = 0; i < kActionEffectObserverTrackCount; i++)
    if (!seen[i]) memset(&observer->tracks[i], 0, sizeof(observer->tracks[i]));
}
