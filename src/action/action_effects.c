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
  observer->scene_clock_valid = 0;
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
  /* Resume/source are stable for the original projectile and trap families.
   * Fireball's handler is stable too and strengthens its identity; trap
   * lightning omits it because one live bolt transitions between $BD36 and
   * the generic timed animation handler $8683 without becoming a new actor.
   * Marahna's orb and split children share a source but retain distinct
   * resume values, so source+resume is their lifecycle key. Linked lightning
   * and the Bloodpool boss child use their validated source/backlink pair. */
  uint32_t continuity_key = (uint32_t)object->source_descriptor |
      ((uint32_t)object->resume_address << 16);
  if (kind == kActionEffect_EnemyFireball)
    continuity_key ^= (uint32_t)object->handler * 0x9E3779B9u;
  else if (kind == kActionEffect_MarahnaFireball)
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->resume_address << 16);
  else if (kind == kActionEffect_SwordBeam) {
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->spawner_backlink << 16);
    if (object->animation_bank == 0x7E &&
        object->animation_address == 0x5000)
      continuity_key ^= (uint32_t)object->local_counter * 0x9E3779B9u;
  }
  else if (kind == kActionEffect_BloodpoolBossLightning ||
           kind == kActionEffect_MarahnaLightningLink ||
           kind == kActionEffect_MarahnaBossLightning)
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
 * These rules come from runs 20260810-124203 through -190729 and Marahna run
 * 20260811-151353. They deliberately combine control-flow identity with
 * animation/composition identity: object fields are polymorphic in ActRaiser,
 * so matching a visual number or palette colour by itself would eventually
 * decorate an unrelated actor.
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
 * Aitos boss sword volley (run 20260812-000613 snap_05_gf21056):
 *   source $D646 emits two diagonal $7E:5000 crescents through an inactive
 *   state-$00 volley controller. Children retain resume $A65D and exact
 *   state/visual/composition/velocity tuples $01/$21/$56D8/(-3,+1) and
 *   $02/$20/$56BE/(-3,-1). Their captured OAM uses OBJ priority 2.
 * Bloodpool wall torches (snap_01_gf2479, snap_06_gf7654):
 *   BG1 metatile $47 immediately above $4F in maps $02/$03 and $02/$05.
 *   The exact pair is the authored object identity and applies across the
 *   Bloodpool group rather than being tied to either observed room number.
 * Marahna (runs 20260811-151353 and -221433, maps $05/$04-$08):
 *   one BG1 metatile $43 is the complete wall torch. Source $E047 emits a
 *   large four-frame $05-$08 orb, then four backlink-linked $32/$4BCD or
 *   $33/$4BD9 children with exact cardinal velocities and direction flips.
 *   Source $DE96 emits the snake enemy's separate $1D/$1E fire shot. $4AA1/$4B82 are
 *   ten-part
 *   horizontal/vertical lightning links whose parent, partner, and midpoint
 *   are validated before capture. The visually fiery $34/$4BE5 family is
 *   moving-platform machinery and must not receive a projectile effect. In
 *   the boss room, source $E483 authors exact $57C2/$5868 charge arcs, the
 *   $59DE orb, backlink-validated $5CE0 diagonal launched bolts, and the
 *   complete $5D01/$5D0D/$5D2E ground-charge cycle.
 * Aitos (same run, map $04/$01):
 *   lava pits are BG1 rows $DC, one-or-more $DD, $DE over an equally wide
 *   sequence $DF/$E7; these are the two bubbly rows above the solid-red $F7
 *   fill. Their emitted fireballs share source $CF9E and resume $CFCD;
 *   exact handler/state and visual/composition pairs cover the wait, rising,
 *   and return phases without matching unrelated $7E:4000 actors. Run
 *   20260812-000613 separates launched volcano rocks as the $CEEC/$CF16
 *   family, and maps waterfall-platform splash frames as exact three-row BG1
 *   structures in maps $04/$02-$03. */
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
  kMarahnaFirstEffectMap = 0x04,
  kMarahnaLastEffectMap = 0x07,
  kMarahnaBossMap = 0x08,
  kMarahnaTorchMetatile = 0x43,
  kMarahnaFireballSourceDescriptor = 0xE047,
  kMarahnaFireballHandler = 0x8661,
  kMarahnaFireballOrbResume = 0xE061,
  kMarahnaFireballOrbState = 0x000C,
  kMarahnaFireballSplitResume = 0xA65D,
  kMarahnaFireballSplitParentResume = 0xE0A6,
  kMarahnaSnakeSourceDescriptor = 0xDE96,
  kMarahnaSnakeWaitHandler = 0x8661,
  kMarahnaSnakeRiseHandler = 0xDF3E,
  kMarahnaSnakeFallHandler = 0xDF63,
  kMarahnaSnakeResume = 0xDF34,
  kMarahnaSnakeFireballResume = 0xA65D,
  kMarahnaSnakeFireballState = 0x0006,
  kMarahnaLightningHandler = 0x8683,
  kMarahnaLightningSourceDescriptor = 0xE18E,
  kMarahnaLightningPartnerSourceDescriptor = 0xE254,
  kMarahnaLightningResume = 0xE24F,
  kMarahnaLightningHorizontalState = 0x0027,
  kMarahnaLightningVerticalState = 0x0028,
  kMarahnaBossLightningSourceDescriptor = 0xE483,
  kMarahnaBossLightningHandler = 0x8661,
  kMarahnaBossLightningParentResume = 0xE4E5,
  kMarahnaBossLightningActiveParentResume = 0xE4F4,
  kMarahnaBossLightningGroundParentResume = 0xE4D7,
  kMarahnaBossLightningBoltResume = 0xE578,
  kMarahnaBossLightningGroundChargeResume = 0xE57E,
  kAitosLavaMap = 0x01,
  kAitosLavaLeftMetatile = 0xDC,
  kAitosLavaMiddleMetatile = 0xDD,
  kAitosLavaRightMetatile = 0xDE,
  kAitosLavaFillMetatile = 0xDF,
  kAitosLavaBubbleMetatile = 0xE7,
  kAitosLavaMaxMiddleCells = 6,
  kAitosLavaFireballSourceDescriptor = 0xCF9E,
  kAitosLavaFireballResume = 0xCFCD,
  kAitosMoltenRockSourceDescriptor = 0xCEEC,
  kAitosMoltenRockResume = 0xCF16,
  kAitosMoltenRockHandler = 0x8661,
  kAitosMoltenRockState = 0x0027,
  kAitosBossMap = 0x03,
  kAitosBossSourceDescriptor = 0xD646,
  kAitosBossSwordBeamHandler = 0x8661,
  kAitosBossSwordBeamResume = 0xA65D,
  kAitosBossSwordBeamParentResume = 0xD793,
  kAitosWaterfallFirstMap = 0x02,
  kAitosWaterfallLastMap = 0x03,
  kAitosSplashTopLeft = 0x36,
  kAitosSplashTopMiddle = 0x5E,
  kAitosSplashTopRight = 0x81,
  kAitosSplashBodyLeft = 0x4E,
  kAitosSplashBodyMiddle = 0xF4,
  kAitosSplashBodyRight = 0x4F,
  kAitosSplashDripLeft = 0xF6,
  kAitosSplashDripMiddle = 0xFC,
  kAitosSplashDripRight = 0xFE,
  kAitosSplashMaxCells = 8,
};

