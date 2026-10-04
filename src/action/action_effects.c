#include "actraiser/actraiser_room_profiles.h"
#include "action_effects.h"
#include "action_map_effect_sources.h"
#include "action/action_environment_capture_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>   /* fprintf (AR_AITOS_WATERFALL_LOG) */
#include <stdlib.h>  /* getenv (AR_AITOS_WATERFALL_LOG) */
#include <string.h>

#include "actraiser_game.h"
#include "action_bg_plan.h"
#include "action_bg_world.h"

/* ── Spell rule table ──────────────────────────────────────────────────────
 *
 * Every spell is declared as data rather than as code. The shape comes from
 * docs/rendering-engine.md section 9a, which records the four mapped casts:
 * $00:9F13 dispatches controller +$38 (the spell ID) to
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
 *     should have been. See action_effect_capture.c's [action-fx census] line. */

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

static uint8_t ScenePriorityFromSpriteAttributeBias(
    const uint8_t *wram, size_t wram_size) {
  return (uint8_t)((Read16(wram, wram_size,
                           kActRaiserWram_SpriteAttributeBias) >> 12) & 0x03u);
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
  memset(&observer->landing_dust, 0, sizeof(observer->landing_dust));
  memset(&observer->fireball_smoke, 0, sizeof(observer->fireball_smoke));
  memset(observer->actor_tracks,0,sizeof(observer->actor_tracks));
  observer->scene_clock_valid = 0;
  observer->scene_map_valid = 0;
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
     * (actraiser_widescreen_sprites.c). The animation bank is one byte. */
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

static bool BeginOrAdvanceSceneTrack(ActionEffectObserver *observer,
                                     ActionEffectObserverTrack *track,
                                     const ActionObjectSnapshot *object,
                                     uint8_t kind, uint8_t phase,
                                     unsigned elapsed_ticks,
                                     bool freeze_when_stationary,
                                     ActionEffectInstance *effect) {
  if (!observer || !track || !object || !effect) return false;
  /* Resume/source are stable for the original projectile and trap families.
   * Fireball's handler and parent are stable too and strengthen its identity; trap
   * lightning omits it because one live bolt transitions between $BD36 and
   * the generic timed animation handler $8683 without becoming a new actor.
   * Marahna's orb and split children share a source but retain distinct
   * resume values, so source+resume is their lifecycle key. Linked lightning
   * and the Bloodpool boss child use their validated source/backlink pair. */
  uint32_t continuity_key = (uint32_t)object->source_descriptor |
      ((uint32_t)object->resume_address << 16);
  if (kind == kActionEffect_EnemyFireball) {
    continuity_key ^= (uint32_t)object->handler * 0x9E3779B9u;
    continuity_key ^= (uint32_t)object->spawner_backlink * 0x85EBCA6Bu;
  } else if (kind == kActionEffect_MarahnaFireball)
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->resume_address << 16);
  else if (kind == kActionEffect_SwordBeam) {
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->spawner_backlink << 16);
    if (object->animation_bank == 0x7E &&
        object->animation_address == 0x5000)
      continuity_key ^= (uint32_t)object->local_counter * 0x9E3779B9u;
  } else if (kind == kActionEffect_FillmoreStatueOrb ||
             kind == kActionEffect_CentaurLightning || kind == kActionEffect_NorthwallBossMagic ||
             kind == kActionEffect_BloodpoolBossLightning ||
             kind == kActionEffect_MarahnaLightningLink ||
             kind == kActionEffect_MarahnaBossLightning)
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->spawner_backlink << 16);
  else if (kind == kActionEffect_FlamingWheel)
    continuity_key = (uint32_t)object->source_descriptor |
        ((uint32_t)object->spawner_backlink << 16);
  if (SceneTrackDiscontinuous(track, object, kind, continuity_key,
                              elapsed_ticks))
    memset(track, 0, sizeof(*track));
  const bool frozen = freeze_when_stationary && track->active &&
      track->kind == kind && track->continuity_valid &&
      track->continuity_key == continuity_key &&
      track->last_world_x == object->world_x &&
      track->last_world_y == object->world_y;
  if (frozen && !effect->velocity_x && !effect->velocity_y) {
    /* A stopped native projectile can retain its velocity or clear it. Keep
     * the last heading so its frozen light and ember wake do not turn around. */
    effect->velocity_x = track->last_velocity_x;
    effect->velocity_y = track->last_velocity_y;
  }
  BeginOrAdvanceTrack(observer, track, kind, phase, 0,
                      frozen ? 0 : elapsed_ticks, effect);
  track->continuity_key = continuity_key;
  track->last_world_x = object->world_x;
  track->last_world_y = object->world_y;
  track->last_velocity_x = effect->velocity_x;
  track->last_velocity_y = effect->velocity_y;
  track->continuity_valid = 1;
  return frozen;
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

/* Exact action-scene identities. These signatures intentionally combine
 * control flow, animation, composition, and relationship fields because the
 * object record is polymorphic. Capture evidence and rejected lookalikes are
 * documented in docs/rendering-engine.md section 9a and the RAM/symbol maps. */
enum {
  kEnemyFireballHandler = 0xBDF0,
  kEnemyFireballResume = 0xBDD9,
  kEnemyFireballState = 0x0023,
  kEnemyFireballSourceFirst = 0xBD76,
  kEnemyFireballSourceSecond = 0xBD84,
  kBloodpoolAct1BossSource = 0xB786,
  kBloodpoolAct1BossFireballHandler = 0xB90D,
  kLightningSourceDescriptor = 0xBD2A,
  kLightningResume = 0xBD69,
  kLightningState = 0x0014,
  kLightningHandler = 0xBD36,
  kAnimationDelayHandler = 0x8661,
  kAnimationRepeatHandler = 0x8683,
  kSharedActionChildResume = 0xA65D,
  kSceneAnimationAddress = 0x4000,
  kSceneAnimationBank = 0x7E,
  kBloodpoolAct2FirstMap = 0x02,
  kBloodpoolAct2LastMap = 0x08,
  kBloodpoolBossMap = 0x08,
  kBossLightningSourceDescriptor = 0xBDFF,
  kDeathHeimWizardSourceDescriptor = 0xF6E2,
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
  kMarahnaFireballSourceDescriptor = 0xE047,
  kMarahnaFireballOrbResume = 0xE061,
  kMarahnaFireballOrbState = 0x000C,
  kMarahnaFireballSplitParentResume = 0xE0A6,
  kMarahnaSnakeSourceDescriptor = 0xDE96,
  kMarahnaSnakeRiseHandler = 0xDF3E,
  kMarahnaSnakeFallHandler = 0xDF63,
  kMarahnaSnakeResume = 0xDF34,
  kMarahnaSnakeFireballState = 0x0006,
  kMarahnaLightningHandler = 0x8683,
  kMarahnaLightningSourceDescriptor = 0xE18E,
  kMarahnaLightningPartnerSourceDescriptor = 0xE254,
  kMarahnaLightningResume = 0xE24F,
  kMarahnaLightningHorizontalState = 0x0027,
  kMarahnaLightningVerticalState = 0x0028,
  kMarahnaBossLightningSourceDescriptor = 0xE483,
  kDeathHeimViperSourceDescriptor = 0xF72A,
  kMarahnaBossLightningParentResume = 0xE4E5,
  kMarahnaBossLightningActiveParentResume = 0xE4F4,
  kMarahnaBossLightningGroundParentResume = 0xE4D7,
  kMarahnaBossLightningBoltResume = 0xE578,
  kMarahnaBossLightningGroundChargeResume = 0xE57E,
  kAitosLavaFireballSourceDescriptor = 0xCF9E,
  kAitosLavaFireballResume = 0xCFCD,
  kAitosStatueFireMap = 0x06,
  kAitosStatueFireSourceLeft = 0xD5C0,
  kAitosMoltenRockSourceDescriptor = 0xCEEC,
  kAitosMoltenRockResume = 0xCF16,
  kAitosMoltenRockState = 0x0027,
  kAitosMoltenRockVisual = 0x002B,
  kAitosMoltenRockComposition = 0x4D2D,
  kAitosBossMap = 0x03,
  kFlamingWheelBossMap = 0x07,
  kAitosBossSourceDescriptor = 0xD646,
  kAitosBossSwordBeamParentResume = 0xD793,
  /* Act 2 lake signature. $01 is the continuous bright side-view lip. The
   * row above interleaves transparent cells with animated splash/flame cells;
   * $77 is the map-$06 variant. $33/$34 and $2C/$32 are the two measured
   * left/right bank pairs. */
  kFillmoreBossMap = 0x04,
  kMinotaurSourceDescriptor = 0xAF5D,
  kDeathHeimMinotaurSourceDescriptor = 0xF6CA,
  kMinotaurAxeResume = 0xB008,
  kFlamingWheelSourceDescriptor = 0xD838,
  kDeathHeimFlamingWheelSourceDescriptor = 0xF712,
  kFlamingWheelProjectileFirstState = 0x0008,
  kFlamingWheelProjectileLastState = 0x000C,
  kNorthwallBossMap = 0x08,
  kIceDragonSourceDescriptor = 0xF161,
  kDeathHeimIceDragonSourceDescriptor = 0xF760,
  kIceDragonIceBallResume = 0xF2CA,
  kTanzaraSourceDescriptor = 0xF80F,
  kDeathHeimMinotaurMap = 0x02,
  kDeathHeimWizardMap = 0x03,
  kDeathHeimFlamingWheelMap = 0x05,
  kDeathHeimIceDragonMap = 0x07,
  kDeathHeimRoomOwnerBacklink = 0x001C,
};

