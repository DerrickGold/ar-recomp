#include "action_effects.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "actraiser_game.h"
#include "action_bg_world.h"

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
_Static_assert(kActionSceneEffectObserverTrackCount ==
                   kActRaiserActionObjectCount,
               "scene observer needs one tracker per action-object slot");

typedef struct ActionObjectSnapshot {
  uint16_t status;
  int16_t world_x, world_y;
  int16_t velocity_x, velocity_y;
  uint16_t left_extent, top_extent, right_extent, bottom_extent;
  uint16_t handler;
  uint16_t animation_address;
  uint8_t animation_bank;
  uint16_t animation_state, animation_index;
  uint16_t resume_address;
  uint16_t composition, visual, flip_attributes;
  uint16_t flags;
  uint16_t source_descriptor;
  uint16_t local_counter;
  uint16_t spawner_backlink;
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

static void RetireSceneAll(ActionEffectObserver *observer) {
  if (!observer) return;
  memset(observer->scene_tracks, 0, sizeof(observer->scene_tracks));
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
    .handler = Read16(wram, wram_size,
                      address + kActRaiserActionObject_Handler),
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
    .resume_address = Read16(
        wram, wram_size, address + kActRaiserActionObject_ResumeAddress),
    .composition = Read16(wram, wram_size,
                          address + kActRaiserActionObject_Composition),
    .visual = Read16(wram, wram_size,
                     address + kActRaiserActionObject_Visual),
    .flip_attributes = Read16(
        wram, wram_size, address + kActRaiserActionObject_FlipAttributes),
    .flags = Read16(wram, wram_size,
                    address + kActRaiserActionObject_Flags),
    .source_descriptor = Read16(
        wram, wram_size, address + kActRaiserActionObject_SourceDescriptor),
    .local_counter = Read16(
        wram, wram_size, address + kActRaiserActionObject_LocalCounter),
    .spawner_backlink = Read16(
        wram, wram_size, address + kActRaiserActionObject_SpawnerBacklink),
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
                                ActionEffectObserverTrack *track, uint8_t kind,
                                uint8_t phase, uint16_t pulse_key,
                                unsigned elapsed_ticks,
                                ActionEffectInstance *effect) {
  if (!observer || !track || !effect) return;
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

static unsigned AbsInt(int value) {
  return (unsigned)(value < 0 ? -value : value);
}

static unsigned SceneMotionLimit(int16_t current_velocity,
                                 int16_t previous_velocity,
                                 unsigned ticks) {
  unsigned speed = AbsInt(current_velocity);
  if (AbsInt(previous_velocity) > speed)
    speed = AbsInt(previous_velocity);
  if (speed && ticks > (UINT_MAX - 8u) / speed) return UINT_MAX;
  return speed * ticks + 8u;
}

/* Ordinary action slots have no outer controller lifetime. A projectile can
 * be freed and another member of the same family allocated into that address
 * between two captures, so (slot, kind) alone is not an actor identity.
 *
 * The control-flow/source tuple catches reuse by a different spawner. The
 * bounded motion check catches reuse by the same spawner: the measured
 * fireballs move three pixels/tick and lightning is stationary, while a new
 * actor appears back at its source. Eight pixels of slack admits handler-side
 * subpixel/collision adjustment without allowing a screen-space teleport to
 * inherit the old particle generation. 16-bit subtraction preserves normal
 * world-coordinate wrap. */
static bool SceneTrackDiscontinuous(const ActionEffectObserverTrack *track,
                                    const ActionObjectSnapshot *object,
                                    uint8_t kind, uint32_t continuity_key,
                                    unsigned elapsed_ticks) {
  if (!track || !object || !track->active || track->kind != kind ||
      !track->continuity_valid)
    return false;
  if (track->continuity_key != continuity_key) return true;

  const int dx = (int16_t)((uint16_t)object->world_x -
                           (uint16_t)track->last_world_x);
  const int dy = (int16_t)((uint16_t)object->world_y -
                           (uint16_t)track->last_world_y);
  const unsigned ticks = elapsed_ticks ? elapsed_ticks : 1u;
  const unsigned limit_x = SceneMotionLimit(
      object->velocity_x, track->last_velocity_x, ticks);
  const unsigned limit_y = SceneMotionLimit(
      object->velocity_y, track->last_velocity_y, ticks);
  return AbsInt(dx) > limit_x || AbsInt(dy) > limit_y;
}

static void BeginOrAdvanceSceneTrack(ActionEffectObserver *observer,
                                     ActionEffectObserverTrack *track,
                                     const ActionObjectSnapshot *object,
                                     uint8_t kind, uint8_t phase,
                                     unsigned elapsed_ticks,
                                     ActionEffectInstance *effect) {
  if (!observer || !track || !object || !effect) return;
  /* Resume/source are stable for the projectile and trap families. Fireball's
   * handler is stable too and strengthens its identity; trap lightning omits
   * it because one live bolt transitions between $BD36 and the generic timed
   * animation handler $8683 without becoming a new actor. The boss-lightning
   * child changes resume across its repeated strike/blank cycles, so its
   * stable source/backlink pair is the lifecycle key instead. */
  uint32_t continuity_key = (uint32_t)object->source_descriptor |
      ((uint32_t)object->resume_address << 16);
  if (kind == kActionEffect_EnemyFireball)
    continuity_key ^= (uint32_t)object->handler * 0x9E3779B9u;
  else if (kind == kActionEffect_BloodpoolBossLightning)
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->spawner_backlink << 16);
  if (SceneTrackDiscontinuous(track, object, kind, continuity_key,
                              elapsed_ticks))
    memset(track, 0, sizeof(*track));
  BeginOrAdvanceTrack(observer, track, kind, phase, 0, elapsed_ticks, effect);
  track->continuity_key = continuity_key;
  track->last_world_x = object->world_x;
  track->last_world_y = object->world_y;
  track->last_velocity_x = object->velocity_x;
  track->last_velocity_y = object->velocity_y;
  track->continuity_valid = 1;
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
    effect->projection_plane = kActionEffectProjectionPlane_Obj;
    effect->geometry = (ActionEffectGeometry){
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {
        -(float)object.left_extent,
        -(float)object.top_extent,
        (float)object.right_extent,
        (float)object.bottom_extent,
      },
    };
    BeginOrAdvanceTrack(observer, &observer->tracks[cohort], effect->kind,
                        effect->phase,
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

/* ── Measured action-scene identities ─────────────────────────────────────
 *
 * These rules come from run 20260810-124203. They deliberately combine
 * control-flow identity with animation/composition identity: object fields are
 * polymorphic in ActRaiser, so matching a visual number or palette colour by
 * itself would eventually decorate an unrelated actor.
 *
 * Enemy fireballs (snap_03_gf7397):
 *   handler $BDF0, resume $BDD9, state $23, animation $7E:4000,
 *   visual/composition $17/$45EF or $18/$4610.
 * Lightning traps (snap_04_gf9417):
 *   source descriptor $BD2A, resume $BD69, state $14, animation $7E:4000,
 *   handler $BD36 or the shared animation-repeat handler $8683, and
 *   visual/composition $1F/$46FE or $20/$479D.
 * Bloodpool boss lightning (runs 20260810-174202 and -180202):
 *   map group/current map $02/$08, source descriptor $BDFF, animation
 *   $7E:5000, shared delay handler $8661, and a validated +$3A backlink.
 *   States $02-$07 are six different authored strikes: vertical/diagonal,
 *   each in long/medium/short lengths, using visuals $00-$05. Their shared
 *   visual $20 is the blank half-cycle, not a telegraph. State $09 is the
 *   linked floor impact. Exact state/visual/composition tuples below cover
 *   every strike shape rather than extrapolating from one captured pose.
 * Player sword beam (run 20260810-175403 snap_01_gf1726):
 *   handler $9D1C, animation $06:8000, player backlink $08A0, and exact
 *   state/visual/composition pairs $13/$30/$99E8 or $14/$31/$9A17.
 * Bloodpool wall torches (snap_01_gf2479, snap_06_gf7654):
 *   BG1 metatile $47 immediately above $4F in maps $02/$03 and $02/$05.
 *   The exact pair is the authored object identity and applies across the
 *   Bloodpool group rather than being tied to either observed room number. */
enum {
  kEnemyFireballHandler = 0xBDF0,
  kEnemyFireballResume = 0xBDD9,
  kEnemyFireballState = 0x0023,
  kLightningSourceDescriptor = 0xBD2A,
  kLightningResume = 0xBD69,
  kLightningState = 0x0014,
  kLightningHandler = 0xBD36,
  kAnimationRepeatHandler = 0x8683,
  kSceneAnimationAddress = 0x4000,
  kSceneAnimationBank = 0x7E,
  kBloodpoolBossMap = 0x08,
  kBossLightningSourceDescriptor = 0xBDFF,
  kBossLightningHandler = 0x8661,
  kBossAnimationAddress = 0x5000,
  kBossLightningFirstStrikeState = 0x0002,
  kBossLightningLastStrikeState = 0x0007,
  kBossLightningImpactState = 0x0009,
  kBossLightningImpactResume = 0xC06A,
  kSwordBeamHandler = 0x9D1C,
  kSwordBeamAnimationAddress = 0x8000,
  kSwordBeamAnimationBank = 0x06,
  kSwordBeamHorizontalState = 0x0013,
  kSwordBeamAlternateState = 0x0014,
  kBloodpoolTorchTopMetatile = 0x47,
  kBloodpoolTorchBottomMetatile = 0x4F,
};

static bool ActionObjectVisible(const ActionObjectSnapshot *object) {
  return object &&
      !(object->status & (kActRaiserObjectStatus_InactiveMask |
                          kActRaiserObjectStatus_IneligibleMask |
                          kActRaiserObjectStatus_NoDraw)) &&
      !(object->flags & kActRaiserObjectFlag_OutsideActivation);
}

static bool IsEnemyFireball(const ActionObjectSnapshot *object) {
  if (!object || object->handler != kEnemyFireballHandler ||
      object->resume_address != kEnemyFireballResume ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kEnemyFireballState)
    return false;
  return (object->visual == 0x0017 && object->composition == 0x45EF) ||
      (object->visual == 0x0018 && object->composition == 0x4610);
}

static bool IsLightningTrap(const ActionObjectSnapshot *object) {
  if (!object || object->source_descriptor != kLightningSourceDescriptor ||
      object->resume_address != kLightningResume ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kLightningState ||
      (object->handler != kLightningHandler &&
       object->handler != kAnimationRepeatHandler))
    return false;
  return (object->visual == 0x001F && object->composition == 0x46FE) ||
      (object->visual == 0x0020 && object->composition == 0x479D);
}

static bool ActionObjectAddressIsValid(uint16_t address) {
  const unsigned table_start = kActRaiserWram_ActionObjectTable;
  const unsigned table_end = table_start +
      kActRaiserActionObjectCount * kActRaiserActionObjectStride;
  return address >= table_start && address < table_end &&
      (address - table_start) % kActRaiserActionObjectStride == 0;
}

static bool SwordBeamParentIsValid(const uint8_t *wram, size_t wram_size,
                                   const ActionObjectSnapshot *object) {
  if (!object || object->spawner_backlink != kActRaiserWram_PlayerObject)
    return false;
  ActionObjectSnapshot player;
  return ReadActionObject(wram, wram_size, kActRaiserWram_PlayerObject,
                          &player) &&
      !(player.status & kActRaiserObjectStatus_InactiveMask) &&
      player.composition &&
      player.animation_address == kSwordBeamAnimationAddress &&
      player.animation_bank == kSwordBeamAnimationBank &&
      player.source_descriptor == object->source_descriptor;
}

static bool IsSwordBeam(const uint8_t *wram, size_t wram_size,
                        const ActionObjectSnapshot *object) {
  if (!object || object->handler != kSwordBeamHandler ||
      object->animation_address != kSwordBeamAnimationAddress ||
      object->animation_bank != kSwordBeamAnimationBank ||
      !object->source_descriptor ||
      !(object->flags & kActRaiserObjectFlag_Attacker) ||
      !SwordBeamParentIsValid(wram, wram_size, object))
    return false;
  return (object->animation_state == kSwordBeamHorizontalState &&
          object->visual == 0x0030 && object->composition == 0x99E8) ||
      (object->animation_state == kSwordBeamAlternateState &&
       object->visual == 0x0031 && object->composition == 0x9A17);
}

static bool BloodpoolBossLightningParentIsValid(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object || !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  return ReadActionObject(wram, wram_size, object->spawner_backlink,
                          &parent) &&
      !(parent.status & kActRaiserObjectStatus_InactiveMask) &&
      parent.composition &&
      parent.source_descriptor == kBossLightningSourceDescriptor &&
      parent.animation_address == kBossAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank;
}

static uint8_t MatchBloodpoolBossLightning(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object || object->source_descriptor !=
          kBossLightningSourceDescriptor ||
      object->handler != kBossLightningHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical) ||
      !BloodpoolBossLightningParentIsValid(wram, wram_size, object))
    return kActionEffectPhase_None;

  static const uint16_t kStrikeCompositions[] = {
    0x5346, 0x5401, 0x5492, 0x54F2, 0x55C2, 0x5661,
  };
  if (object->animation_state >= kBossLightningFirstStrikeState &&
      object->animation_state <= kBossLightningLastStrikeState) {
    const unsigned strike =
        object->animation_state - kBossLightningFirstStrikeState;
    if (object->visual == strike &&
        object->composition == kStrikeCompositions[strike])
      return kActionEffectPhase_BossLightningStrike;
  }

  if (object->animation_state == kBossLightningImpactState &&
      object->resume_address == kBossLightningImpactResume &&
      ((object->visual == 0x0008 && object->composition == 0x570A) ||
       (object->visual == 0x0009 && object->composition == 0x5716) ||
       (object->visual == 0x000A && object->composition == 0x5729)))
    return kActionEffectPhase_BossLightningImpact;

  return kActionEffectPhase_None;
}

static bool SceneFrameAppend(ActionSceneEffectFrame *dst,
                             const ActionEffectInstance *effect) {
  if (!dst || !effect) return false;
  if (dst->effect_count >= kActionSceneEffectMaxInstances) {
    dst->overflow = 1;
    return false;
  }
  dst->effects[dst->effect_count++] = *effect;
  if (effect->flags & kActionEffectFlag_Visible) dst->visible_count++;
  return true;
}

static void CaptureBloodpoolTorches(ActionSceneEffectFrame *dst,
                                    const uint8_t *wram,
                                    size_t wram_size) {
  if (!dst || !wram ||
      Read8(wram, wram_size, kActRaiserWram_MapGroup) !=
          kActRaiserMapGroup_Bloodpool)
    return;

  ActionBgMapView map;
  if (!ActionBgMapView_Init(
          &map, wram, wram_size,
          Read16(wram, wram_size, kActRaiserWram_Bg1Width),
          Read16(wram, wram_size, kActRaiserWram_Bg1Height),
          Read16(wram, wram_size, kActRaiserWram_BgMapPage)))
    return;
  for (unsigned y = 0; y + kActionBgMetatilePixels < map.world_height;
       y += kActionBgMetatilePixels) {
    for (unsigned x = 0; x < map.world_width;
         x += kActionBgMetatilePixels) {
      uint8_t top = 0, bottom = 0;
      if (!ActionBgMapView_LookupMetatile(&map, (int)x, (int)y, &top) ||
          top != kBloodpoolTorchTopMetatile ||
          !ActionBgMapView_LookupMetatile(
              &map, (int)x, (int)(y + kActionBgMetatilePixels), &bottom) ||
          bottom != kBloodpoolTorchBottomMetatile)
        continue;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      /* Every instance uses the same animated BG tiles, so its source flame
       * changes on one shared game clock. Keep lifecycle time authentic and
       * synchronized here; the renderer owns the deliberately faster visual
       * response used by the added light and embers. */
      const uint16_t clock = dst->game_frame;
      ActionEffectInstance effect = {
        .generation = 0x54000000u ^ identity,
        .pulse_generation = 0x74000000u ^ identity,
        .world_x = (int16_t)(x + 8),
        .world_y = (int16_t)(y + 15),
        .left_extent = 5,
        .top_extent = 9,
        .right_extent = 5,
        .bottom_extent = 2,
        .age_ticks = clock,
        .phase_ticks = clock,
        .pulse_ticks = clock,
        .kind = kActionEffect_WallTorch,
        .phase = kActionEffectPhase_WallTorch,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-5.0f, -9.0f, 5.0f, 2.0f},
        },
      };
      SceneFrameAppend(dst, &effect);
    }
  }
}