static bool IsMarahnaEffectMap(const uint8_t *wram, size_t wram_size) {
  if (!wram ||
      Read8(wram, wram_size, kActRaiserWram_MapGroup) !=
          kActRaiserMapGroup_Marahna)
    return false;
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  return map >= kMarahnaFirstEffectMap && map <= kMarahnaLastEffectMap;
}

static bool IsAitosLavaMap(const uint8_t *wram, size_t wram_size) {
  return wram &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Aitos &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == kAitosLavaMap;
}

static bool IsAitosWaterfallMap(const uint8_t *wram, size_t wram_size) {
  if (!wram ||
      Read8(wram, wram_size, kActRaiserWram_MapGroup) !=
          kActRaiserMapGroup_Aitos)
    return false;
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  return map >= kAitosWaterfallFirstMap && map <= kAitosWaterfallLastMap;
}

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

typedef struct MarahnaFireballSplitLifecycle {
  int16_t velocity_x, velocity_y;
  uint16_t state, visual, composition, flips;
} MarahnaFireballSplitLifecycle;

typedef struct MarahnaFireballOrbLifecycle {
  int16_t velocity_x, velocity_y;
  uint16_t visual, composition;
} MarahnaFireballOrbLifecycle;

static bool ActionObjectAddressIsValid(uint16_t address);

static bool MarahnaFireballSplitParentIsValid(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object || !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  return ReadActionObject(wram, wram_size, object->spawner_backlink,
                          &parent) &&
      (parent.status & kActRaiserObjectStatus_InactiveMask) &&
      parent.source_descriptor == kMarahnaFireballSourceDescriptor &&
      parent.handler == kMarahnaFireballHandler &&
      parent.animation_address == kSceneAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank &&
      parent.resume_address == kMarahnaFireballSplitParentResume &&
      parent.animation_state == 0x000E && parent.visual == 0x000C &&
      parent.composition == 0x4597 &&
      parent.left_extent == 8 && parent.top_extent == 8 &&
      parent.right_extent == 8 && parent.bottom_extent == 8 &&
      !parent.spawner_backlink;
}

static bool MarahnaSnakeFireballParentIsValid(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object || !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  if (!ReadActionObject(wram, wram_size, object->spawner_backlink, &parent) ||
      (parent.status & kActRaiserObjectStatus_InactiveMask) ||
      parent.source_descriptor != kMarahnaSnakeSourceDescriptor ||
      parent.animation_address != kSceneAnimationAddress ||
      parent.animation_bank != kSceneAnimationBank ||
      parent.left_extent != 16 || parent.top_extent != 24 ||
      parent.right_extent != 16 || parent.bottom_extent != 24 ||
      (parent.flip_attributes & kActRaiserObjectFlip_Vertical) ||
      (parent.flip_attributes & kActRaiserObjectFlip_Mask) !=
          (object->flip_attributes & kActRaiserObjectFlip_Mask) ||
      parent.spawner_backlink)
    return false;
  static const struct {
    uint16_t handler, resume, state, visual, composition;
  } kLifecycle[] = {
    {kMarahnaSnakeWaitHandler, kMarahnaSnakeResume,
     0x0005, 0x0000, 0x4435},
    {kMarahnaSnakeRiseHandler, kMarahnaSnakeResume,
     0x0003, 0x0001, 0x4464},
    {kMarahnaSnakeFallHandler, kMarahnaSnakeResume,
     0x0004, 0x0001, 0x4464},
  };
  for (size_t i = 0; i < sizeof(kLifecycle) / sizeof(kLifecycle[0]); i++)
    if (parent.handler == kLifecycle[i].handler &&
        parent.resume_address == kLifecycle[i].resume &&
        parent.animation_state == kLifecycle[i].state &&
        parent.visual == kLifecycle[i].visual &&
        parent.composition == kLifecycle[i].composition)
      return true;
  return false;
}

static uint8_t MatchMarahnaSnakeFireballShot(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object ||
      object->source_descriptor != kMarahnaSnakeSourceDescriptor ||
      object->handler != kMarahnaSnakeWaitHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kMarahnaSnakeFireballState ||
      object->resume_address != kMarahnaSnakeFireballResume ||
      object->left_extent != 8 || object->top_extent != 4 ||
      object->right_extent != 8 || object->bottom_extent != 4 ||
      object->velocity_y != 0 || object->local_counter != 6 ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical) ||
      !MarahnaSnakeFireballParentIsValid(wram, wram_size, object))
    return kActionEffectPhase_None;
  const bool horizontal_flip =
      (object->flip_attributes & kActRaiserObjectFlip_Horizontal) != 0;
  if (object->velocity_x != (horizontal_flip ? 4 : -4))
    return kActionEffectPhase_None;
  if ((object->visual == 0x001D && object->composition == 0x4869) ||
      (object->visual == 0x001E && object->composition == 0x487C))
    return kActionEffectPhase_MarahnaSnakeFireballShot;
  return kActionEffectPhase_None;
}