static bool SourceIs(uint16_t source, uint16_t original,
                     uint16_t death_heim) {
  return source == original || source == death_heim;
}

static bool IsOriginalOrDeathHeimRoom(const uint8_t *wram, size_t wram_size,
                                      uint8_t original_group,
                                      uint8_t original_map,
                                      uint8_t death_heim_map) {
  const uint8_t group = Read8(wram, wram_size, kActRaiserWram_MapGroup);
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  return (group == original_group && map == original_map) ||
      (group == kActRaiserMapGroup_DeathHeim && map == death_heim_map);
}

static bool SourceMatchesOriginalOrDeathHeimRoom(
    const uint8_t *wram, size_t wram_size, uint16_t source,
    uint8_t original_group, uint8_t original_map, uint16_t original_source,
    uint8_t death_heim_map, uint16_t death_heim_source) {
  const uint8_t group = Read8(wram, wram_size, kActRaiserWram_MapGroup);
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  return (group == original_group && map == original_map &&
          source == original_source) ||
      (group == kActRaiserMapGroup_DeathHeim && map == death_heim_map &&
       source == death_heim_source);
}

static bool IsMarahnaEffectMap(const uint8_t *wram, size_t wram_size) {
  if (!wram ||
      Read8(wram, wram_size, kActRaiserWram_MapGroup) !=
          kActRaiserMapGroup_Marahna)
    return false;
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  return map >= kMarahnaFirstEffectMap && map <= kMarahnaLastEffectMap;
}

static bool IsBloodpoolAct2Map(const uint8_t *wram, size_t wram_size) {
  if (!wram ||
      Read8(wram, wram_size, kActRaiserWram_MapGroup) !=
          kActRaiserMapGroup_Bloodpool)
    return false;
  const uint8_t map = Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  /* Bloodpool Act 2 loads one ordinary-enemy animation family at map $02 and
   * retains it through map $08. Randomized enemies are therefore legal in
   * every room of this range, not only the rooms in the discovery capture. */
  return map >= kBloodpoolAct2FirstMap && map <= kBloodpoolAct2LastMap;
}

static bool IsAitosLavaMap(const uint8_t *wram, size_t wram_size) {
  return wram &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Aitos &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == kAitosLavaMap;
}

static bool IsAitosStatueFireMap(const uint8_t *wram, size_t wram_size) {
  return wram &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Aitos &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
          kAitosStatueFireMap;
}



static bool ActionObjectVisible(const ActionObjectSnapshot *object) {
  return object &&
      !(object->status & (kActRaiserObjectStatus_InactiveMask |
                          kActRaiserObjectStatus_IneligibleMask |
                          kActRaiserObjectStatus_NoDraw)) &&
      !(object->flags & kActRaiserObjectFlag_OutsideActivation);
}

static bool IsEnemyFireball(const ActionObjectSnapshot *object) {
  if (!object ||
      !SourceIs(object->source_descriptor, kEnemyFireballSourceFirst,
                kEnemyFireballSourceSecond) ||
      object->handler != kEnemyFireballHandler ||
      object->resume_address != kEnemyFireballResume ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kEnemyFireballState)
    return false;
  return (object->visual == 0x0017 && object->composition == 0x45EF) ||
      (object->visual == 0x0018 && object->composition == 0x4610);
}

static bool ActionObjectAddressIsValid(uint16_t address);

static bool IsBloodpoolAct1BossFireball(const ActionObjectSnapshot *object,
                                       const uint8_t *wram, size_t size) {
  /* $B8E9 initializes a cloned boss record as a stationary state-0 flame. $B90D owns
   * launched state-1 fireballs; each retains one of four spawn call sites.
   * The body and death fragments share $B786/$5000, so require the flight
   * artwork and retained root identity. The parent's boss flag, status and
   * composition change during death while its launched shots still exist. */
  if (object->source_descriptor != kBloodpoolAct1BossSource ||
      object->handler != kBloodpoolAct1BossFireballHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != 1 ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical) ||
      !((object->visual == 6 && object->composition == 0x5207) ||
        (object->visual == 7 && object->composition == 0x521A)) ||
      !(object->resume_address == 0xB82D || object->resume_address == 0xB841 ||
        object->resume_address == 0xB867 || object->resume_address == 0xB87B) ||
      !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  return ReadActionObject(wram, size, object->spawner_backlink, &parent) &&
      !parent.spawner_backlink &&
      parent.source_descriptor == kBloodpoolAct1BossSource &&
      parent.animation_address == kBossAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank;
}

enum { kFireballSmokeSourceAbsent, kFireballSmokeSourceMoving, kFireballSmokeSourceFrozen };

static void AgeFireballSmoke(ActionFireballSmoke *smoke,
                             const ActionEffectObserver *observer,
                             const uint8_t *sources, unsigned ticks) {
  unsigned count = 0;
  for (unsigned i = 0; i < smoke->count; ++i) {
    ActionFireballSmokePuff puff = smoke->puffs[i];
    unsigned source = kFireballSmokeSourceAbsent;
    if (ActionObjectAddressIsValid(puff.source_address)) {
      const unsigned slot = (puff.source_address - kActRaiserWram_ActionObjectTable) /
          kActRaiserActionObjectStride;
      if (observer->scene_tracks[slot].active &&
          observer->scene_tracks[slot].generation == puff.source_generation)
        source = sources[slot];
    }
    /* Stopped trails disappear with their source. Ordinary detached smoke
     * still dissipates, and a reused slot cannot inherit a frozen trail. */
    if (puff.frozen && source == kFireballSmokeSourceAbsent) continue;
    /* Re-presenting a paused frame is not evidence that this actor stopped. */
    if (ticks) puff.frozen = source == kFireballSmokeSourceFrozen;
    if (!puff.frozen) {
      if (ticks >= kActionFireballSmokeLifetime - puff.age) continue;
      puff.age += ticks;
    }
    smoke->puffs[count++] = puff;
  }
  smoke->count = (uint8_t)count;
}