static void PopulateSceneObjectEffect(ActionEffectInstance *effect,
                                      uint16_t address,
                                      const ActionObjectSnapshot *object,
                                      uint8_t kind, uint8_t phase) {
  if (!effect || !object) return;
  *effect = (ActionEffectInstance) {
    .record_address = address,
    .world_x = object->world_x,
    .world_y = object->world_y,
    .velocity_x = object->velocity_x,
    .velocity_y = object->velocity_y,
    .left_extent = object->left_extent,
    .top_extent = object->top_extent,
    .right_extent = object->right_extent,
    .bottom_extent = object->bottom_extent,
    .composition = object->composition,
    .visual = object->visual,
    .animation_state = object->animation_state,
    .animation_index = object->animation_index,
    .flip_attributes = object->flip_attributes,
    .kind = kind,
    .phase = phase,
    .role = kActionEffectRole_Body,
    .obj_priority = 0,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane = kActionEffectProjectionPlane_Obj,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {
        -(float)object->left_extent,
        -(float)object->top_extent,
        (float)object->right_extent,
        (float)object->bottom_extent,
      },
    },
  };
  if (ActionObjectVisible(object)) effect->flags |= kActionEffectFlag_Visible;
  if (object->flip_attributes & kActRaiserObjectFlip_Horizontal)
    effect->flags |= kActionEffectFlag_FlipHorizontal;
  if (object->flip_attributes & kActRaiserObjectFlip_Vertical)
    effect->flags |= kActionEffectFlag_FlipVertical;
}