static uint8_t MatchMarahnaFireball(const uint8_t *wram, size_t wram_size,
                                    const ActionObjectSnapshot *object) {
  if (!object) return kActionEffectPhase_None;
  if (object->source_descriptor == kMarahnaSnakeSourceDescriptor)
    return MatchMarahnaSnakeFireballShot(wram, wram_size, object);
  if (object->source_descriptor != kMarahnaFireballSourceDescriptor ||
      object->handler != kMarahnaFireballHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank)
    return kActionEffectPhase_None;

  static const MarahnaFireballOrbLifecycle kOrb[] = {
    { 0, 0, 0x0007, 0x451C},
    {-1, 0, 0x0008, 0x4528},
    {-2, 0, 0x0008, 0x4528},
    { 0, 0, 0x0005, 0x4504},
    { 1, 0, 0x0006, 0x4510},
    { 2, 0, 0x0006, 0x4510},
  };
  if (object->resume_address == kMarahnaFireballOrbResume &&
      object->animation_state == kMarahnaFireballOrbState &&
      object->left_extent == 8 && object->top_extent == 8 &&
      object->right_extent == 8 && object->bottom_extent == 8 &&
      !(object->flip_attributes & kActRaiserObjectFlip_Mask)) {
    for (size_t i = 0; i < sizeof(kOrb) / sizeof(kOrb[0]); i++)
      if (object->velocity_x == kOrb[i].velocity_x &&
          object->velocity_y == kOrb[i].velocity_y &&
          object->visual == kOrb[i].visual &&
          object->composition == kOrb[i].composition)
        return kActionEffectPhase_MarahnaFireballOrb;
  }

  static const MarahnaFireballSplitLifecycle kSplit[] = {
    { 0,  3, 0x000F, 0x0032, 0x4BCD, 0x0000},
    {-3,  0, 0x0010, 0x0033, 0x4BD9, 0x0000},
    { 0, -3, 0x000F, 0x0032, 0x4BCD,
      kActRaiserObjectFlip_Vertical},
    { 3,  0, 0x0010, 0x0033, 0x4BD9,
      kActRaiserObjectFlip_Horizontal},
  };
  if (object->resume_address != kMarahnaFireballSplitResume ||
      object->left_extent != 4 || object->top_extent != 4 ||
      object->right_extent != 4 || object->bottom_extent != 4 ||
      !MarahnaFireballSplitParentIsValid(wram, wram_size, object))
    return kActionEffectPhase_None;
  for (size_t i = 0; i < sizeof(kSplit) / sizeof(kSplit[0]); i++)
    if (object->velocity_x == kSplit[i].velocity_x &&
        object->velocity_y == kSplit[i].velocity_y &&
        object->animation_state == kSplit[i].state &&
        object->visual == kSplit[i].visual &&
        object->composition == kSplit[i].composition &&
        (object->flip_attributes & kActRaiserObjectFlip_Mask) ==
            kSplit[i].flips)
      return kActionEffectPhase_MarahnaFireballSplit;
  return kActionEffectPhase_None;
}

typedef struct AitosLavaFireballLifecycle {
  uint16_t state;
  uint16_t handler;
  int16_t velocity_x;
  int16_t velocity_y;
} AitosLavaFireballLifecycle;

static bool IsAitosLavaFireball(const ActionObjectSnapshot *object) {
  static const AitosLavaFireballLifecycle kLifecycle[] = {
    {0x0022, 0xCFE3,  0, -4},
    {0x0023, 0x8661,  0,  0},
    {0x0024, 0xCFFE, -1,  6},
  };
  if (!object ||
      object->source_descriptor != kAitosLavaFireballSourceDescriptor ||
      object->resume_address != kAitosLavaFireballResume ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->left_extent != 0x08 || object->top_extent != 0x08 ||
      object->right_extent != 0x08 || object->bottom_extent != 0x08 ||
      (object->flip_attributes & kActRaiserObjectFlip_Mask))
    return false;
  const bool artwork =
      (object->visual == 0x002A && object->composition == 0x4D21) ||
      (object->visual == 0x002B && object->composition == 0x4D2D);
  if (!artwork) return false;
  for (size_t i = 0; i < sizeof(kLifecycle) / sizeof(kLifecycle[0]); i++)
    if (object->animation_state == kLifecycle[i].state &&
        object->handler == kLifecycle[i].handler &&
        object->velocity_x == kLifecycle[i].velocity_x &&
        object->velocity_y == kLifecycle[i].velocity_y)
      return true;
  return false;
}

static bool IsAitosMoltenRock(const ActionObjectSnapshot *object) {
  if (!object ||
      object->source_descriptor != kAitosMoltenRockSourceDescriptor ||
      object->resume_address != kAitosMoltenRockResume ||
      object->handler != kAitosMoltenRockHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kAitosMoltenRockState ||
      object->visual != 0x002B || object->composition != 0x4D2D ||
      object->left_extent != 8 || object->top_extent != 8 ||
      object->right_extent != 8 || object->bottom_extent != 8 ||
      (object->velocity_x != -2 && object->velocity_x != 2) ||
      object->velocity_y < -1 || object->velocity_y > 1)
    return false;
  const uint16_t flips =
      object->flip_attributes & kActRaiserObjectFlip_Mask;
  return object->velocity_x < 0 ? flips == 0
                                : flips == kActRaiserObjectFlip_Horizontal;
}

static bool ActionObjectAddressIsValid(uint16_t address) {
  const unsigned table_start = kActRaiserWram_ActionObjectTable;
  const unsigned table_end = table_start +
      kActRaiserActionObjectCount * kActRaiserActionObjectStride;
  return address >= table_start && address < table_end &&
      (address - table_start) % kActRaiserActionObjectStride == 0;
}

static bool MarahnaLightningEndpointMatches(
    const ActionObjectSnapshot *object, bool partner, bool vertical) {
  if (!object || (object->status & kActRaiserObjectStatus_InactiveMask) ||
      !object->composition || object->handler != kMarahnaLightningHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      (object->flip_attributes & kActRaiserObjectFlip_Mask))
    return false;
  if (partner) {
    return object->source_descriptor ==
               kMarahnaLightningPartnerSourceDescriptor &&
        object->animation_state == 0x001D &&
        object->visual == (vertical ? 0x0010 : 0x000F) &&
        object->composition == (vertical ? 0x45DC : 0x45D0);
  }
  return object->source_descriptor == kMarahnaLightningSourceDescriptor &&
      object->animation_state == 0x001A &&
      object->visual == (vertical ? 0x000E : 0x000D) &&
      object->composition == (vertical ? 0x45C4 : 0x45B8);
}