static void EmitFireballSmoke(ActionFireballSmoke *smoke,
                             const ActionEffectInstance *effect, unsigned ticks) {
  if (!ticks || !(effect->flags & kActionEffectFlag_Visible) ||
      (!effect->velocity_x && !effect->velocity_y)) return;
  /* Reconstruct emissions across skipped captures, oldest first. Restrict the
   * history to this generation and the smoke lifetime: a recycled slot must
   * not bridge from its previous occupant, and catch-up work stays bounded. */
  unsigned back = effect->age_ticks % kActionFireballSmokeInterval;
  if (back >= ticks) return;
  unsigned history = ticks - 1;
  if (history > effect->age_ticks) history = effect->age_ticks;
  if (history >= kActionFireballSmokeLifetime) history = kActionFireballSmokeLifetime - 1;
  back += (history - back) / kActionFireballSmokeInterval * kActionFireballSmokeInterval;
  const int tail = effect->velocity_x < 0 ? 12 : -12;
  for (;;) {
    if (smoke->count == kActionFireballSmokeMaxPuffs) {
      memmove(smoke->puffs, smoke->puffs + 1,
              (kActionFireballSmokeMaxPuffs - 1) * sizeof(smoke->puffs[0]));
      --smoke->count;
    }
    smoke->puffs[smoke->count++] = (ActionFireballSmokePuff){
      .seed = effect->generation * 0x9E3779B9u +
          (effect->age_ticks - back) / kActionFireballSmokeInterval,
      .source_generation = effect->generation,
      .source_address = effect->record_address,
      .x = (int16_t)(effect->world_x - effect->velocity_x * (int)back + tail),
      .y = (int16_t)(effect->world_y - effect->velocity_y * (int)back - 1),
      .age = (uint16_t)back, .priority = effect->obj_priority,
    };
    if (back < kActionFireballSmokeInterval) break;
    back -= kActionFireballSmokeInterval;
  }
}