void ActionSceneEffects_CaptureFrame(ActionEffectObserver *observer,
                                     ActionSceneEffectFrame *dst,
                                     const uint8_t *wram, size_t wram_size,
                                     unsigned elapsed_ticks) {
  if (!dst) return;
  memset(dst, 0, sizeof(*dst));
  if (!observer) return;
  if (!observer->next_generation || !observer->next_pulse_generation)
    ActionEffectObserver_Reset(observer);
  if (wram && wram_size > kActRaiserWram_GameFrame + 1)
    dst->game_frame = Read16(wram, wram_size, kActRaiserWram_GameFrame);
  if (!IsActionMap(wram, wram_size)) {
    RetireSceneAll(observer);
    return;
  }

  CaptureBloodpoolTorches(dst, wram, wram_size);
  const bool boss_lightning_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Bloodpool &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
          kBloodpoolBossMap;
  bool seen[kActionSceneEffectObserverTrackCount] = {false};
  for (unsigned slot = 0; slot < kActionSceneEffectObserverTrackCount;
       slot++) {
    const uint16_t address = (uint16_t)(kActRaiserWram_ActionObjectTable +
        slot * kActRaiserActionObjectStride);
    ActionObjectSnapshot object;
    if (!ReadActionObject(wram, wram_size, address, &object) ||
        (object.status & kActRaiserObjectStatus_InactiveMask) ||
        !object.composition)
      continue;

    uint8_t kind = kActionEffect_None;
    uint8_t phase = kActionEffectPhase_None;
    if (IsEnemyFireball(&object)) {
      kind = kActionEffect_EnemyFireball;
      phase = kActionEffectPhase_EnemyFireballFlight;
    } else if (IsSwordBeam(wram, wram_size, &object)) {
      kind = kActionEffect_SwordBeam;
      phase = kActionEffectPhase_SwordBeamFlight;
    } else if (IsLightningTrap(&object)) {
      kind = kActionEffect_LightningTrap;
      phase = kActionEffectPhase_LightningActive;
    } else if (boss_lightning_map &&
               (phase = MatchBloodpoolBossLightning(
                    wram, wram_size, &object)) !=
                   kActionEffectPhase_None) {
      kind = kActionEffect_BloodpoolBossLightning;
    } else {
      continue;
    }

    seen[slot] = true;
    ActionEffectInstance effect;
    PopulateSceneObjectEffect(&effect, address, &object, kind, phase);
    if (kind == kActionEffect_SwordBeam) {
      /* Both `$06:8000` projectile compositions emit the same six 8x8
       * crescent parts over local x=[0,16], y=[-1,31]. Their collision header
       * deliberately contains signed offsets (for example left=$FFE0), so it
       * is not a drawable bounding box and must not position the light. */
      effect.geometry.data.rect =
          (ActionEffectLocalRect){0.0f, -1.0f, 16.0f, 31.0f};
    }
    /* Animation index advances inside one projectile/strike lifecycle. It is
     * artwork cadence, not a new emission pulse; using it as pulse_key would
     * reseed every spark whenever the source sprite changed frame. */
    BeginOrAdvanceSceneTrack(observer, &observer->scene_tracks[slot], &object,
                             kind, phase, elapsed_ticks, &effect);
    SceneFrameAppend(dst, &effect);
  }
  for (unsigned i = 0; i < kActionSceneEffectObserverTrackCount; i++)
    if (!seen[i])
      memset(&observer->scene_tracks[i], 0,
             sizeof(observer->scene_tracks[i]));

  if (dst->overflow) {
    dst->effect_count = 0;
    dst->visible_count = 0;
  }
}