static bool IsMarahnaLightningLink(const uint8_t *wram, size_t wram_size,
                                   const ActionObjectSnapshot *object) {
  if (!object || object->handler != kMarahnaLightningHandler ||
      object->source_descriptor != kMarahnaLightningSourceDescriptor ||
      object->resume_address != kMarahnaLightningResume ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      (object->flip_attributes & kActRaiserObjectFlip_Mask) ||
      !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;

  const bool horizontal =
      object->animation_state == kMarahnaLightningHorizontalState &&
      object->visual == 0x002E && object->composition == 0x4AA1 &&
      object->left_extent == 40 && object->right_extent == 40 &&
      object->top_extent == 4 && object->bottom_extent == 4;
  const bool vertical =
      object->animation_state == kMarahnaLightningVerticalState &&
      object->visual == 0x0031 && object->composition == 0x4B82 &&
      object->left_extent == 5 && object->right_extent == 5 &&
      object->top_extent == 40 && object->bottom_extent == 40;
  if (!horizontal && !vertical) return false;

  const unsigned partner_address =
      (unsigned)object->spawner_backlink + kActRaiserActionObjectStride;
  if (partner_address > UINT16_MAX ||
      !ActionObjectAddressIsValid((uint16_t)partner_address))
    return false;
  ActionObjectSnapshot parent, partner;
  if (!ReadActionObject(wram, wram_size, object->spawner_backlink, &parent) ||
      !ReadActionObject(wram, wram_size, (uint16_t)partner_address,
                        &partner) ||
      !MarahnaLightningEndpointMatches(&parent, false, vertical) ||
      !MarahnaLightningEndpointMatches(&partner, true, vertical))
    return false;
  return (int32_t)object->world_x * 2 ==
             (int32_t)parent.world_x + partner.world_x &&
      (int32_t)object->world_y * 2 ==
             (int32_t)parent.world_y + partner.world_y;
}

static bool MarahnaBossParentMatches(const ActionObjectSnapshot *object) {
  if (!object || (object->status & kActRaiserObjectStatus_InactiveMask) ||
      !object->composition ||
      object->source_descriptor != kMarahnaBossLightningSourceDescriptor ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->left_extent != 48 || object->top_extent != 40 ||
      object->right_extent != 48 || object->bottom_extent != 8 ||
      object->spawner_backlink ||
      (object->flip_attributes & kActRaiserObjectFlip_Mask))
    return false;
  return (object->handler == kMarahnaBossLightningHandler &&
          object->animation_state == 0x0000 &&
          object->resume_address == kMarahnaBossLightningParentResume) ||
      (object->handler == kMarahnaBossLightningHandler &&
       object->animation_state == 0x0001 &&
       object->resume_address == kMarahnaBossLightningActiveParentResume) ||
      (object->handler == kAnimationRepeatHandler &&
       object->animation_state == 0x000A && object->visual == 0x0000 &&
       object->composition == 0x5307 &&
       object->resume_address == kMarahnaBossLightningGroundParentResume);
}

static uint8_t MatchMarahnaBossLightning(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object ||
      object->source_descriptor != kMarahnaBossLightningSourceDescriptor ||
      object->handler != kMarahnaBossLightningHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank)
    return kActionEffectPhase_None;

  if (MarahnaBossParentMatches(object)) {
    if ((object->visual == 0x0007 && object->composition == 0x57C2) ||
        (object->visual == 0x0008 && object->composition == 0x5868))
      return kActionEffectPhase_MarahnaBossLightningCharge;
    if (object->visual == 0x000A && object->composition == 0x59DE)
      return kActionEffectPhase_MarahnaBossLightningOrb;
    return kActionEffectPhase_None;
  }

  uint8_t phase = kActionEffectPhase_None;
  const uint16_t flips =
      object->flip_attributes & kActRaiserObjectFlip_Mask;
  if (object->resume_address == kMarahnaBossLightningBoltResume &&
      object->animation_state == 0x0004 && object->visual == 0x0011 &&
      object->composition == 0x5CE0 && object->velocity_y == 4 &&
      !object->top_extent && object->bottom_extent == 32) {
    const bool left = object->velocity_x == -4 &&
        object->left_extent == 32 && !object->right_extent && !flips;
    const bool right = object->velocity_x == 4 &&
        !object->left_extent && object->right_extent == 32 &&
        flips == kActRaiserObjectFlip_Horizontal;
    if (left || right)
      phase = kActionEffectPhase_MarahnaBossLightningBolt;
  } else if (
      object->resume_address == kMarahnaBossLightningGroundChargeResume &&
      object->animation_state == 0x0007 && !object->velocity_y) {
    static const struct {
      uint16_t visual, composition, extent;
    } kGroundChargeFrames[] = {
      {0x0012, 0x5D01, 8},
      {0x0013, 0x5D0D, 16},
      {0x0014, 0x5D2E, 16},
    };
    bool artwork_matches = false;
    for (size_t i = 0;
         i < sizeof(kGroundChargeFrames) / sizeof(kGroundChargeFrames[0]);
         i++) {
      const uint16_t extent = kGroundChargeFrames[i].extent;
      if (object->visual == kGroundChargeFrames[i].visual &&
          object->composition == kGroundChargeFrames[i].composition &&
          object->left_extent == extent && object->top_extent == extent &&
          object->right_extent == extent && object->bottom_extent == extent) {
        artwork_matches = true;
        break;
      }
    }
    const bool left = object->velocity_x == -4 && !flips;
    const bool right = object->velocity_x == 4 &&
        flips == kActRaiserObjectFlip_Horizontal;
    if (artwork_matches && (left || right))
      phase = kActionEffectPhase_MarahnaBossLightningGroundCharge;
  }
  if (phase == kActionEffectPhase_None ||
      !ActionObjectAddressIsValid(object->spawner_backlink))
    return kActionEffectPhase_None;

  ActionObjectSnapshot parent;
  if (!ReadActionObject(wram, wram_size, object->spawner_backlink, &parent) ||
      !MarahnaBossParentMatches(&parent))
    return kActionEffectPhase_None;
  const bool post_impact_parent =
      parent.handler == kAnimationRepeatHandler &&
      parent.animation_state == 0x000A &&
      parent.resume_address == kMarahnaBossLightningGroundParentResume;
  if ((phase == kActionEffectPhase_MarahnaBossLightningGroundCharge) !=
      post_impact_parent)
    return kActionEffectPhase_None;
  return phase;
}