static bool IsFillmoreStatueOrb(const ActionObjectSnapshot *object,
    const uint8_t *wram, size_t size) {
  /* $B3EA clones the statue; $B406/$B42F run its rolling/falling ball.
   * Parent and child share a source, so neither that nor a red palette is
   * sufficient identity. Verify the flight animation AND live statue backlink. */
  if (object->source_descriptor != 0xB3BF ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->resume_address != 0xB3E9 ||
      !((object->handler == 0xB406 && object->animation_state == 0x0A) ||
        (object->handler == 0xB42F && object->animation_state == 0x0B)) ||
      object->visual < 0x1B || object->visual > 0x1E ||
      object->composition != 0x48F0 + (object->visual-0x1B)*12 ||
      object->spawner_backlink < kActRaiserWram_ActionObjectTable ||
      object->spawner_backlink >= kActRaiserWram_ActionObjectTable +
          kActionSceneEffectObserverTrackCount*kActRaiserActionObjectStride ||
      (object->spawner_backlink-kActRaiserWram_ActionObjectTable) % kActRaiserActionObjectStride)
    return false;
  ActionObjectSnapshot parent;
  return ReadActionObject(wram, size, object->spawner_backlink, &parent) &&
      !(parent.status & kActRaiserObjectStatus_InactiveMask) &&
      parent.source_descriptor == 0xB3BF && !parent.spawner_backlink &&
      parent.animation_address == kSceneAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank && parent.animation_state == 0x24;
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
      parent.handler == kAnimationDelayHandler &&
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
    {kAnimationDelayHandler, kMarahnaSnakeResume,
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
      object->handler != kAnimationDelayHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kMarahnaSnakeFireballState ||
      object->resume_address != kSharedActionChildResume ||
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
      object->handler != kAnimationDelayHandler ||
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
  if (object->resume_address != kSharedActionChildResume ||
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
    {0x0023, kAnimationDelayHandler,  0,  0},
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
      (object->visual == kAitosMoltenRockVisual &&
       object->composition == kAitosMoltenRockComposition);
  if (!artwork) return false;
  for (size_t i = 0; i < sizeof(kLifecycle) / sizeof(kLifecycle[0]); i++)
    if (object->animation_state == kLifecycle[i].state &&
        object->handler == kLifecycle[i].handler &&
        object->velocity_x == kLifecycle[i].velocity_x &&
        object->velocity_y == kLifecycle[i].velocity_y)
      return true;
  return false;
}

static bool IsAitosStatueFire(const ActionObjectSnapshot *object) {
  if (!object || !ActRaiser_IsAitosStatueFireActor(
          kActRaiserMapGroup_Aitos, kAitosStatueFireMap,
          object->source_descriptor, object->animation_address,
      object->animation_bank) ||
      object->spawner_backlink != 0 || object->velocity_x != 0 ||
      object->velocity_y != 0)
    return false;

  /* `$D5B1/$D5C0` are the two facing spawn records. Their retained source and
   * base flip select the direction; priority is deliberately not part of the
   * identity. State $18 grows the plume through visuals $1C-$1E and state $19
   * sustains it with the $1F/$1E flicker. State $1A's long $17 hold is the
   * inactive interval between breaths and must not emit presentation fire. */
  const uint16_t expected_flip =
      object->source_descriptor == kAitosStatueFireSourceLeft
          ? kActRaiserObjectFlip_Horizontal : 0;
  if ((object->flip_attributes & kActRaiserObjectFlip_Mask) != expected_flip ||
      (object->animation_state != 0x0018 &&
       object->animation_state != 0x0019))
    return false;

  if (object->top_extent != 8 || object->bottom_extent != 8) return false;
  if (object->animation_state == 0x0018 &&
      object->visual == 0x001C && object->composition == 0x4763)
    return object->left_extent == 16 && object->right_extent == 16;
  if (object->animation_state == 0x0018 &&
      object->visual == 0x001D && object->composition == 0x4776)
    return object->left_extent + object->right_extent == 48 &&
        object->left_extent >= 16 && object->right_extent >= 16;
  const bool full_pillar =
      (object->visual == 0x001E && object->composition == 0x4790) ||
      (object->animation_state == 0x0019 &&
       object->visual == 0x001F && object->composition == 0x47B1);
  return full_pillar &&
      object->left_extent + object->right_extent == 64 &&
      object->left_extent >= 16 && object->right_extent >= 16;
}

static bool IsAitosMoltenRock(const ActionObjectSnapshot *object) {
  if (!object ||
      object->source_descriptor != kAitosMoltenRockSourceDescriptor ||
      object->resume_address != kAitosMoltenRockResume ||
      object->handler != kAnimationDelayHandler ||
      object->animation_address != kSceneAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != kAitosMoltenRockState ||
      object->visual != kAitosMoltenRockVisual ||
      object->composition != kAitosMoltenRockComposition ||
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
      !SourceIs(object->source_descriptor,
                kMarahnaBossLightningSourceDescriptor,
                kDeathHeimViperSourceDescriptor) ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->left_extent != 48 || object->top_extent != 40 ||
      object->right_extent != 48 || object->bottom_extent != 8 ||
      (object->source_descriptor == kMarahnaBossLightningSourceDescriptor
           ? object->spawner_backlink != 0
           : object->spawner_backlink != kDeathHeimRoomOwnerBacklink) ||
      (object->flip_attributes & kActRaiserObjectFlip_Mask))
    return false;
  return (object->handler == kAnimationDelayHandler &&
          object->animation_state == 0x0000 &&
          object->resume_address == kMarahnaBossLightningParentResume) ||
      (object->handler == kAnimationDelayHandler &&
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
      !SourceIs(object->source_descriptor,
                kMarahnaBossLightningSourceDescriptor,
                kDeathHeimViperSourceDescriptor) ||
      object->handler != kAnimationDelayHandler ||
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
      !MarahnaBossParentMatches(&parent) ||
      parent.source_descriptor != object->source_descriptor)
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
      parent.status != kActRaiserObjectStatus_Inactive ||
      parent.source_descriptor != kAitosBossSourceDescriptor ||
      parent.handler != kAnimationDelayHandler ||
      parent.animation_address != kBossAnimationAddress ||
      parent.animation_bank != kSceneAnimationBank ||
      parent.resume_address != kAitosBossSwordBeamParentResume ||
      parent.animation_state != 0x0000 || parent.visual != 0x0023 ||
      parent.composition != 0x56FE ||
      parent.flip_attributes != object->flip_attributes ||
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
  const uint16_t reflected_flips =
      kActRaiserObjectFlip_Horizontal | kActRaiserObjectFlip_Vertical;
  if (!object ||
      object->source_descriptor != kAitosBossSourceDescriptor ||
      object->handler != kAnimationDelayHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->resume_address != kSharedActionChildResume ||
      object->animation_index != 0x0001 ||
      (object->flip_attributes != 0 &&
       object->flip_attributes != reflected_flips) ||
      object->flags != 0x0020 ||
      !AitosBossSwordBeamParentIsValid(wram, wram_size, object))
    return false;

  /* Run 20260812-224123 measured the controller's other facing. The engine
   * rotates the complete two-child volley by 180 degrees: controller and
   * child both carry H+V ($C000), both velocity components reverse, and the
   * four culling extents swap sides. Keeping that relationship explicit
   * admits all four authored diagonals without accepting independent flip,
   * velocity, or extent mixtures. */
  const bool reflected = object->flip_attributes == reflected_flips;

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
        object->velocity_x == (reflected ? 3 : -3) &&
        object->velocity_y ==
            (reflected ? -kCrescents[i].velocity_y
                       : kCrescents[i].velocity_y) &&
        object->left_extent == (reflected ? 16 : 8) &&
        object->right_extent == (reflected ? 8 : 16) &&
        object->top_extent ==
            (reflected ? kCrescents[i].bottom_extent
                       : kCrescents[i].top_extent) &&
        object->bottom_extent ==
            (reflected ? kCrescents[i].top_extent
                       : kCrescents[i].bottom_extent))
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
      SourceIs(parent.source_descriptor, kBossLightningSourceDescriptor,
               kDeathHeimWizardSourceDescriptor) &&
      parent.source_descriptor == object->source_descriptor &&
      parent.animation_address == kBossAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank;
}

static uint8_t MatchBloodpoolBossLightning(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  if (!object ||
      !SourceIs(object->source_descriptor, kBossLightningSourceDescriptor,
                kDeathHeimWizardSourceDescriptor) ||
      object->handler != kAnimationDelayHandler ||
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

/* First-act magic uses the resident $5000 family, not the second-act boss
 * signatures above. Match the authored visual AND its saved continuation;
 * body poses, spear pieces and copied-but-not-initialized children share the
 * source record. See docs/ram-map.md, "First-act boss magic". */
static uint8_t MatchCentaurLightning(const ActionObjectSnapshot *object) {
  if (object->source_descriptor != 0xAD45 || object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical))
    return kActionEffectPhase_None;
  const bool delay = object->handler == kAnimationDelayHandler;
  const bool repeat = object->handler == kAnimationRepeatHandler;
  const unsigned state = object->animation_state, visual = object->visual;
  const unsigned resume = object->resume_address;
  if (repeat && (object->flags & 0x4000) &&
      ((state == 2 && resume == 0xAE0C) || (state == 8 && resume == 0xAE15))) {
    static const uint16_t kCharge[] = {0x5413, 0x5537, 0x5677, 0x57B7, 0x58F7};
    if (visual >= 7 && visual <= 11 && object->composition == kCharge[visual - 7])
      return kActionEffectPhase_CentaurStaffCharge;
  }
  if (!ActionObjectAddressIsValid(object->spawner_backlink)) return kActionEffectPhase_None;
  const bool blank = visual == 0x18 && object->composition == 0x634F;
  static const uint16_t kBolts[] = {0x635B, 0x636E, 0x638F, 0x63BE, 0x63FB, 0x640E, 0x642F, 0x645E};
  const bool bolt =
      visual >= 0x19 && visual <= 0x20 && object->composition == kBolts[visual - 0x19];
  if ((blank || bolt) &&
      ((delay && state == 0x0D && resume == 0xAEC7 &&
        (blank || (visual >= 0x1D && visual <= 0x1F))) ||
       (state == 0x0E && ((delay && resume == 0xAECD) || (repeat && resume == 0xAEEC)) &&
        (blank || visual == 0x20)) ||
       (delay && state == 0x0B && resume == 0xAEF7 &&
        (blank || (visual >= 0x19 && visual <= 0x1B))) ||
       (state == 0x0C && ((delay && resume == 0xAEFD) || (repeat && resume == 0xAF1C)) &&
        (blank || visual == 0x1C))))
    return kActionEffectPhase_BossLightningStrike;
  static const uint16_t kImpact[] = {0x649B, 0x64A7, 0x64C8};
  if (delay && state == 4 && resume == 0xAF58 &&
      (blank ||
       (visual >= 0x21 && visual <= 0x23 && object->composition == kImpact[visual - 0x21])))
    return kActionEffectPhase_BossLightningImpact;
  return kActionEffectPhase_None;
}

static uint8_t MatchNorthwallBossMagic(const ActionObjectSnapshot *object) {
  if (object->source_descriptor != 0xE7C6 || object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank || object->handler != kAnimationDelayHandler ||
      (object->flip_attributes & kActRaiserObjectFlip_Vertical))
    return kActionEffectPhase_None;
  const unsigned visual = object->visual;
  static const uint16_t kCharge[] = {0x51A7, 0x522A, 0x52AD, 0x5314, 0x53AC};
  if (object->animation_state == 2 && object->resume_address == 0xE85E &&
      (object->flags & 0x4000) && visual >= 0x0A && visual <= 0x0E &&
      object->composition == kCharge[visual - 0x0A])
    return kActionEffectPhase_NorthwallMagicCharge;
  if (!ActionObjectAddressIsValid(object->spawner_backlink)) return kActionEffectPhase_None;
  if (object->animation_state == 0 && object->resume_address == 0xE8AE && visual == 9 &&
      object->composition == 0x518D)
    return kActionEffectPhase_NorthwallMagicFall;
  static const uint16_t kImpact[] = {0x5114, 0x5120, 0x5133, 0x5146, 0x5159, 0x516C};
  /* The floor child outlives the falling actor that allocated it. Its own
   * exact signature remains valid after that parent slot retires/recycles. */
  /* Regional European impact poses keep visual 8. Accept only starts of
   * slots in the shared NorthwallExpand workspace, never their padding. */
  const unsigned expansion_offset =
      (unsigned)object->composition - kActRaiserWram_NorthwallImpactExpansion;
  const bool expanded = visual == 8 &&
                        expansion_offset < kActRaiserNorthwallImpactExpansionCount *
                                               kActRaiserNorthwallImpactExpansionStride &&
                        expansion_offset % kActRaiserNorthwallImpactExpansionStride == 0;
  if (object->animation_state == 1 && object->resume_address == 0xE8BD &&
      (expanded || (visual >= 3 && visual <= 8 && object->composition == kImpact[visual - 3])))
    return kActionEffectPhase_NorthwallMagicImpact;
  return kActionEffectPhase_None;
}

static bool BossFamilyParentIsValid(const uint8_t *wram, size_t wram_size,
                                    const ActionObjectSnapshot *object,
                                    uint16_t original_source,
                                    uint16_t death_heim_source) {
  if (!object || !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;
  ActionObjectSnapshot parent;
  return ReadActionObject(wram, wram_size, object->spawner_backlink,
                          &parent) &&
      parent.composition &&
      SourceIs(parent.source_descriptor, original_source,
               death_heim_source) &&
      parent.source_descriptor == object->source_descriptor &&
      parent.animation_address == kBossAnimationAddress &&
      parent.animation_bank == kSceneAnimationBank;
}

static bool IsMinotaurAxe(const uint8_t *wram, size_t wram_size,
                         const ActionObjectSnapshot *object) {
  static const uint16_t kCompositions[] = {
    0x50FB, 0x5138, 0x5159, 0x5196, 0x51B7, 0x51F4, 0x5215, 0x5252,
  };
  if (!object ||
      !SourceIs(object->source_descriptor, kMinotaurSourceDescriptor,
                kDeathHeimMinotaurSourceDescriptor) ||
      object->handler != kAnimationDelayHandler ||
      object->resume_address != kMinotaurAxeResume ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state != 0x0003 ||
      !BossFamilyParentIsValid(wram, wram_size, object,
                               kMinotaurSourceDescriptor,
                               kDeathHeimMinotaurSourceDescriptor))
    return false;
  if (object->visual <= 7u)
    return object->composition == kCompositions[object->visual];
  return object->visual == 0x0010 && object->composition == 0x59B0 &&
      object->left_extent == 4 && object->top_extent == 4 &&
      object->right_extent == 4 && object->bottom_extent == 4;
}

static bool IsFlamingWheel(const ActionObjectSnapshot *object) {
  if (!object ||
      !SourceIs(object->source_descriptor, kFlamingWheelSourceDescriptor,
                kDeathHeimFlamingWheelSourceDescriptor) ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      !(object->flags & 0x4000u) || !object->composition)
    return false;
  /* The source descriptor identifies the whole boss family, not just the
   * visible wheel. Children retain an action-object backlink, while the native
   * body is root-owned and the rematch body retains Death Heim's room owner.
   * This remains stable as the body moves between delay, repeat, and AI
   * handlers without admitting helpers as additional full-strength emitters. */
  return object->source_descriptor == kFlamingWheelSourceDescriptor
      ? object->spawner_backlink == 0
      : object->spawner_backlink == kDeathHeimRoomOwnerBacklink;
}

static bool IsFlamingWheelProjectile(
    const uint8_t *wram, size_t wram_size,
    const ActionObjectSnapshot *object) {
  static const uint16_t kCompositions[] = {
    0x51B5, 0x51C1, 0x51CD, 0x51D9,
  };
  static const int8_t kVelocity[][2] = {
    {-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {1, 0},
  };
  if (!object ||
      !SourceIs(object->source_descriptor, kFlamingWheelSourceDescriptor,
                kDeathHeimFlamingWheelSourceDescriptor) ||
      object->handler != kAnimationDelayHandler ||
      object->resume_address != kSharedActionChildResume ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      object->animation_state < kFlamingWheelProjectileFirstState ||
      object->animation_state > kFlamingWheelProjectileLastState ||
      object->visual >= sizeof(kCompositions) / sizeof(kCompositions[0]) ||
      object->composition != kCompositions[object->visual] ||
      object->left_extent != 8 || object->top_extent != 8 ||
      object->right_extent != 8 || object->bottom_extent != 8 ||
      object->local_counter != object->animation_state ||
      object->flags != 0x0020 ||
      object->flip_attributes != kActRaiserObjectFlip_Horizontal ||
      !ActionObjectAddressIsValid(object->spawner_backlink))
    return false;

  const unsigned direction =
      (unsigned)(object->animation_state - kFlamingWheelProjectileFirstState);
  if (object->velocity_x != kVelocity[direction][0] ||
      object->velocity_y != kVelocity[direction][1])
    return false;

  ActionObjectSnapshot parent;
  return ReadActionObject(wram, wram_size, object->spawner_backlink,
                          &parent) &&
      parent.source_descriptor == object->source_descriptor &&
      IsFlamingWheel(&parent);
}

static bool IsIceDragonIceBall(const uint8_t *wram, size_t wram_size,
                               const ActionObjectSnapshot *object) {
  static const uint16_t kCompositions[] = {
    0x5D9C, 0x5DA8, 0x5DB4, 0x5DC0,
    0x5DCC, 0x5DD8, 0x5DE4, 0x5DF0,
  };
  if (!object ||
      !SourceIs(object->source_descriptor, kIceDragonSourceDescriptor,
                kDeathHeimIceDragonSourceDescriptor) ||
      object->handler != kAnimationDelayHandler ||
      object->resume_address != kIceDragonIceBallResume ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank ||
      !BossFamilyParentIsValid(wram, wram_size, object,
                               kIceDragonSourceDescriptor,
                               kDeathHeimIceDragonSourceDescriptor))
    return false;
  if (object->visual < 0x0012 || object->visual > 0x0019)
    return false;
  const unsigned frame = object->visual - 0x0012;
  return object->composition == kCompositions[frame] &&
      object->animation_state == (frame < 4u ? 0x0019 : 0x001A);
}

typedef struct TanzaraProjectileFrame {
  uint16_t resume, state, visual, composition;
} TanzaraProjectileFrame;

static bool IsTanzaraProjectile(const ActionObjectSnapshot *object) {
  static const TanzaraProjectileFrame kFrames[] = {
    {0xFD77, 0x0008, 0x0016, 0x5D17},
    {0xFD77, 0x0008, 0x0017, 0x5D23},
    {0xFD77, 0x0008, 0x0018, 0x5D2F},
    {0xFD77, 0x0008, 0x0019, 0x5D50},
    {0xFD77, 0x0008, 0x001A, 0x5D71},
    {0xFD77, 0x0008, 0x001B, 0x5D92},
    {0xFD77, 0x0008, 0x001C, 0x5DB3},
    {0xFD9E, 0x0009, 0x0027, 0x5E68},
    {0xFD9E, 0x0009, 0x0028, 0x5E74},
    {0xFBEA, 0x002D, 0x0030, 0x5EF7},
    {0xFBEA, 0x002D, 0x0031, 0x5F03},
    {0xFBEA, 0x002D, 0x0032, 0x5F0F},
    {0xFBEA, 0x002D, 0x0033, 0x5F1B},
    {0xFBEA, 0x002D, 0x0034, 0x5F27},
    {0xFBF5, 0x0025, 0x0033, 0x5F1B},
    {0xFBF5, 0x0025, 0x0035, 0x5F48},
    {0xFC13, 0x0027, 0x0033, 0x5F1B},
    {0xFC13, 0x0027, 0x0035, 0x5F48},
    {0xFC21, 0x0028, 0x0033, 0x5F1B},
    {0xFC21, 0x0028, 0x0035, 0x5F48},
    {0xFCA1, 0x001B, 0x001F, 0x5E08},
    {0xFCA1, 0x001B, 0x0021, 0x5E20},
    {0xFCA1, 0x001B, 0x0023, 0x5E38},
    {0xFCA1, 0x001B, 0x0025, 0x5E50},
    {0xFCAF, 0x001B, 0x001F, 0x5E08},
    {0xFCAF, 0x001B, 0x0021, 0x5E20},
    {0xFCAF, 0x001B, 0x0023, 0x5E38},
    {0xFCAF, 0x001B, 0x0025, 0x5E50},
    {0xFCB5, 0x0016, 0x001F, 0x5E08},
    {0xFCB5, 0x0016, 0x0021, 0x5E20},
    {0xFCB5, 0x0016, 0x0023, 0x5E38},
    {0xFCB5, 0x0016, 0x0025, 0x5E50},
    {0xFCD6, 0x0017, 0x001F, 0x5E08},
    {0xFCD6, 0x0017, 0x0021, 0x5E20},
    {0xFCD6, 0x0019, 0x001F, 0x5E08},
    {0xFCD6, 0x0019, 0x0021, 0x5E20},
    {0xFCED, 0x001A, 0x0020, 0x5E14},
    {0xFCED, 0x001A, 0x0022, 0x5E2C},
    {0xFCED, 0x001A, 0x0024, 0x5E44},
    {0xFCED, 0x001A, 0x0026, 0x5E5C},
    {0xFCFB, 0x001A, 0x0020, 0x5E14},
    {0xFCFB, 0x001A, 0x0022, 0x5E2C},
    {0xFCFB, 0x001A, 0x0024, 0x5E44},
    {0xFCFB, 0x001A, 0x0026, 0x5E5C},
    {0xFD22, 0x0013, 0x0020, 0x5E14},
    {0xFD22, 0x0013, 0x0022, 0x5E2C},
    {0xFD22, 0x0014, 0x0020, 0x5E14},
    {0xFD22, 0x0014, 0x0022, 0x5E2C},
    {0xFD44, 0x0022, 0x0029, 0x5E95},
    {0xFD44, 0x0022, 0x002A, 0x5EA8},
  };
  if (!object || object->source_descriptor != kTanzaraSourceDescriptor ||
      object->handler != kAnimationDelayHandler ||
      object->animation_address != kBossAnimationAddress ||
      object->animation_bank != kSceneAnimationBank)
    return false;
  for (size_t i = 0; i < sizeof(kFrames) / sizeof(kFrames[0]); i++)
    if (object->resume_address == kFrames[i].resume &&
        object->animation_state == kFrames[i].state &&
        object->visual == kFrames[i].visual &&
        object->composition == kFrames[i].composition)
      return true;
  return false;
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

bool ActionSceneEffects_RoomUsesBg1Decorations(
    const uint8_t *wram, size_t wram_size) {
  const unsigned cave_room = FillmoreCaveRoom(wram, wram_size);
  /* Room 3 uses the winner mask for scenery dimming throughout the climb,
   * as well as the floor mist. Room 2 uses it for damp-stone highlights. */
  return cave_room == 2 || cave_room == 3 || IsBloodpoolMarsh(wram, wram_size) ||
      ActionMapEnvironmentScene_UsesTorches(Read8(wram,wram_size,kActRaiserWram_MapGroup),
          Read8(wram,wram_size,kActRaiserWram_CurrentMap));
}

bool ActionSceneEffects_RoomUsesBg2Decorations(
    const uint8_t *wram, size_t wram_size) {
  return IsFillmoreForest(wram, wram_size) || IsBloodpoolMarsh(wram, wram_size) ||
      (BloodpoolCastleRoom(wram,wram_size) &&
       Read8(wram,wram_size,kActRaiserWram_CurrentMap) != 5) ||
      FillmoreCaveRoom(wram, wram_size) == 2 ||
      (wram && Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
           kActRaiserMapGroup_Aitos &&
       Read8(wram, wram_size, kActRaiserWram_CurrentMap) >= 2 &&
       Read8(wram, wram_size, kActRaiserWram_CurrentMap) <= 3);
}

void ActionEnvironmentalEffects_CaptureFrameFiltered(
    ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t wram_size, bool native_ray_field, bool native_water_field, bool native_atmosphere_field, bool native_moon_field, bool native_marsh_field, bool native_castle_field) {
  /* Environmental fields are optional, bounded records. Their observers
   * continue keeping scene time while this capture is gated off. */
  if (!observer || !dst || !observer->scene_clock_valid ||
      !observer->scene_map_valid ||
      dst->decoration_overflow)
    return;
  if (observer->scene_map_group == kActRaiserMapGroup_Bloodpool) {
    CaptureBloodpoolMarsh(observer, dst, wram, wram_size,native_moon_field,native_marsh_field);
    CaptureBloodpoolCastle(observer, dst, wram, wram_size,native_castle_field);
    return;
  }
  if (observer->scene_map_group != kActRaiserMapGroup_Fillmore) return;
  if (observer->scene_map_number != 1) {
    CaptureFillmoreCaveFiltered(observer, dst, wram, wram_size,native_water_field,native_atmosphere_field);
    return;
  }
  if(native_ray_field)CaptureFillmoreForest(observer, dst, wram, wram_size);
}

void ActionEnvironmentalEffects_CaptureFrame(
    ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t wram_size) {
  ActionEnvironmentalEffects_CaptureFrameFiltered(observer,dst,wram,wram_size,true,true,true,true,true,true);
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

/* No new game objects or renderer identity tests: every active composition can
 * be selected by an authored effect, including previously unrecognized attacks. */
static void CaptureAuthoringActor(ActionEffectObserver *observer,ActionSceneEffectFrame *dst,
    unsigned slot,uint16_t address,const ActionObjectSnapshot *object,const uint8_t *ram,size_t size,unsigned ticks) {
  ActionEffectActorTrack *track=&observer->actor_tracks[slot];
  ActionEffectActor actor={.source=object->source_descriptor,.animation=object->animation_address,
    .bank=object->animation_bank,.state=object->animation_state,.visual=object->visual,
    .handler=object->handler,.resume=object->resume_address,.address=address,
    .x=object->world_x,.y=object->world_y,.vx=object->velocity_x,.vy=object->velocity_y,
    .priority=ScenePriorityFromSpriteAttributeBias(ram,size),.visible=ActionObjectVisible(object),
    .flip=(uint8_t)(object->flip_attributes>>8),.player=address==kActRaiserWram_PlayerObject};
  const unsigned parent=object->spawner_backlink;
  if(parent>=kActRaiserWram_ActionObjectTable && parent<kActRaiserWram_ActionObjectTable+kActionEffectActorMax*kActRaiserActionObjectStride &&
      (parent-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride==0 &&
      !(Read16(ram,size,parent+kActRaiserActionObject_Status)&kActRaiserObjectStatus_InactiveMask))
    actor.parent_source=Read16(ram,size,parent+kActRaiserActionObject_SourceDescriptor);
  const ActionEffectActor *old=&track->actor;
  const bool same=track->active&&old->source==actor.source&&old->animation==actor.animation&&old->bank==actor.bank&&
    old->parent_source==actor.parent_source&&abs((int)actor.x-old->x)<=128&&abs((int)actor.y-old->y)<=128;
  actor.generation=same?old->generation:AllocateSequence(&observer->next_actor_generation);
  actor.age=same?AddSaturated16(old->age,ticks):0;
  actor.phase_ticks=same&&old->state==actor.state?AddSaturated16(old->phase_ticks,ticks):0;
  *track=(ActionEffectActorTrack){actor,1};
  if(dst->actor_count<kActionEffectActorMax)dst->actors[dst->actor_count++]=actor;
}

/* AR_AITOS_WATERFALL_LOG=1: the veil appended below is what publishes the
 * `waterfall` section token (action_effect_capture.c), which in turn admits the
 * folded BG2 continuation (diorama.c). Those three live in different files, so
 * a capture-side dropout presents as a rendering bug at the far end and costs a
 * session to trace. Log the capture decision on CHANGE only, so a jump reads as
 * a handful of lines instead of 60 per second. */
static void AitosWaterfallLog(const ActionSceneEffectFrame *dst,
                              const ActionEnvironmentWaterfallTrace *trace) {
  const int camera_x = trace->camera_x, camera_y = trace->camera_y;
  const unsigned splash_count = trace->splash_count;
  const bool published = trace->published;
  static int log_on = -1;
  if (log_on < 0) {
    const char *value = getenv("AR_AITOS_WATERFALL_LOG");
    log_on = (value && value[0] && value[0] != '0') ? 1 : 0;
  }
  if (!log_on) return;
  static unsigned last_splash = UINT_MAX;
  static int last_published = -1;
  if (splash_count == last_splash && (int)published == last_published) return;
  last_splash = splash_count;
  last_published = (int)published;
  fprintf(stderr,
          "[aitos-wf] capture gf=%u cam=(%d,%d) scan=[%u,%u)x[%u,%u) "
          "splash=%u veil=%s\n",
          (unsigned)dst->game_frame, camera_x, camera_y,
          trace->x0, trace->x1, trace->y0, trace->y1,
          splash_count, published ? "yes" : "no");
}


void ActionSceneEffects_CaptureFrameFiltered(ActionEffectObserver *observer,
                                     ActionSceneEffectFrame *dst,
                                     const uint8_t *wram, size_t wram_size,
                                     unsigned elapsed_ticks, bool native_glow,const ActionSurfaceField *const *surfaces) {
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

  const uint8_t map_group =
      Read8(wram, wram_size, kActRaiserWram_MapGroup);
  const uint8_t map_number =
      Read8(wram, wram_size, kActRaiserWram_CurrentMap);
  if (!observer->scene_map_valid ||
      observer->scene_map_group != map_group ||
      observer->scene_map_number != map_number) {
    /* A room handoff can replace the whole action table without presenting an
     * intermediate inactive frame. Never let a same-slot/source actor inherit
     * the previous room's trail, pulse, or map-decoration clock. */
    RetireSceneAll(observer);
    observer->scene_map_group = map_group;
    observer->scene_map_number = map_number;
    observer->scene_map_valid = 1;
  }

  if (!observer->scene_clock_valid) {
    /* Preserve the established visual phase on entry/load, then decouple it
     * from $0088: that ROM clock keeps moving on the native pause screen. */
    observer->scene_clock = dst->game_frame;
    observer->scene_clock_valid = 1;
  } else {
    observer->scene_clock = (uint16_t)(observer->scene_clock + elapsed_ticks);
  }
  ActionEnvironmentScene map_scene;
  if (ActionEnvironmentScene_FromWram(&map_scene,wram,wram_size,observer->scene_clock)) {
    map_scene.suppress_default_glow_field=!native_glow;
    if(surfaces)memcpy(map_scene.surface_fields,surfaces,sizeof(map_scene.surface_fields));
    ActionEnvironmentWaterfallTrace trace = {0};
    ActionMapEnvironmentScene_Capture(&map_scene,dst,&trace);
    if (trace.valid) AitosWaterfallLog(dst,&trace);
  }
  if (dst->decoration_overflow) {
    dst->decoration_count = 0;
    dst->decoration_visible_count = 0;
  }
  const bool marahna_effect_map = IsMarahnaEffectMap(wram, wram_size);
  const bool bloodpool_act2_map = IsBloodpoolAct2Map(wram, wram_size);
  const bool aitos_lava_map = IsAitosLavaMap(wram, wram_size);
  const bool aitos_statue_fire_map =
      IsAitosStatueFireMap(wram, wram_size);
  const bool aitos_boss_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Aitos &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == kAitosBossMap;
  const bool boss_lightning_map =
      IsOriginalOrDeathHeimRoom(wram, wram_size,
                                kActRaiserMapGroup_Bloodpool,
                                kBloodpoolBossMap, kDeathHeimWizardMap);
  const bool marahna_boss_map =
      IsOriginalOrDeathHeimRoom(wram, wram_size,
                                kActRaiserMapGroup_Marahna,
                                kMarahnaBossMap, kDeathHeimViperMap);
  const bool minotaur_map =
      IsOriginalOrDeathHeimRoom(wram, wram_size,
                                kActRaiserMapGroup_Fillmore,
                                kFillmoreBossMap, kDeathHeimMinotaurMap);
  const bool flaming_wheel_map =
      IsOriginalOrDeathHeimRoom(wram, wram_size,
                                kActRaiserMapGroup_Aitos,
                                kFlamingWheelBossMap,
                                kDeathHeimFlamingWheelMap);
  const bool ice_dragon_map =
      IsOriginalOrDeathHeimRoom(wram, wram_size,
                                kActRaiserMapGroup_Northwall,
                                kNorthwallBossMap, kDeathHeimIceDragonMap);
  const bool tanzara_map =
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_DeathHeim &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
          kActRaiserDeathHeimMap_FinalBoss;
  bool seen[kActionSceneEffectObserverTrackCount] = {false};
  uint8_t smoke_sources[kActionSceneEffectObserverTrackCount] = {0};
  for (unsigned slot = 0; slot < kActionSceneEffectObserverTrackCount;
       slot++) {
    const uint16_t address = (uint16_t)(kActRaiserWram_ActionObjectTable +
        slot * kActRaiserActionObjectStride);
    ActionObjectSnapshot object;
    if (!ReadActionObject(wram, wram_size, address, &object) ||
        (object.status & kActRaiserObjectStatus_InactiveMask) ||
        !object.composition) {
      observer->actor_tracks[slot].active=0;
      continue;
    }
    CaptureAuthoringActor(observer,dst,slot,address,&object,wram,wram_size,elapsed_ticks);

    uint8_t kind = kActionEffect_None;
    uint8_t phase = kActionEffectPhase_None;
    bool aitos_boss_sword_beam = false;
    const bool bloodpool_boss_fireball =
        map_group == kActRaiserMapGroup_Bloodpool && map_number == 1 &&
        IsBloodpoolAct1BossFireball(&object, wram, wram_size);
    if (map_group == kActRaiserMapGroup_Fillmore && map_number == 1 &&
        (phase = MatchCentaurLightning(&object)) != kActionEffectPhase_None) {
      kind = kActionEffect_CentaurLightning;
    } else if (map_group == kActRaiserMapGroup_Northwall && map_number == 4 &&
               (phase = MatchNorthwallBossMagic(&object)) != kActionEffectPhase_None) {
      kind = kActionEffect_NorthwallBossMagic;
    } else if (minotaur_map &&
        SourceMatchesOriginalOrDeathHeimRoom(
            wram, wram_size, object.source_descriptor,
            kActRaiserMapGroup_Fillmore, kFillmoreBossMap,
            kMinotaurSourceDescriptor, kDeathHeimMinotaurMap,
            kDeathHeimMinotaurSourceDescriptor) &&
        IsMinotaurAxe(wram, wram_size, &object)) {
      kind = kActionEffect_MinotaurAxe;
      phase = kActionEffectPhase_MinotaurAxeFlight;
    } else if (flaming_wheel_map &&
               SourceMatchesOriginalOrDeathHeimRoom(
                   wram, wram_size, object.source_descriptor,
                   kActRaiserMapGroup_Aitos, kFlamingWheelBossMap,
                   kFlamingWheelSourceDescriptor,
                   kDeathHeimFlamingWheelMap,
                   kDeathHeimFlamingWheelSourceDescriptor) &&
               IsFlamingWheel(&object)) {
      kind = kActionEffect_FlamingWheel;
      phase = kActionEffectPhase_FlamingWheelBody;
    } else if (flaming_wheel_map &&
               SourceMatchesOriginalOrDeathHeimRoom(
                   wram, wram_size, object.source_descriptor,
                   kActRaiserMapGroup_Aitos, kFlamingWheelBossMap,
                   kFlamingWheelSourceDescriptor,
                   kDeathHeimFlamingWheelMap,
                   kDeathHeimFlamingWheelSourceDescriptor) &&
               IsFlamingWheelProjectile(wram, wram_size, &object)) {
      kind = kActionEffect_FlamingWheelProjectile;
      phase = kActionEffectPhase_FlamingWheelProjectileFlight;
    } else if (ice_dragon_map &&
               SourceMatchesOriginalOrDeathHeimRoom(
                   wram, wram_size, object.source_descriptor,
                   kActRaiserMapGroup_Northwall, kNorthwallBossMap,
                   kIceDragonSourceDescriptor, kDeathHeimIceDragonMap,
                   kDeathHeimIceDragonSourceDescriptor) &&
               IsIceDragonIceBall(wram, wram_size, &object)) {
      kind = kActionEffect_IceDragonIceBall;
      phase = kActionEffectPhase_IceDragonIceBallFlight;
    } else if (tanzara_map && IsTanzaraProjectile(&object)) {
      kind = kActionEffect_TanzaraProjectile;
      phase = kActionEffectPhase_TanzaraProjectileFlight;
    } else if (bloodpool_boss_fireball ||
               (bloodpool_act2_map && IsEnemyFireball(&object))) {
      kind = kActionEffect_EnemyFireball;
      phase = kActionEffectPhase_EnemyFireballFlight;
    } else if (map_group == kActRaiserMapGroup_Fillmore &&
        (map_number == 2 || map_number == 3) &&
        IsFillmoreStatueOrb(&object, wram, wram_size)) {
      kind = kActionEffect_FillmoreStatueOrb;
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
               SourceMatchesOriginalOrDeathHeimRoom(
                   wram, wram_size, object.source_descriptor,
                   kActRaiserMapGroup_Marahna, kMarahnaBossMap,
                   kMarahnaBossLightningSourceDescriptor,
                   kDeathHeimViperMap,
                   kDeathHeimViperSourceDescriptor) &&
               (phase = MatchMarahnaBossLightning(
                    wram, wram_size, &object)) !=
                   kActionEffectPhase_None) {
      kind = kActionEffect_MarahnaBossLightning;
    } else if (aitos_lava_map && IsAitosLavaFireball(&object)) {
      kind = kActionEffect_AitosLavaFireball;
      phase = kActionEffectPhase_AitosLavaFireballFlight;
    } else if (aitos_statue_fire_map && IsAitosStatueFire(&object)) {
      kind = kActionEffect_AitosStatueFire;
      phase = kActionEffectPhase_AitosStatueFireBreath;
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
    } else if (bloodpool_act2_map && IsLightningTrap(&object)) {
      kind = kActionEffect_LightningTrap;
      phase = kActionEffectPhase_LightningActive;
    } else if (boss_lightning_map &&
               SourceMatchesOriginalOrDeathHeimRoom(
                   wram, wram_size, object.source_descriptor,
                   kActRaiserMapGroup_Bloodpool, kBloodpoolBossMap,
                   kBossLightningSourceDescriptor,
                   kDeathHeimWizardMap,
                   kDeathHeimWizardSourceDescriptor) &&
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
    if (kind == kActionEffect_CentaurLightning || kind == kActionEffect_NorthwallBossMagic) {
      effect.obj_priority = ScenePriorityFromSpriteAttributeBias(wram, wram_size);
      /* Keep blink frames tracked without painting light over invisible art.
       * Native OAM has a one-pixel Y bias relative to the world hot point. */
      effect.geometry.data.rect.y0 -= 1;
      effect.geometry.data.rect.y1 -= 1;
      if (kind == kActionEffect_CentaurLightning) {
        if (object.visual == 0x18 ||
            (phase == kActionEffectPhase_CentaurStaffCharge && object.visual == 7))
          effect.flags &= ~kActionEffectFlag_Visible;
        if (phase == kActionEffectPhase_CentaurStaffCharge) {
          const float x = (effect.flags & kActionEffectFlag_FlipHorizontal) ? 24 : -24;
          effect.geometry.data.rect = (ActionEffectLocalRect){x - 8, -65, x + 8, -49};
        }
      }
    }
    if (bloodpool_boss_fireball || kind == kActionEffect_AitosStatueFire ||
        kind == kActionEffect_FillmoreStatueOrb ||
        kind == kActionEffect_FlamingWheel ||
        kind == kActionEffect_FlamingWheelProjectile) {
      /* These measured compositions carry raw priority zero in their part
       * words; $00:8D68 supplies the room's live OBJ band through $008F.
       * Death Heim is therefore free to select a different band without
       * changing or weakening the family identity. */
      effect.obj_priority = ScenePriorityFromSpriteAttributeBias(
          wram, wram_size);
    }
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
        const bool reflected = object.flip_attributes ==
            (kActRaiserObjectFlip_Horizontal |
             kActRaiserObjectFlip_Vertical);
        if (object.animation_state == 0x0001) {
          effect.geometry.data.rect = reflected
              ? (ActionEffectLocalRect){-16.0f, -9.0f, 8.0f, 15.0f}
              : (ActionEffectLocalRect){-8.0f, -17.0f, 16.0f, 7.0f};
        } else {
          effect.geometry.data.rect = reflected
              ? (ActionEffectLocalRect){-16.0f, -17.0f, 8.0f, 7.0f}
              : (ActionEffectLocalRect){-8.0f, -9.0f, 16.0f, 15.0f};
        }
      } else {
        /* Player crescents have raw part priority zero. Match $8D68's live
         * room bias, including band 2 in the Bloodpool Act 1 boss arena. */
        effect.obj_priority = ScenePriorityFromSpriteAttributeBias(wram, wram_size);
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
    const bool frozen = BeginOrAdvanceSceneTrack(
        observer, &observer->scene_tracks[slot], &object,
        kind, phase, elapsed_ticks, bloodpool_boss_fireball, &effect);
    if (bloodpool_boss_fireball) {
      /* Native boss shots keep flying after leaving the activation window.
       * Track those slots, but do not let invisible shots exhaust the shared
       * render list and suppress the player's sword beam during a long fight. */
      if (!(effect.flags & kActionEffectFlag_Visible)) continue;
      smoke_sources[slot] = frozen ? kFireballSmokeSourceFrozen : kFireballSmokeSourceMoving;
    }
    SceneFrameAppend(dst, &effect);
  }
  for (unsigned i = 0; i < kActionSceneEffectObserverTrackCount; i++)
    if (!seen[i])
      memset(&observer->scene_tracks[i], 0,
             sizeof(observer->scene_tracks[i]));

  AgeFireballSmoke(&observer->fireball_smoke, observer, smoke_sources, elapsed_ticks);
  for (unsigned i = 0; i < dst->effect_count; ++i) {
    const ActionEffectInstance *effect = &dst->effects[i];
    const unsigned slot = (effect->record_address - kActRaiserWram_ActionObjectTable) /
        kActRaiserActionObjectStride;
    if (smoke_sources[slot] == kFireballSmokeSourceMoving)
      EmitFireballSmoke(&observer->fireball_smoke, effect, elapsed_ticks);
  }

  if (dst->overflow) {
    dst->effect_count = 0;
    dst->visible_count = 0;
  }
  dst->fireball_smoke = observer->fireball_smoke;
  dst->fireball_smoke.clock = observer->scene_clock;
  dst->fireball_smoke.camera_delta_x = (int16_t)(
      Read16(wram, wram_size, kActRaiserWram_Bg1CameraX) -
      Read16(wram, wram_size, kActRaiserWram_Bg2CameraX));
  dst->fireball_smoke.camera_delta_y = (int16_t)(
      Read16(wram, wram_size, kActRaiserWram_Bg1CameraY) -
      Read16(wram, wram_size, kActRaiserWram_Bg2CameraY));
}

void ActionSceneEffects_CaptureFrame(ActionEffectObserver *observer,
                                     ActionSceneEffectFrame *dst,
                                     const uint8_t *wram, size_t wram_size,
                                     unsigned elapsed_ticks) {
  ActionSceneEffects_CaptureFrameFiltered(observer,dst,wram,wram_size,elapsed_ticks,true,NULL);
}