static bool PlayerSwordBeamParentIsValid(
    const uint8_t *wram, size_t wram_size,
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

static bool IsPlayerSwordBeam(const uint8_t *wram, size_t wram_size,
                              const ActionObjectSnapshot *object) {
  if (!object || object->handler != kSwordBeamHandler ||
      object->animation_address != kSwordBeamAnimationAddress ||
      object->animation_bank != kSwordBeamAnimationBank ||
      !object->source_descriptor ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical) ||
      !(object->flags & kActRaiserObjectFlag_Attacker) ||
      !PlayerSwordBeamParentIsValid(wram, wram_size, object))
    return false;
  return (object->animation_state == kSwordBeamHorizontalState &&
          object->visual == 0x0030 && object->composition == 0x99E8) ||
      (object->animation_state == kSwordBeamAlternateState &&
       object->visual == 0x0031 && object->composition == 0x9A17);
}

static bool AitosBossSwordBeamParentIsValid(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object || !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  if (!ReadActionObject(wram, wram_size, object->spawner_backlink, &parent) ||
      parent.status != 0x4000 ||
      parent.source_descriptor != kAitosBossSourceDescriptor ||
      parent.handler != kAitosBossSwordBeamHandler ||
      parent.animation_address != kBossAnimationAddress ||
      parent.animation_bank != kSceneAnimationBank ||
      parent.resume_address != kAitosBossSwordBeamParentResume ||
      parent.animation_state != 0x0000 || parent.visual != 0x0023 ||
      parent.composition != 0x56FE || parent.flip_attributes != 0 ||
      parent.left_extent != 8 || parent.top_extent != 8 ||
      parent.right_extent != 8 || parent.bottom_extent != 8 ||
      parent.flags != 0x0020 || parent.local_counter != 0x000D ||
      !ActionObjectAddressIsValid(parent.spawner_backlink))
    return false;

  ActionObjectSnapshot boss;
  return ReadActionObject(wram, wram_size, parent.spawner_backlink, &boss) &&
      !(boss.status & kActRaiserObjectStatus_InactiveMask) &&
      boss.composition &&
      boss.source_descriptor == kAitosBossSourceDescriptor &&
      boss.animation_address == kBossAnimationAddress &&
      boss.animation_bank == kSceneAnimationBank &&
      boss.spawner_backlink == 0 && (boss.flags & 0x4000);
}

static bool IsAitosBossSwordBeam(const uint8_t *wram, size_t wram_size,
                                 const ActionObjectSnapshot *object) {
  if (!object ||
      object->source_descriptor != kAitosBossSourceDescriptor ||
      object->handler != kAitosBossSwordBeamHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->resume_address != kAitosBossSwordBeamResume ||
      object->animation_index != 0x0001 || object->flip_attributes != 0 ||
      object->flags != 0x0020 ||
      !AitosBossSwordBeamParentIsValid(wram, wram_size, object))
    return false;

  static const struct {
    uint16_t state, visual, composition, local_counter;
    int16_t velocity_y;
    uint16_t top_extent, bottom_extent;
  } kCrescents[] = {
    {0x0001, 0x0021, 0x56D8, 0x0001, 1, 16, 8},
    {0x0002, 0x0020, 0x56BE, 0x0002, -1, 8, 16},
  };
  for (unsigned i = 0; i < sizeof(kCrescents) / sizeof(kCrescents[0]); i++)
    if (object->animation_state == kCrescents[i].state &&
        object->visual == kCrescents[i].visual &&
        object->composition == kCrescents[i].composition &&
        object->local_counter == kCrescents[i].local_counter &&
        object->velocity_x == -3 &&
        object->velocity_y == kCrescents[i].velocity_y &&
        object->left_extent == 8 && object->right_extent == 16 &&
        object->top_extent == kCrescents[i].top_extent &&
        object->bottom_extent == kCrescents[i].bottom_extent)
      return true;
  return false;
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

static bool SceneDecorationAppend(ActionSceneEffectFrame *dst,
                                  const ActionEffectInstance *effect) {
  if (!dst || !effect) return false;
  if (dst->decoration_count >= kActionSceneDecorationMaxInstances) {
    dst->decoration_overflow = 1;
    return false;
  }
  dst->decorations[dst->decoration_count++] = *effect;
  if (effect->flags & kActionEffectFlag_Visible)
    dst->decoration_visible_count++;
  return true;
}

typedef struct WallTorchMapRule {
  uint8_t top_metatile;
  uint8_t bottom_metatile;
  int8_t anchor_y;
  int8_t bottom_extent;
  bool requires_bottom;
  bool camera_bounded;
} WallTorchMapRule;

static bool WallTorchRuleFor(const uint8_t *wram, size_t wram_size,
                             WallTorchMapRule *rule) {
  if (!wram || !rule) return false;
  const uint8_t group =
      Read8(wram, wram_size, kActRaiserWram_MapGroup);
  if (group == kActRaiserMapGroup_Bloodpool) {
    *rule = (WallTorchMapRule) {
      .top_metatile = kBloodpoolTorchTopMetatile,
      .bottom_metatile = kBloodpoolTorchBottomMetatile,
      .anchor_y = 15,
      .bottom_extent = 2,
      .requires_bottom = true,
    };
    return true;
  }
  if (IsMarahnaEffectMap(wram, wram_size) ||
      (Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
           kActRaiserMapGroup_Marahna &&
       Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
           kMarahnaBossMap)) {
    *rule = (WallTorchMapRule) {
      .top_metatile = kMarahnaTorchMetatile,
      .anchor_y = 11,
      .bottom_extent = 5,
      .camera_bounded = true,
    };
    return true;
  }
  return false;
}

typedef struct SceneBgScanBounds {
  unsigned x0, y0;
  unsigned x1, y1;  /* exclusive */
} SceneBgScanBounds;

static bool SceneBgScanBounds_InitWindow(
    SceneBgScanBounds *bounds, const ActionBgMapView *map,
    int camera_x, int camera_y, int margin_x, int margin_y,
    bool include_partial_cells) {
  if (!bounds || !map || !map->world_width || !map->world_height ||
      margin_x < 0 || margin_y < 0)
    return false;
  int min_x = camera_x - margin_x;
  int min_y = camera_y - margin_y;
  int max_x = camera_x + kActRaiserAuthenticWidth + margin_x;
  int max_y = camera_y + kActRaiserAuthenticHeight + margin_y;
  if (max_x < 0 || max_y < 0 || min_x >= (int)map->world_width ||
      min_y >= (int)map->world_height)
    return false;
  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x >= (int)map->world_width) max_x = (int)map->world_width - 1;
  if (max_y >= (int)map->world_height) max_y = (int)map->world_height - 1;
  const unsigned cell = kActionBgMetatilePixels;
  const unsigned align_bias = include_partial_cells ? 0u : cell - 1u;
  bounds->x0 = ((unsigned)min_x + align_bias) / cell * cell;
  bounds->y0 = ((unsigned)min_y + align_bias) / cell * cell;
  bounds->x1 = ((unsigned)max_x / cell + 1u) * cell;
  bounds->y1 = ((unsigned)max_y / cell + 1u) * cell;
  if (bounds->x1 > map->world_width) bounds->x1 = map->world_width;
  if (bounds->y1 > map->world_height) bounds->y1 = map->world_height;
  return bounds->x0 < bounds->x1 && bounds->y0 < bounds->y1;
}

static bool SceneBgScanBounds_Init(SceneBgScanBounds *bounds,
                                   const ActionBgMapView *map,
                                   bool camera_bounded,
                                   int camera_x, int camera_y) {
  if (!bounds || !map || !map->world_width || !map->world_height)
    return false;
  if (!camera_bounded) {
    *bounds = (SceneBgScanBounds){
      .x1 = map->world_width,
      .y1 = map->world_height,
    };
    return true;
  }
  return SceneBgScanBounds_InitWindow(
      bounds, map, camera_x, camera_y,
      kActRaiserAuthenticWidth, kActRaiserAuthenticWidth, false);
}

static void CaptureWallTorches(ActionSceneEffectFrame *dst,
                               const uint8_t *wram,
                               size_t wram_size, uint16_t clock) {
  WallTorchMapRule rule;
  if (!dst || !WallTorchRuleFor(wram, wram_size, &rule)) return;

  ActionBgMapView map;
  if (!ActionBgMapView_Init(
          &map, wram, wram_size,
          Read16(wram, wram_size, kActRaiserWram_Bg1Width),
          Read16(wram, wram_size, kActRaiserWram_Bg1Height),
          Read16(wram, wram_size, kActRaiserWram_BgMapPage)))
    return;
  const int camera_x = Read16(wram, wram_size, kActRaiserWram_Bg1CameraX);
  const int camera_y = Read16(wram, wram_size, kActRaiserWram_Bg1CameraY);
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_Init(&bounds, &map, rule.camera_bounded,
                              camera_x, camera_y))
    return;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      uint8_t top = 0, bottom = 0;
      if (!ActionBgMapView_LookupMetatile(&map, (int)x, (int)y, &top) ||
          top != rule.top_metatile)
        continue;
      if (rule.requires_bottom &&
          (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)(y + kActionBgMetatilePixels), &bottom) ||
           bottom != rule.bottom_metatile))
        continue;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      /* Every instance uses the same animated BG tiles, so its source flame
       * changes on one shared gameplay clock. The renderer owns the
       * deliberately faster visual response used by the added light and
       * embers; the observer clock keeps that response frozen with the source
       * BG throughout ActRaiser's native pause. */
      ActionEffectInstance effect = {
        .generation = 0x54000000u ^ identity,
        .pulse_generation = 0x74000000u ^ identity,
        .world_x = (int16_t)(x + 8),
        .world_y = (int16_t)(y + rule.anchor_y),
        .left_extent = 5,
        .top_extent = 9,
        .right_extent = 5,
        .bottom_extent = (uint16_t)rule.bottom_extent,
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
          .data.rect = {-5.0f, -9.0f, 5.0f,
                        (float)rule.bottom_extent},
        },
      };
      SceneDecorationAppend(dst, &effect);
    }
  }
}

static void CaptureAitosLavaPits(ActionSceneEffectFrame *dst,
                                 const uint8_t *wram,
                                 size_t wram_size, uint16_t clock) {
  if (!dst || !IsAitosLavaMap(wram, wram_size)) return;
  ActionBgMapView map;
  if (!ActionBgMapView_Init(
          &map, wram, wram_size,
          Read16(wram, wram_size, kActRaiserWram_Bg1Width),
          Read16(wram, wram_size, kActRaiserWram_Bg1Height),
          Read16(wram, wram_size, kActRaiserWram_BgMapPage)))
    return;
  const int camera_x = Read16(wram, wram_size, kActRaiserWram_Bg1CameraX);
  const int camera_y = Read16(wram, wram_size, kActRaiserWram_Bg1CameraY);
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_Init(&bounds, &map, true, camera_x, camera_y))
    return;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      uint8_t metatile = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != kAitosLavaLeftMetatile)
        continue;

      unsigned middle_cells = 0;
      unsigned total_cells = 0;
      for (unsigned step = 1; step <= kAitosLavaMaxMiddleCells + 1;
           step++) {
        const unsigned cell_x = x + step * kActionBgMetatilePixels;
        if (cell_x < x ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile))
          break;
        if (step <= kAitosLavaMaxMiddleCells &&
            metatile == kAitosLavaMiddleMetatile) {
          middle_cells++;
          continue;
        }
        if (middle_cells && metatile == kAitosLavaRightMetatile)
          total_cells = step + 1;
        break;
      }
      if (!total_cells) continue;

      bool bubbles_valid = true;
      unsigned bubble_rows = 1;
      static const uint8_t kBubbleRows[] = {
        kAitosLavaFillMetatile, kAitosLavaBubbleMetatile,
      };
      const bool has_second_bubble_row =
          y <= map.world_height - 3u * kActionBgMetatilePixels;
      if (has_second_bubble_row) bubble_rows++;
      for (unsigned row = 0; row < bubble_rows; row++) {
        for (unsigned cell = 0; cell < total_cells; cell++) {
          const unsigned cell_x = x + cell * kActionBgMetatilePixels;
          const unsigned cell_y = y + (row + 1u) * kActionBgMetatilePixels;
          if (cell_x < x || cell_y < y ||
              !ActionBgMapView_LookupMetatile(
                  &map, (int)cell_x, (int)cell_y, &metatile) ||
              metatile != kBubbleRows[row]) {
            bubbles_valid = false;
            break;
          }
        }
        if (!bubbles_valid) break;
      }
      if (!bubbles_valid) continue;

      const unsigned width = total_cells * kActionBgMetatilePixels;
      const unsigned height = bubble_rows * kActionBgMetatilePixels;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      const float half_width = (float)width * 0.5f;
      const float half_height = (float)height * 0.5f;
      ActionEffectInstance effect = {
        .generation = 0x4C000000u ^ identity,
        .pulse_generation = 0x6C000000u ^ identity,
        .world_x = (int16_t)(x + width / 2u),
        .world_y = (int16_t)(
            y + kActionBgMetatilePixels + height / 2u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = (uint16_t)(height / 2u),
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = (uint16_t)(height / 2u),
        .age_ticks = clock,
        .phase_ticks = clock,
        .pulse_ticks = clock,
        .kind = kActionEffect_AitosLavaPit,
        .phase = kActionEffectPhase_AitosLavaPit,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -half_height,
                        half_width, half_height},
        },
      };
      SceneDecorationAppend(dst, &effect);
    }
  }
}

static bool AitosSplashStructureWidth(
    const ActionBgMapView *map, unsigned x, unsigned y,
    unsigned *total_cells) {
  if (total_cells) *total_cells = 0;
  if (!map || !total_cells) return false;
  uint8_t tile = 0;
  if (!ActionBgMapView_LookupMetatile(map, (int)x, (int)y, &tile) ||
      tile != kAitosSplashTopLeft)
    return false;
  for (unsigned cells = 2; cells <= kAitosSplashMaxCells; cells++) {
    const unsigned right_x = x + (cells - 1u) * kActionBgMetatilePixels;
    if (right_x < x ||
        !ActionBgMapView_LookupMetatile(
            map, (int)right_x, (int)y, &tile))
      return false;
    if (tile == kAitosSplashTopMiddle) continue;
    if (tile != kAitosSplashTopRight) return false;
    static const uint8_t kLeft[] = {
      kAitosSplashBodyLeft, kAitosSplashDripLeft,
    };
    static const uint8_t kMiddle[] = {
      kAitosSplashBodyMiddle, kAitosSplashDripMiddle,
    };
    static const uint8_t kRight[] = {
      kAitosSplashBodyRight, kAitosSplashDripRight,
    };
    for (unsigned row = 0; row < 2; row++) {
      const unsigned row_y = y + (row + 1u) * kActionBgMetatilePixels;
      for (unsigned cell = 0; cell < cells; cell++) {
        const uint8_t expected = cell == 0 ? kLeft[row]
            : cell + 1u == cells ? kRight[row] : kMiddle[row];
        if (!ActionBgMapView_LookupMetatile(
                map, (int)(x + cell * kActionBgMetatilePixels),
                (int)row_y, &tile) || tile != expected)
          return false;
      }
    }
    *total_cells = cells;
    return true;
  }
  return false;
}

static void CaptureAitosWater(ActionSceneEffectFrame *dst,
                              const uint8_t *wram,
                              size_t wram_size, uint16_t clock) {
  if (!dst || !IsAitosWaterfallMap(wram, wram_size)) return;
  ActionBgMapView map;
  if (!ActionBgMapView_Init(
          &map, wram, wram_size,
          Read16(wram, wram_size, kActRaiserWram_Bg1Width),
          Read16(wram, wram_size, kActRaiserWram_Bg1Height),
          Read16(wram, wram_size, kActRaiserWram_BgMapPage)))
    return;
  const int camera_x = Read16(wram, wram_size, kActRaiserWram_Bg1CameraX);
  const int camera_y = Read16(wram, wram_size, kActRaiserWram_Bg1CameraY);
  SceneBgScanBounds bounds;
  /* Cover the maximum wide side margin and Diorama vertical extension while
   * keeping the immutable scene payload bounded to structures that can
   * actually enter this presentation. */
  if (!SceneBgScanBounds_InitWindow(
          &bounds, &map, camera_x, camera_y, 128, 64, true))
    return;

  unsigned splash_count = 0;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      unsigned cells = 0;
      if (!AitosSplashStructureWidth(&map, x, y, &cells)) continue;
      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = 0x57000000u ^ identity,
        .pulse_generation = 0x77000000u ^ identity,
        .world_x = (int16_t)(x + width / 2u),
        .world_y = (int16_t)(y + 16u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = 16,
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = 16,
        .age_ticks = clock,
        .phase_ticks = clock,
        .pulse_ticks = clock,
        .kind = kActionEffect_AitosWaterSplash,
        .phase = kActionEffectPhase_AitosWaterSplash,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -16.0f, half_width, 16.0f},
        },
      };
      if (SceneDecorationAppend(dst, &effect)) splash_count++;
    }
  }
  if (!splash_count || dst->decoration_overflow) return;

  /* BG2 uses the same decoded 512x512 map in the preceding dark cave, so the
   * camera-local presence of an exact splash structure is the live art
   * discriminator for the waterfall section. One broad BG2 record supplies
   * a restrained flow veil without replacing the source pixels. */
  const int bg2_camera_x =
      Read16(wram, wram_size, kActRaiserWram_Bg2CameraX);
  const int bg2_camera_y =
      Read16(wram, wram_size, kActRaiserWram_Bg2CameraY);
  const uint32_t map_identity =
      Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  ActionEffectInstance waterfall = {
    .generation = 0x57540000u ^ map_identity,
    .pulse_generation = 0x77540000u ^ map_identity,
    .world_x = (int16_t)(bg2_camera_x + 128),
    .world_y = (int16_t)(bg2_camera_y + 112),
    .left_extent = 256,
    .top_extent = 176,
    .right_extent = 256,
    .bottom_extent = 176,
    .age_ticks = clock,
    .phase_ticks = clock,
    .pulse_ticks = clock,
    .kind = kActionEffect_AitosWaterfall,
    .phase = kActionEffectPhase_AitosWaterfallFlow,
    .role = kActionEffectRole_Body,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {-256.0f, -176.0f, 256.0f, 176.0f},
    },
  };
  if (!SceneDecorationAppend(dst, &waterfall)) return;

  /* `$04/$02` intentionally keeps BG2's vertical extension short: allowing
   * more raw-wrap rows repeats water into non-water areas. A separate
   * after-BG2 Diorama record puts foam and mist over the uncovered bottom
   * band. It uses BG2's camera/shape but not its winner pixels; later BG1 and
   * OBJ planes remain in front. */
  ActionEffectInstance mist = waterfall;
  mist.generation = 0x575D0000u ^ map_identity;
  mist.pulse_generation = 0x775D0000u ^ map_identity;
  mist.world_y = (int16_t)(bg2_camera_y + kActRaiserAuthenticHeight - 8);
  mist.top_extent = 32;
  mist.bottom_extent = 24;
  mist.kind = kActionEffect_AitosWaterfallMist;
  mist.phase = kActionEffectPhase_AitosWaterfallMist;
  mist.render_layer = kActionEffectRenderLayer_Atmosphere;
  mist.geometry.data.rect =
      (ActionEffectLocalRect){-256.0f, -32.0f, 256.0f, 24.0f};
  SceneDecorationAppend(dst, &mist);
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

  if (!observer->scene_clock_valid) {
    /* Preserve the established visual phase on entry/load, then decouple it
     * from $0088: that ROM clock keeps moving on the native pause screen. */
    observer->scene_clock = dst->game_frame;
    observer->scene_clock_valid = 1;
  } else {
    observer->scene_clock = (uint16_t)(observer->scene_clock + elapsed_ticks);
  }
  CaptureWallTorches(dst, wram, wram_size, observer->scene_clock);
  CaptureAitosLavaPits(dst, wram, wram_size, observer->scene_clock);
  CaptureAitosWater(dst, wram, wram_size, observer->scene_clock);
  if (dst->decoration_overflow) {
    dst->decoration_count = 0;
    dst->decoration_visible_count = 0;
  }
  const bool marahna_effect_map = IsMarahnaEffectMap(wram, wram_size);
  const bool aitos_lava_map = IsAitosLavaMap(wram, wram_size);
  const bool aitos_boss_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Aitos &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == kAitosBossMap;
  const bool boss_lightning_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Bloodpool &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
          kBloodpoolBossMap;
  const bool marahna_boss_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Marahna &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
          kMarahnaBossMap;
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
    bool aitos_boss_sword_beam = false;
    if (IsEnemyFireball(&object)) {
      kind = kActionEffect_EnemyFireball;
      phase = kActionEffectPhase_EnemyFireballFlight;
    } else if (marahna_effect_map &&
               (phase = MatchMarahnaFireball(
                    wram, wram_size, &object)) !=
                   kActionEffectPhase_None) {
      kind = kActionEffect_MarahnaFireball;
    } else if (marahna_effect_map &&
               IsMarahnaLightningLink(wram, wram_size, &object)) {
      kind = kActionEffect_MarahnaLightningLink;
      phase = kActionEffectPhase_MarahnaLightningActive;
    } else if (marahna_boss_map &&
               (phase = MatchMarahnaBossLightning(
                    wram, wram_size, &object)) !=
                   kActionEffectPhase_None) {
      kind = kActionEffect_MarahnaBossLightning;
    } else if (aitos_lava_map && IsAitosLavaFireball(&object)) {
      kind = kActionEffect_AitosLavaFireball;
      phase = kActionEffectPhase_AitosLavaFireballFlight;
    } else if (aitos_lava_map && IsAitosMoltenRock(&object)) {
      kind = kActionEffect_AitosMoltenRock;
      phase = kActionEffectPhase_AitosMoltenRockFlight;
    } else if (aitos_boss_map &&
               IsAitosBossSwordBeam(wram, wram_size, &object)) {
      kind = kActionEffect_SwordBeam;
      phase = kActionEffectPhase_SwordBeamFlight;
      aitos_boss_sword_beam = true;
    } else if (IsPlayerSwordBeam(wram, wram_size, &object)) {
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
    if (kind == kActionEffect_MarahnaBossLightning &&
        phase == kActionEffectPhase_MarahnaBossLightningBolt) {
      /* The authored bolt owns one asymmetric 32x32 quadrant extending down
       * and toward its velocity. Preserve that measured segment explicitly;
       * the renderer jitters it in local space before production projection,
       * so flat and diorama modes follow the same camera transform. */
      effect.geometry.data.rect = object.velocity_x < 0
          ? (ActionEffectLocalRect){-32.0f, 0.0f, 0.0f, 32.0f}
          : (ActionEffectLocalRect){0.0f, 0.0f, 32.0f, 32.0f};
    }
    if (kind == kActionEffect_SwordBeam) {
      if (aitos_boss_sword_beam) {
        /* Both three-part boss crescents are authored in OBJ priority 2.
         * `$8D68`'s one-pixel Y bias shifts their ordinary 24x24 headers up
         * one pixel: captured OAM confirms both rectangles exactly. */
        effect.obj_priority = 2;
        effect.geometry.data.rect = object.animation_state == 0x0001
            ? (ActionEffectLocalRect){-8.0f, -17.0f, 16.0f, 7.0f}
            : (ActionEffectLocalRect){-8.0f, -9.0f, 16.0f, 15.0f};
      } else {
        /* These headers use signed 8-bit origins even though the action ABI
         * publishes them as words. `$8D68` performs wrapping byte arithmetic:
         * state $13's normal X=0/8 parts minus left=$E0 draw at +32..+48, not
         * 0..16. The one-pixel OBJ Y bias is included here. Keep the two states
         * and H-flipped choices explicit so presentation follows the exact OAM
         * rectangles observed in run 20260810-184935. */
        const bool flipped =
            (object.flip_attributes & kActRaiserObjectFlip_Horizontal) != 0;
        if (object.animation_state == kSwordBeamHorizontalState) {
          effect.geometry.data.rect = flipped
              ? (ActionEffectLocalRect){-48.0f, -33.0f, -32.0f, -1.0f}
              : (ActionEffectLocalRect){32.0f, -33.0f, 48.0f, -1.0f};
        } else {
          effect.geometry.data.rect = flipped
              ? (ActionEffectLocalRect){-56.0f, -9.0f, -40.0f, 23.0f}
              : (ActionEffectLocalRect){40.0f, -9.0f, 56.0f, 23.0f};
        }
      }
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
