#include "actraiser/actraiser_room_profiles.h"
#include "action_effects.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>   /* fprintf (AR_AITOS_WATERFALL_LOG) */
#include <stdlib.h>  /* getenv (AR_AITOS_WATERFALL_LOG) */
#include <string.h>

#include "actraiser_game.h"
#include "action_bg_plan.h"
#include "action_bg_world.h"
#include "action_landing_dust.h"
#include "action_bloodpool_surface.h"
#include "action_bloodpool_occluders.h"
#include "action_castle_sources.h"
#include "action_cave_surface.h"

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

static uint16_t Read16(const uint8_t *wram, size_t wram_size,
                       size_t address) {
  if (!wram || address + 1 >= wram_size) return 0;
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

static uint8_t Read8(const uint8_t *wram, size_t wram_size, size_t address) {
  if (!wram || address >= wram_size) return 0;
  return wram[address];
}

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
  kBloodpoolTorchTopMetatile = 0x47,
  kBloodpoolTorchBottomMetatile = 0x4F,
  kMarahnaFirstEffectMap = 0x04,
  kMarahnaLastEffectMap = 0x07,
  kMarahnaBossMap = 0x08,
  kMarahnaTorchMetatile = 0x43,
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
  kAitosLavaMap = 0x01,
  kAitosLavaLeftMetatile = 0xDC,
  kAitosLavaMiddleMetatile = 0xDD,
  kAitosLavaRightMetatile = 0xDE,
  kAitosLavaFillMetatile = 0xDF,
  kAitosLavaBubbleMetatile = 0xE7,
  kAitosLavaMaxMiddleCells = 6,
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
  kAitosSideLavaLipMetatile = 0x01,
  kAitosSideLavaBodyMetatile = 0x05,
  kAitosSideLavaFirstAnimatedMetatile = 0x02,
  kAitosSideLavaLastAnimatedMetatile = 0x04,
  kAitosSideLavaMap6AnimatedMetatile = 0x77,
  kAitosSideLavaMaxCells = 64,
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
  kDeathHeimViperMap = 0x06,
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

static bool IsAitosWaterfallMap(const uint8_t *wram, size_t wram_size) {
  return wram && ActRaiserRoom_ProfileFor(
      Read8(wram, wram_size, kActRaiserWram_MapGroup),
      Read8(wram, wram_size, kActRaiserWram_CurrentMap)) ==
          kActRaiserRoomProfile_AitosWaterfall;
}

static bool IsAitosAct2LavaMap(const uint8_t *wram, size_t wram_size) {
  return wram && (ActRaiserRoom_ProfileFor(
      Read8(wram, wram_size, kActRaiserWram_MapGroup),
      Read8(wram, wram_size, kActRaiserWram_CurrentMap)) == kActRaiserRoomProfile_AitosAct2Lava);
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
           kMarahnaBossMap) ||
      (Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
           kActRaiserMapGroup_DeathHeim &&
       Read8(wram, wram_size, kActRaiserWram_CurrentMap) ==
           kDeathHeimViperMap)) {
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

static bool IsFillmoreForest(const uint8_t *wram, size_t wram_size) {
  return wram && wram_size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Fillmore &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Height) == 768 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Width) == 2304 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Height) == 512;
}

static bool IsBloodpoolMarsh(const uint8_t *wram, size_t size) {
  return wram && size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, size, kActRaiserWram_MapGroup) == kActRaiserMapGroup_Bloodpool &&
      Read8(wram, size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, size, kActRaiserWram_Bg1Height) == 512 &&
      Read16(wram, size, kActRaiserWram_Bg2Width) == 256 &&
      Read16(wram, size, kActRaiserWram_Bg2Height) == 256;
}

static bool BloodpoolWaterTile(uint8_t tile) {
  switch (tile) {
    case 0x18: case 0x19: case 0x20: case 0x21: case 0x22:
    case 0x5E: case 0x9E: case 0x9F: case 0xA0: return true;
    default: return false;
  }
}

/* Merge scanline runs, then identical runs in adjacent rows. The 768x352
 * window includes the largest extended capture plus its horizontal apron.
 * Reads are bounded by the validated native map/definition table. */
static bool CaptureMoonOccluders(ActionMoonlightOcclusion *dst,
    const ActionBgMapView *map, const uint8_t *wram, size_t size,
    unsigned table, unsigned mask, unsigned attributes) {
  const int camera_x = (int16_t)Read16(wram,size,kActRaiserWram_Bg1CameraX);
  const int camera_y = (int16_t)Read16(wram,size,kActRaiserWram_Bg1CameraY);
  const int x0 = camera_x > 256 ? (camera_x-256)&~7 : 0;
  const int x1 = camera_x+512 < 4096 ? (camera_x+519)&~7 : 4096;
  const int y0 = camera_y > 64 ? camera_y-64 : 0;
  const int y1 = camera_y+288 < 512 ? camera_y+288 : 512;
  /* Alternating pixels are the worst case; rounding the window to whole CHR
   * tiles can add seven columns. No heap or full-resolution mask is needed. */
  uint16_t runs[2][388];
  uint16_t words[97];
  unsigned previous_count = 0;
  dst->count = 0;
  for (int y = y0; y < y1; y++) {
    if (y == y0 || !(y&7)) {
      for (int x = x0; x < x1; x += 8) {
        uint8_t metatile;
        uint16_t word = 0xFF;
        if (ActionBgMapView_LookupMetatile(map,x,y,&metatile)) {
          const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
          word = (Read16(wram,size,table+metatile*8+quadrant*2)&mask)|attributes;
        }
        words[(x-x0)/8] = word;
      }
    }
    uint16_t *previous = runs[(y-y0)&1], *current = runs[(y-y0+1)&1];
    unsigned count = 0, match = 0;
    int start = -1;
    for (int tile_x = x0; tile_x <= x1; tile_x += 8) {
      unsigned bits = 0;
      if (tile_x < x1) {
        const uint16_t word = words[(tile_x-x0)/8];
        unsigned row = (unsigned)y&7u;
        if (word&0x8000u) row = 7-row;
        if (!(word&0x2000u)) bits = (unsigned)(kBloodpoolTileOpacity[word&255u] >> (row*8))&255u;
        if (word&0x4000u) {
          bits = ((bits&0x55u)<<1)|((bits>>1)&0x55u);
          bits = ((bits&0x33u)<<2)|((bits>>2)&0x33u);
          bits = (bits<<4)|(bits>>4);
          bits &= 255u;
        }
      }
      if (bits == 255u) { if (start < 0) start = tile_x; continue; }
      if (!bits && start < 0) continue;
      for (int pixel = 0; pixel < 8; pixel++) {
        const int x = tile_x+pixel;
        if (bits&(1u<<pixel)) { if (start < 0) start = x; continue; }
        if (start < 0) continue;
        while (match < previous_count && dst->rectangles[previous[match]].x0 < start) match++;
        unsigned index;
        if (match < previous_count && dst->rectangles[previous[match]].x0 == start &&
            dst->rectangles[previous[match]].x1 == x) {
          index = previous[match++];
          dst->rectangles[index].y1 = (int16_t)(y+1);
        } else {
          if (dst->count == kActionMoonlightMaxOccluders) return false;
          index = dst->count++;
          dst->rectangles[index] = (ActionMoonlightOccluder){
            (int16_t)start,(int16_t)y,(int16_t)x,(int16_t)(y+1)};
        }
        if (count == sizeof(runs[0])/sizeof(runs[0][0])) return false;
        current[count++] = (uint16_t)index;
        start = -1;
      }
    }
    previous_count = count;
  }
  dst->valid = true;
  return true;
}

static void CaptureBloodpoolMoon(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const ActionBgMapView *foreground, const uint8_t *wram, size_t size) {
  ActionBgMapView sky;
  uint8_t top, bottom;
  const unsigned table = Read16(wram,size,0x52);
  const unsigned mask = Read16(wram,size,0x54), attributes = Read8(wram,size,0x6B) << 8;
  if (dst->decoration_count > kActionSceneDecorationMaxInstances-2 ||
      table > size || size-table < 2048 || mask != 0xECFF || attributes != 0x1000 ||
      !ActionBgMapView_Init(&sky,wram,size,256,256,Read16(wram,size,0x4A)) ||
      !ActionBgMapView_LookupMetatile(&sky,112,48,&top) || top != 0x3C ||
      !ActionBgMapView_LookupMetatile(&sky,112,64,&bottom) || bottom != 0x44)
    return;
  if (!CaptureMoonOccluders(&dst->moonlight,foreground,wram,size,table,mask,attributes)) {
    dst->moonlight.count = 0;
    dst->moonlight.valid = false;
    return;
  }
  for (unsigned family = 0; family < 2; family++) {
    const ActionEffectInstance effect = {
      .generation = 0xB1000002u+family, .pulse_generation = 0xB1000002u+family,
      .world_x = 112, .world_y = 62, .visual = 1,
      .age_ticks = observer->scene_clock, .phase_ticks = observer->scene_clock,
      .pulse_ticks = observer->scene_clock,
      .kind = family ? kActionEffect_BloodpoolMoonReflection : kActionEffect_BloodpoolMoonlight,
      .phase = kActionEffectPhase_BloodpoolEnvironment,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = kActionEffectRenderLayer_Bg2Plane,
      .projection_plane = kActionEffectProjectionPlane_Bg2,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
      .clip_rect = {-384,0,384,194},
    };
    (void)SceneDecorationAppend(dst,&effect);
  }
}

static bool BloodpoolPixel(const ActionBgMapView *map, const uint8_t *ram, size_t size,
    unsigned table, int x, int y, bool low_only) {
  uint8_t tile;
  if (!ActionBgMapView_LookupMetatile(map,x,y,&tile)) return false;
  const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
  const uint16_t word = (Read16(ram,size,table+tile*8+quadrant*2)&0xECFFu)|0x1000u;
  if (low_only && (word&0x2000u)) return false;
  const unsigned px = (word&0x4000u) ? 7-((unsigned)x&7u) : (unsigned)x&7u;
  const unsigned py = (word&0x8000u) ? 7-((unsigned)y&7u) : (unsigned)y&7u;
  return (kBloodpoolTileOpacity[word&255u] >> (py*8+px))&1u;
}

static bool BloodpoolExposedWater(int x, uint16_t sources) {
  for (unsigned i = 0; i < kBloodpoolWaterSpanCount; i++)
    if ((sources&(1u<<i)) && x >= kBloodpoolWaterSpans[i].left+6 &&
        x < kBloodpoolWaterSpans[i].right-6) return true;
  return false;
}

static bool BloodpoolTimberMaterial(uint8_t tile, const uint8_t *ram, size_t size,
    unsigned table) {
  /* Authored horizontal timber, including moss and support variants. Verify
   * definitions as well as map IDs before interpreting the CHR silhouette. */
  static const struct { uint8_t tile; uint16_t words[4]; } materials[] = {
    {0x51,{0x0A3A,0x0A3B,0x084A,0x084B}}, {0x52,{0x0A77,0x0A3B,0x00FF,0x084B}},
    {0x59,{0x0A8D,0x0A77,0x00FF,0x00FF}}, {0x5A,{0x0A77,0x0A78,0x00FF,0x0863}},
    {0x5B,{0x0A77,0x0A77,0x087A,0x00FF}}, {0x5C,{0x0A77,0x0A7D,0x087A,0x00FF}},
    {0x77,{0x0A77,0x0A77,0x00FF,0x00FF}}, {0x78,{0x0A77,0x0A8C,0x087A,0x00FF}},
    {0x79,{0x0A77,0x0A7D,0x00FF,0x00FF}},
  };
  for (unsigned i = 0; i < sizeof(materials)/sizeof(materials[0]); i++) {
    if (tile != materials[i].tile) continue;
    for (unsigned q = 0; q < 4; q++)
      if (Read16(ram,size,table+tile*8+q*2) != materials[i].words[q]) return false;
    return true;
  }
  return false;
}

static void CaptureBloodpoolDetails(ActionBloodpoolDetails *dst, const ActionBgMapView *map,
    const uint8_t *ram, size_t size, uint16_t sources) {
  dst->valid = dst->timber_count = dst->post_count = 0;
  const unsigned table = Read16(ram,size,0x52);
  const int camera = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraX);
  const int x0 = camera > 256 ? (camera-256)&~15 : 0;
  const int x1 = camera+512 < 4096 ? (camera+527)&~15 : 4096;
  for (int y = 0; y < 480; y += 16) for (int x = x0; x < x1; x += 16) {
    uint8_t tile;
    if (!ActionBgMapView_LookupMetatile(map,x,y,&tile) ||
        !BloodpoolTimberMaterial(tile,ram,size,table)) continue;
    int start = 0, best_start = 0, best_length = 0;
    for (int p = 0; p <= 16; p++) {
      if (p < 16 && BloodpoolPixel(map,ram,size,table,x+p,y,true) &&
          !BloodpoolPixel(map,ram,size,table,x+p,y-1,false)) continue;
      if (p-start > best_length) { best_start = start; best_length = p-start; }
      start = p+1;
    }
    if (best_length < 4) continue;
    if (dst->timber_count == kActionBloodpoolMaxTimber) return;
    ActionBloodpoolTimber *edge = &dst->timber[dst->timber_count++];
    *edge = (ActionBloodpoolTimber){.x0=(int16_t)(x+best_start),
      .x1=(int16_t)(x+best_start+best_length),.y=(int16_t)y};
    edge->drip_x = (int16_t)(edge->x0+1+((x/16+y/16*7)%(best_length-2)));
    int bottom = y;
    while (bottom < y+16 && BloodpoolPixel(map,ram,size,table,edge->drip_x,bottom,false)) bottom++;
    edge->drip_y = edge->landing_y = (int16_t)bottom;
    if (bottom == y+16) continue; /* Supporting post: no invented underside. */
    int landing = bottom+1;
    while (landing < 480 && !BloodpoolPixel(map,ram,size,table,edge->drip_x,landing,false))
      landing++;
    edge->water_landing = landing == 480 && BloodpoolExposedWater(edge->drip_x,sources);
    edge->landing_y = (int16_t)(edge->water_landing ? 488 : landing);
  }
  int start = -1;
  for (int x = x0; x <= x1; x++) {
    uint8_t water_tile = 0;
    const bool post_tile = ActionBgMapView_LookupMetatile(map,x,480,&water_tile) &&
        (water_tile == 0x21 || water_tile == 0x22 || water_tile == 0x5E || water_tile == 0xA0);
    if (x < x1 && BloodpoolExposedWater(x,sources) &&
        post_tile && BloodpoolPixel(map,ram,size,table,x,479,true)) {
      if (start < 0) start = x;
      continue;
    }
    if (start < 0) continue;
    if (x-start <= 20 && start > x0 && x < x1) {
      if (dst->post_count == kActionBloodpoolMaxPosts) return;
      const int center = (start+x)/2;
      if (dst->post_count && center-dst->posts[dst->post_count-1] < 10)
        dst->posts[dst->post_count-1] = (int16_t)((center+dst->posts[dst->post_count-1])/2);
      else
        dst->posts[dst->post_count++] = (int16_t)center;
    }
    start = -1;
  }
  dst->valid = true;
}

static void CaptureBloodpoolMarsh(ActionEffectObserver *observer,
    ActionSceneEffectFrame *dst, const uint8_t *wram, size_t size) {
  if (observer->scene_map_number != 1 || !IsBloodpoolMarsh(wram, size)) return;
  ActionBgMapView map;
  uint8_t bank, timber;
  if (!ActionBgMapView_Init(&map, wram, size, 4096, 512,
          Read16(wram, size, kActRaiserWram_BgMapPage)) ||
      !ActionBgMapView_LookupMetatile(&map, 0, 432, &bank) || bank != 0x8A ||
      !ActionBgMapView_LookupMetatile(&map, 736, 320, &timber) || timber != 0xB9)
    return; /* Reject inherited maps during entry/room changes. */
  uint16_t sources = 0;
  for (unsigned i = 0; i < kBloodpoolWaterSpanCount; i++) {
    bool valid = true;
    for (int x = kBloodpoolWaterSpans[i].left; x < kBloodpoolWaterSpans[i].right; x += 16) {
      uint8_t tile;
      if (!ActionBgMapView_LookupMetatile(&map, x, 480, &tile) || !BloodpoolWaterTile(tile)) {
        valid = false;
        break;
      }
    }
    if (valid) sources |= (uint16_t)(1u << i);
  }
  if (!sources || dst->decoration_count > kActionSceneDecorationMaxInstances - 2) return;
  const int x = (int16_t)Read16(wram, size, kActRaiserWram_Bg1CameraX) + 128;
  for (unsigned family = 0; family < 2; family++) {
    const ActionEffectInstance effect = {
      .generation = 0xB1000000u | family, .pulse_generation = 0xB1000000u | family,
      .world_x = (int16_t)x, .world_y = 480, .visual = 1, .source_mask = sources,
      .age_ticks = observer->scene_clock, .phase_ticks = observer->scene_clock,
      .pulse_ticks = observer->scene_clock,
      .kind = family ? kActionEffect_BloodpoolMist : kActionEffect_BloodpoolWater,
      .phase = kActionEffectPhase_BloodpoolEnvironment,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = family ? kActionEffectRenderLayer_Bg2HighAlpha :
                              kActionEffectRenderLayer_Bg1HighPlane,
      .projection_plane = family ? kActionEffectProjectionPlane_Bg1 :
                                  kActionEffectProjectionPlane_Bg1High,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-48,384,32}},
      .clip_rect = {-(float)x,-48,4096.0f-x,32},
    };
    (void)SceneDecorationAppend(dst, &effect);
  }
  CaptureBloodpoolMoon(observer,dst,&map,wram,size);
  if (!dst->moonlight.valid || dst->decoration_count > kActionSceneDecorationMaxInstances-3)
    return;
  CaptureBloodpoolDetails(&dst->bloodpool,&map,wram,size,sources);
  const ActionEffectInstance moon = dst->decorations[dst->decoration_count-2];
  for (unsigned family = 0; family < 3; family++) {
    ActionEffectInstance detail = moon;
    detail.kind = (uint8_t)(kActionEffect_BloodpoolTimber+family);
    detail.generation = detail.pulse_generation = 0xB1000004u+family;
    detail.world_x = (int16_t)x;
    detail.world_y = 0;
    detail.source_mask = sources;
    detail.render_layer = family == 0 ? kActionEffectRenderLayer_Bg1Plane :
        family == 1 ? kActionEffectRenderLayer_Bg2HighAlpha : kActionEffectRenderLayer_Bg2Alpha;
    detail.projection_plane = family == 2 ? kActionEffectProjectionPlane_Bg2 :
        kActionEffectProjectionPlane_Bg1;
    detail.geometry.data.rect = detail.clip_rect = (ActionEffectLocalRect){-384,0,384,512};
    if (family == 2) {
detail.world_x = 112;
      detail.world_y = 62;
      detail.geometry.data.rect = detail.clip_rect = (ActionEffectLocalRect){-100,-40,100,44};
    }
    (void)SceneDecorationAppend(dst,&detail);
  }
}

static unsigned BloodpoolCastleRoom(const uint8_t *ram, size_t size) {
  if (!IsBloodpoolAct2Map(ram,size)) return 0;
  const unsigned room = Read8(ram,size,kActRaiserWram_CurrentMap);
  static const uint16_t dimensions[][4] = {
    {768,512,256,256}, {1024,1024,256,256}, {512,512,256,256},
    {1792,1024,1792,1024}, {768,256,256,256}, {1024,1024,256,256}, {256,256,256,256},
  };
  const uint16_t *d = dimensions[room-2];
  return Read16(ram,size,kActRaiserWram_Bg1Width) == d[0] &&
      Read16(ram,size,kActRaiserWram_Bg1Height) == d[1] &&
      Read16(ram,size,kActRaiserWram_Bg2Width) == d[2] &&
      Read16(ram,size,kActRaiserWram_Bg2Height) == d[3] ? room : 0;
}

static bool CastleTileIs(const ActionBgMapView *map, int x, int y, uint8_t expected) {
  uint8_t tile;
  return ActionBgMapView_LookupMetatile(map,x,y,&tile) && tile == expected;
}

static void CaptureCastleWater(ActionSceneEffectFrame *dst, const uint8_t *ram,
    size_t size, const ActionEffectInstance *castle) {
  ActionBgMapView water;
  /* Room 5 blends BG2 water into the resolved BG1 masonry. Both maps scroll
   * together; light the resolved scenery rather than an occluded rear plane. */
  if (Read16(ram,size,kActRaiserWram_Bg1CameraX) != Read16(ram,size,kActRaiserWram_Bg2CameraX) ||
      Read16(ram,size,kActRaiserWram_Bg1CameraY) != Read16(ram,size,kActRaiserWram_Bg2CameraY)) return;
  if (!ActionBgMapView_Init(&water,ram,size,1792,1024,
          Read16(ram,size,kActRaiserWram_BgMapPage+kActRaiserBgLayerStateStride))) return;
  const unsigned table = Read16(ram,size,kActRaiserWram_BgMetatileTable+kActRaiserBgLayerStateStride);
  static const uint16_t surface[] = {0x0470,0x0471,0x0402,0x0403};
  for (unsigned q = 0; q < 4; q++)
    if (Read16(ram,size,table+0x15*8+q*2) != surface[q]) return;
  ActionEffectInstance effect = *castle;
  effect.kind = kActionEffect_CastleWater;
  effect.render_layer = kActionEffectRenderLayer_Bg1Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg1;
  effect.world_x = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraX)+128;
  effect.world_y = kActionCastleWaterSurface;
  effect.geometry.data.rect = (ActionEffectLocalRect){-384,0,384,16};
  effect.clip_rect = (ActionEffectLocalRect){kActionCastleWaterLeft-effect.world_x,0,
      kActionCastleWaterRight-effect.world_x,16};
  effect.source_mask = 0;
  for (unsigned strip = 0; strip < kActionCastleWaterStripCount; strip++) {
    const int left = kActionCastleWaterLeft+strip*kActionCastleWaterStripWidth;
    bool valid = true;
    for (int x = left; valid && x < left+kActionCastleWaterStripWidth; x += 16)
      valid = CastleTileIs(&water,x,kActionCastleWaterSurface,0x15) &&
          CastleTileIs(&water,x,kActionCastleWaterSurface-16,0);
    if (valid) effect.source_mask |= (uint16_t)(1u<<strip);
  }
  if (effect.source_mask) (void)SceneDecorationAppend(dst,&effect);
}

static void CaptureBloodpoolCastle(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
  const unsigned room = BloodpoolCastleRoom(ram,size);
  if (!room || observer->scene_map_number != room ||
      dst->decoration_count > kActionSceneDecorationMaxInstances-3) return;
  const unsigned width = Read16(ram,size,kActRaiserWram_Bg1Width);
  const unsigned height = Read16(ram,size,kActRaiserWram_Bg1Height);
  ActionBgMapView map;
  static const uint8_t witnesses[][2] = {
    {0x08,0x08}, {0x17,0x23}, {0x09,0xFA}, {0x09,0xA3},
    {0x09,0x23}, {0x09,0x09}, {0x77,0x09},
  };
  if (!ActionBgMapView_Init(&map,ram,size,width,height,
          Read16(ram,size,kActRaiserWram_BgMapPage)) ||
      !CastleTileIs(&map,0,(int)height-16,witnesses[room-2][0]) ||
      !CastleTileIs(&map,(int)width-16,0,witnesses[room-2][1])) return;
  /* Check decoded shared window definitions too: same map ids with replaced
   * graphics must not silently become light emitters. No ROM address coupling. */
  const unsigned table = Read16(ram,size,kActRaiserWram_BgMetatileTable);
  static const uint16_t window[] = {0x04EE,0x44EE,0x04FE,0x44FE};
  for (unsigned q = 0; q < 4; q++)
    if (Read16(ram,size,table+0x44*8+q*2) != window[q]) return;
  const int x = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraX)+128;
  const int y = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraY)-160;
  ActionEffectInstance effect = {
    .generation = 0xCA000000u|room, .pulse_generation = 0xCB000000u|room,
    .world_x = (int16_t)x, .world_y = (int16_t)y, .visual = (uint16_t)room,
    .age_ticks = observer->scene_clock, .phase_ticks = observer->scene_clock,
    .pulse_ticks = observer->scene_clock,
    .kind = kActionEffect_CastleLight, .phase = kActionEffectPhase_CastleEnvironment,
    .flags = kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
    .clip_rect = {-x,-y,(float)width-x,(float)height-y},
  };
  unsigned index = 0;
  for (unsigned i = 0; i < kActionCastleSourceCount; i++) {
    const ActionCastleSource *s = &kActionCastleSources[i];
    if (s->room != room) continue;
    if (index >= 16) return;
    if (CastleTileIs(&map,s->check_x,s->check_y,s->top_tile) &&
        CastleTileIs(&map,s->check_x,s->check_y+16,s->below_tile) &&
        (s->diffuse || CastleTileIs(&map,s->x,s->sill-1,s->sill_tile)))
      effect.source_mask |= (uint16_t)(1u<<index);
    index++;
  }
  if (effect.source_mask) (void)SceneDecorationAppend(dst,&effect);

  /* Low haze has an actual supporting surface across its full span. Never
   * reuse a single floor height over a pit or newly replaced collision map. */
  static const uint16_t floors[][3] = {
    {32,608,480}, {0,0,0}, {176,336,464}, {928,1504,976},
    {160,640,208}, {352,816,208}, {32,224,224},
  };
  const uint16_t *floor = floors[room-2];
  bool supported = floor[2] != 0 && size > 0x06A0;
  for (int fx = floor[0]; supported && fx < floor[1]; fx += 16) {
    uint8_t above, below;
    supported = ActionBgMapView_LookupMetatile(&map,fx,floor[2]-1,&above) &&
        ActionBgMapView_LookupMetatile(&map,fx,floor[2],&below) &&
        ram[0x05A0+below] == 15 && ram[0x05A0+above] == 0 &&
        above != 0x03 && above != 0x04 && above != 0x46 && above != 0x4E;
  }
  if (supported) {
    ActionEffectInstance mist = effect;
    mist.kind = kActionEffect_CastleMist;
    mist.render_layer = kActionEffectRenderLayer_Bg1Mist;
    mist.world_x = (int16_t)floor[0];
    mist.world_y = (int16_t)floor[2];
    mist.source_mask = 0;
    mist.geometry.data.rect = (ActionEffectLocalRect){0,-18,floor[1]-floor[0],0};
    mist.clip_rect = mist.geometry.data.rect;
    (void)SceneDecorationAppend(dst,&mist);
  }
  if (room == 5) {
    CaptureCastleWater(dst,ram,size,&effect);
    return; /* Interior BG2 has water, but no moon/sky artwork. */
  }
  ActionBgMapView sky;
  if (!ActionBgMapView_Init(&sky,ram,size,256,256,
          Read16(ram,size,kActRaiserWram_BgMapPage+kActRaiserBgLayerStateStride)) ||
      !CastleTileIs(&sky,112,48,room < 6 ? 0x3C : 0x10) ||
      !CastleTileIs(&sky,128,64,room < 6 ? 0x45 : 0x05)) return;
  effect.kind = kActionEffect_CastleSky;
  effect.render_layer = kActionEffectRenderLayer_Bg2Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg2;
  effect.world_x = room < 6 ? 112 : 128;
  effect.world_y = room < 6 ? 62 : 48;
  effect.source_mask = 0;
  effect.geometry.data.rect = (ActionEffectLocalRect){-144,-48,144,208};
  effect.clip_rect = (ActionEffectLocalRect){-effect.world_x,-effect.world_y,
      256-effect.world_x,256-effect.world_y};
  if (room == 2 || room == 6 || room == 7) {
    /* Exterior rays spread beyond the moon's native 256px sky map, just as
     * in Act 1. The presentation plane supplies the actual visible bounds. */
    effect.geometry.data.rect = effect.clip_rect =
        (ActionEffectLocalRect){-384,-48,384,194};
  }
  (void)SceneDecorationAppend(dst,&effect);
}

static unsigned FillmoreCaveRoom(const uint8_t *wram, size_t size) {
  if (!wram || size <= kActRaiserWram_Bg2Height + 1 ||
      Read8(wram, size, kActRaiserWram_MapGroup) != kActRaiserMapGroup_Fillmore)
    return 0;
  const unsigned room = Read8(wram, size, kActRaiserWram_CurrentMap);
  if (room < 2 || room > 4) return 0;
  static const unsigned dimensions[][4] = {
    {2048,1280,2048,1280}, {1024,1792,256,512}, {512,256,256,256},
  };
  const unsigned *d = dimensions[room - 2];
  return Read16(wram, size, kActRaiserWram_Bg1Width) == d[0] &&
      Read16(wram, size, kActRaiserWram_Bg1Height) == d[1] &&
      Read16(wram, size, kActRaiserWram_Bg2Width) == d[2] &&
      Read16(wram, size, kActRaiserWram_Bg2Height) == d[3] ? room : 0;
}

bool ActionSceneEffects_RoomUsesBg1Decorations(
    const uint8_t *wram, size_t wram_size) {
  WallTorchMapRule rule;
  const unsigned cave_room = FillmoreCaveRoom(wram, wram_size);
  /* Room 3 uses the winner mask for scenery dimming throughout the climb,
   * as well as the floor mist. Room 2 uses it for damp-stone highlights. */
  return cave_room == 2 || cave_room == 3 || IsBloodpoolMarsh(wram, wram_size) ||
      WallTorchRuleFor(wram, wram_size, &rule);
}

static bool CaveMapReady(const ActionBgMapView *map, unsigned room) {
  /* Distinct stone/ceiling pairs in the decoded maps, not ROM addresses or
   * palette guesses. Reject a transition's inherited or incomplete map. */
  static const unsigned signatures[][6] = {
    {326,352,0xB8, 420,432,0xB9},
    {896,128,0x39, 448,1664,0x26},
    {80,80,0x08, 96,192,0x26},
  };
  const unsigned *s = signatures[room - 2];
  uint8_t a, b;
  return ActionBgMapView_LookupMetatile(map, (int)s[0], (int)s[1], &a) && a == s[2] &&
      ActionBgMapView_LookupMetatile(map, (int)s[3], (int)s[4], &b) && b == s[5];
}

_Static_assert(kActionLandingDustMaxPuffs + 3 + kActionTempleMistMaxSpans <=
                   kActionSceneDecorationMaxInstances,
               "temple floor mist must leave room for landing dust and ambient fields");

static int TempleMistFloor(const uint8_t *wram, const ActionBgMapView *map, int x) {
  /* Search only the lower hall. The native collision LUT, rather than the
   * decorative ledge pixels, determines the supporting surface in each column. */
  for (int y = 1664; y <= 1712; y += 16) {
    uint8_t below, above;
    if (!ActionBgMapView_LookupMetatile(map, x, y, &below) ||
        !ActionBgMapView_LookupMetatile(map, x, y-1, &above)) return 0;
    if (above == 0x18 || above == 0x20) return 0; /* Never cover spike pits. */
    const unsigned collision = wram[0x05A0 + below];
    const bool capital = below >= 0x54 && below <= 0x57 && collision == 3;
    if ((collision == 15 || capital) && wram[0x05A0 + above] == 0) return y;
  }
  return 0;
}

static void CaptureTempleMist(ActionSceneEffectFrame *dst, const uint8_t *wram,
    const ActionBgMapView *map, uint16_t clock) {
  const uint8_t count_before = dst->decoration_count;
  const uint8_t visible_before = dst->decoration_visible_count;
  int left = 512, floor = 0;
  unsigned spans = 0;
  for (int x = 512; x <= 960; x += 16) {
    const int next_floor = x < 960 ? TempleMistFloor(wram, map, x) : 0;
    if (next_floor == floor) continue;
    if (floor) {
      /* A changed/fragmented map may exceed the cosmetic budget. Omit only
       * this family instead of consuming actor or landing-cloud records. */
      if (++spans > kActionTempleMistMaxSpans ||
          dst->decoration_count >= kActionSceneDecorationMaxInstances) {
        dst->decoration_count = count_before;
        dst->decoration_visible_count = visible_before;
        return;
      }
      const ActionEffectInstance effect = {
        .generation = 0xC3000000u | (unsigned)left,
        .pulse_generation = 0xD3000000u | (unsigned)left,
        .world_x = (int16_t)left, .world_y = (int16_t)floor, .visual = 3,
        .age_ticks = clock, .phase_ticks = clock, .pulse_ticks = clock,
        .kind = kActionEffect_TempleGroundMist, .phase = kActionEffectPhase_CaveEnvironment,
        .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
        .render_layer = kActionEffectRenderLayer_Bg1Mist,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,-26,x-left,0}},
        .clip_rect = {0,-26,x-left,0},
      };
      (void)SceneDecorationAppend(dst, &effect);
    }
    left = x;
    floor = next_floor;
  }
}

_Static_assert(kActionCaveWetSourceCount <= 16, "cave sources must fit the captured mask");

static uint16_t CaveWetSourceMask(const ActionBgMapView *map) {
  uint16_t mask = 0;
  for (unsigned i = 0; i < kActionCaveWetSourceCount; i++) {
    const ActionCaveWetSource *s = &kActionCaveWetSources[i];
    uint8_t tip, landing;
    if (ActionBgMapView_LookupMetatile(map, s->x, s->ceiling_y-1, &tip) &&
        ActionBgMapView_LookupMetatile(map, s->x, s->landing_y, &landing) &&
        tip == s->ceiling_tile && landing == s->landing_tile)
      mask |= (uint16_t)(1u << i);
  }
  return mask;
}

static void CaptureFillmoreCave(ActionEffectObserver *observer,
    ActionSceneEffectFrame *dst, const uint8_t *wram, size_t size) {
  const unsigned room = FillmoreCaveRoom(wram, size);
  if (!room || observer->scene_map_number != room) {
    memset(&observer->landing_dust, 0, sizeof(observer->landing_dust));
    return;
  }
  const unsigned width = Read16(wram, size, kActRaiserWram_Bg1Width);
  const unsigned height = Read16(wram, size, kActRaiserWram_Bg1Height);
  ActionBgMapView map;
  if (!ActionBgMapView_Init(&map, wram, size, width, height,
          Read16(wram, size, kActRaiserWram_BgMapPage)) || !CaveMapReady(&map, room)) {
    memset(&observer->landing_dust, 0, sizeof(observer->landing_dust));
    return;
  }
  const ActionBgMapView playfield = map;
  if (room == 2) {
    uint8_t pool, fall;
    if (!ActionBgMapView_Init(&map, wram, size, 2048, 1280,
            Read16(wram, size, kActRaiserWram_BgMapPage + kActRaiserBgLayerStateStride)) ||
        !ActionBgMapView_LookupMetatile(&map, 0, 896, &pool) || pool != 1 ||
        !ActionBgMapView_LookupMetatile(&map, 720, 0, &fall) || fall != 2) {
      memset(&observer->landing_dust, 0, sizeof(observer->landing_dust));
      return;
    }
  }
  ActionLandingDust_Capture(&observer->landing_dust, dst, wram, size,
      &playfield, room, observer->scene_clock);
  const uint16_t wet_sources = room == 2 ? CaveWetSourceMask(&playfield) : 0;
  /* Aggregate fields cap this family at seven records, independent of how
   * many water tiles/emitters exist. Actor slots and their budget are untouched. */
  enum { cave = 1u << 2, temple = 1u << 3, tower = 1u << 4 };
  static const struct {
    uint8_t kind, rooms, layer, plane;
  } fields[] = {
    {kActionEffect_CaveWater, cave,
     kActionEffectRenderLayer_Bg2HighPlane, kActionEffectProjectionPlane_Bg2High},
    {kActionEffect_CaveDrips, cave,
     kActionEffectRenderLayer_WorldOverlay, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TempleDust, cave | temple | tower,
     kActionEffectRenderLayer_WorldOverlay, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TowerWindowLight, tower,
     kActionEffectRenderLayer_ForegroundLight, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_CaveMist, cave,
     kActionEffectRenderLayer_WorldDust, kActionEffectProjectionPlane_Bg2High},
    {kActionEffect_CaveSheen, cave,
     kActionEffectRenderLayer_Bg1Plane, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_CaveAmbientLight, cave | temple,
     kActionEffectRenderLayer_ForegroundLight, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TempleGrit, cave | temple,
     kActionEffectRenderLayer_WorldDust, kActionEffectProjectionPlane_Bg1},
  };
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    if (!(fields[i].rooms & (1u << room))) continue;
    const uint8_t kind = fields[i].kind;
    const bool water = fields[i].plane == kActionEffectProjectionPlane_Bg2High;
    const int x = (int16_t)Read16(wram, size, water ? kActRaiserWram_Bg2CameraX :
                                                           kActRaiserWram_Bg1CameraX) + 128;
    const int y = (int16_t)Read16(wram, size, water ? kActRaiserWram_Bg2CameraY :
                                                           kActRaiserWram_Bg1CameraY) - 160;
    const ActionEffectInstance effect = {
      .generation = 0xC2000000u | (room << 8) | kind,
      .pulse_generation = 0xD2000000u | (room << 8) | kind,
      .world_x = (int16_t)x, .world_y = (int16_t)y, .visual = (uint16_t)room,
      .age_ticks = observer->scene_clock, .phase_ticks = observer->scene_clock,
      .pulse_ticks = observer->scene_clock,
      .kind = kind, .phase = kActionEffectPhase_CaveEnvironment,
      .source_mask = (kind == kActionEffect_CaveDrips || kind == kActionEffect_CaveSheen) ?
          wet_sources : 0,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = fields[i].layer,
      .projection_plane = fields[i].plane,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
      .clip_rect = {-(float)x, -(float)y, (float)width - x, (float)height - y},
    };
    if (!SceneDecorationAppend(dst, &effect)) {
      dst->decoration_count = dst->decoration_visible_count = 0;
      return;
    }
  }
  if (room == 3) CaptureTempleMist(dst, wram, &playfield, observer->scene_clock);
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

void ActionEnvironmentalEffects_CaptureFrame(
    ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t wram_size) {
  /* Environmental fields are optional, bounded records. Their observers
   * continue keeping scene time while this capture is gated off. */
  if (!observer || !dst || !observer->scene_clock_valid ||
      !observer->scene_map_valid ||
      dst->decoration_overflow)
    return;
  if (observer->scene_map_group == kActRaiserMapGroup_Bloodpool) {
    CaptureBloodpoolMarsh(observer, dst, wram, wram_size);
    CaptureBloodpoolCastle(observer, dst, wram, wram_size);
    return;
  }
  if (observer->scene_map_group != kActRaiserMapGroup_Fillmore) return;
  if (observer->scene_map_number != 1) {
    CaptureFillmoreCave(observer, dst, wram, wram_size);
    return;
  }
  if (!IsFillmoreForest(wram, wram_size)) return;
  /* Forest light uses the mean BG1/BG2 camera for independent parallax;
   * authored sources stay in world space and the origin only culls work. */
  ActionBgMapView map;
  if (!ActionBgMapView_Init(&map, wram, wram_size, 2304, 512,
          Read16(wram, wram_size, kActRaiserWram_BgMapPage +
              kActRaiserBgLayerStateStride)))
    return;
  uint8_t tile, below;
  if (!ActionBgMapView_LookupMetatile(&map, 144, 112, &tile) ||
      !ActionBgMapView_LookupMetatile(&map, 144, 128, &below) ||
      tile != 0x0F || below != 0x01)
    return; /* Room signature only; these are not light-source positions. */
  const int bg2_x = (int16_t)Read16(wram, wram_size, kActRaiserWram_Bg2CameraX);
  const int bg2_y = (int16_t)Read16(wram, wram_size, kActRaiserWram_Bg2CameraY);
  const int cx = ((int16_t)Read16(wram, wram_size, kActRaiserWram_Bg1CameraX) +
                  (int16_t)Read16(wram, wram_size, kActRaiserWram_Bg2CameraX)) / 2;
  const int cy = ((int16_t)Read16(wram, wram_size, kActRaiserWram_Bg1CameraY) +
                  (int16_t)Read16(wram, wram_size, kActRaiserWram_Bg2CameraY)) / 2;
  const int x = cx + 128;
  const ActionEffectInstance effect = {
    .generation = 0x46000000u,
    .pulse_generation = 0x66000000u,
    .world_x = (int16_t)x, .world_y = (int16_t)(cy - 160),
    .age_ticks = observer->scene_clock,
    .phase_ticks = observer->scene_clock,
    .pulse_ticks = observer->scene_clock,
    .kind = kActionEffect_ForestCanopyLight,
    .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect,
                 .data.rect = {-384, 0, 384, 544}},
    .clip_rect = {cx - bg2_x - x, 160 - bg2_y,
                  2304 + cx - bg2_x - x, 512 + 160 - bg2_y},
  };
  ActionEffectInstance leaves = effect;
  leaves.kind = kActionEffect_ForestLeaves;
  leaves.render_layer = kActionEffectRenderLayer_Bg2Alpha;
  ActionEffectInstance forward = effect;
  forward.kind = kActionEffect_ForestForwardLight;
  forward.render_layer = kActionEffectRenderLayer_ForegroundLight;
  if (!SceneDecorationAppend(dst, &effect) || !SceneDecorationAppend(dst, &leaves) ||
      !SceneDecorationAppend(dst, &forward))
    dst->decoration_count = dst->decoration_visible_count = 0;
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
        .render_layer = kActionEffectRenderLayer_Bg1Plane,
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

static bool IsAitosSideLavaAnimatedCell(uint8_t metatile) {
  return (metatile >= kAitosSideLavaFirstAnimatedMetatile &&
          metatile <= kAitosSideLavaLastAnimatedMetatile) ||
      metatile == kAitosSideLavaMap6AnimatedMetatile;
}

static bool IsAitosSideLavaBankPair(uint8_t left, uint8_t right) {
  return (left == 0x33 && right == 0x34) ||
      (left == 0x2C && right == 0x32) ||
      (left == 0x33 && right == 0x32);
}

/* Act 2 turns the isometric pit mouths into broad side-on lakes. Their exact
 * semantic is a maximal $01 lip run, one of the measured bank pairs, an
 * animated/transparent surface row immediately above, and lava body below.
 * Lighting is anchored to the lip rather than the red volume: using the full
 * lake depth as an emitter would put sparks hundreds of pixels underwater. */
static void CaptureAitosSideLavaReservoirs(
    ActionSceneEffectFrame *dst, const uint8_t *wram,
    size_t wram_size, uint16_t clock) {
  if (!dst || !IsAitosAct2LavaMap(wram, wram_size)) return;
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
    if (y < kActionBgMetatilePixels ||
        y + kActionBgMetatilePixels >= map.world_height)
      continue;
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      if (x < kActionBgMetatilePixels) continue;
      uint8_t metatile = 0, left_bank = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != kAitosSideLavaLipMetatile)
        continue;

      /* If the camera window begins inside a very wide lake, walk left to its
       * authentic bank once. Later in-window cells see a lip immediately to
       * their left and skip as duplicates. Without this, map $06's 640px lake
       * lost all heat whenever its left bank sat beyond the scan margin. */
      unsigned run_x = x;
      unsigned walked = 0;
      if (x == bounds.x0) {
        while (run_x >= 2u * kActionBgMetatilePixels &&
               walked < kAitosSideLavaMaxCells) {
          uint8_t previous = 0;
          if (!ActionBgMapView_LookupMetatile(
                  &map, (int)(run_x - kActionBgMetatilePixels),
                  (int)y, &previous) ||
              previous != kAitosSideLavaLipMetatile)
            break;
          run_x -= kActionBgMetatilePixels;
          walked++;
        }
        if (walked == kAitosSideLavaMaxCells) continue;
      }
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)(run_x - kActionBgMetatilePixels),
               (int)y, &left_bank) ||
          left_bank == kAitosSideLavaLipMetatile)
        continue;

      unsigned cells = 0;
      bool surface_valid = true;
      bool has_animated_surface = false;
      bool has_lava_body = false;
      for (; cells < kAitosSideLavaMaxCells; cells++) {
        const unsigned cell_x = run_x + cells * kActionBgMetatilePixels;
        if (cell_x < run_x || cell_x >= map.world_width ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile) ||
            metatile != kAitosSideLavaLipMetatile)
          break;
        uint8_t surface = 0, body = 0;
        if (!ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y - kActionBgMetatilePixels), &surface) ||
            (surface != 0 && !IsAitosSideLavaAnimatedCell(surface)) ||
            !ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y + kActionBgMetatilePixels), &body)) {
          surface_valid = false;
          break;
        }
        has_animated_surface |= IsAitosSideLavaAnimatedCell(surface);
        has_lava_body |= body == kAitosSideLavaBodyMetatile;
      }
      if (!surface_valid || cells < 3 || !has_animated_surface ||
          !has_lava_body || cells == kAitosSideLavaMaxCells)
        continue;
      const unsigned right_x = run_x + cells * kActionBgMetatilePixels;
      uint8_t right_bank = 0;
      if (right_x < run_x || right_x >= map.world_width ||
          !ActionBgMapView_LookupMetatile(
              &map, (int)right_x, (int)y, &right_bank) ||
          !IsAitosSideLavaBankPair(left_bank, right_bank))
        continue;

      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(run_x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = 0x4A000000u ^ identity,
        .pulse_generation = 0x6A000000u ^ identity,
        .world_x = (int16_t)(run_x + width / 2u),
        /* The visible orange lip occupies the top half of the $01 row. */
        .world_y = (int16_t)(y + 4u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = 4,
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = 4,
        .age_ticks = clock,
        .phase_ticks = clock,
        .pulse_ticks = clock,
        .kind = kActionEffect_AitosLavaReservoir,
        .phase = kActionEffectPhase_AitosLavaReservoir,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_Bg1HighPlane,
        .projection_plane = kActionEffectProjectionPlane_Bg1High,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -4.0f, half_width, 4.0f},
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

/* AR_AITOS_WATERFALL_LOG=1: the veil appended below is what publishes the
 * `waterfall` section token (action_effect_capture.c), which in turn admits the
 * folded BG2 continuation (diorama.c). Those three live in different files, so
 * a capture-side dropout presents as a rendering bug at the far end and costs a
 * session to trace. Log the capture decision on CHANGE only, so a jump reads as
 * a handful of lines instead of 60 per second. */
static void AitosWaterfallLog(const ActionSceneEffectFrame *dst,
                              const SceneBgScanBounds *bounds,
                              int camera_x, int camera_y,
                              unsigned splash_count, bool published) {
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
          bounds->x0, bounds->x1, bounds->y0, bounds->y1,
          splash_count, published ? "yes" : "no");
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
  if (!splash_count || dst->decoration_overflow) {
    AitosWaterfallLog(dst, &bounds, camera_x, camera_y, splash_count, false);
    return;
  }
  AitosWaterfallLog(dst, &bounds, camera_x, camera_y, splash_count, true);

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
    .bottom_extent = 312,
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
      /* Continue the veil through one 224-row overflow repeat. The lower
       * bound is chosen so the particle field ends at screen row 448 after
       * its 24px travel margin: authentic bottom 224 + one repeat 224. */
      .data.rect = {-256.0f, -176.0f, 256.0f, 312.0f},
    },
  };
  if (!SceneDecorationAppend(dst, &waterfall)) return;

  /* `$04/$02` intentionally keeps BG2's vertical extension short: allowing
   * more raw-wrap rows repeats water into non-water areas. Anchor the
   * after-BG2 foam/mist at the END of those safe rows, not at authentic row
   * 224. The latter was technically submitted but faded out before reaching
   * the black gap. It uses BG2's camera/shape but not its winner pixels; later
   * BG1 and OBJ planes remain in front. */
  ActionEffectInstance mist = waterfall;
  mist.generation = 0x575D0000u ^ map_identity;
  mist.pulse_generation = 0x775D0000u ^ map_identity;
  mist.world_y = (int16_t)(
      bg2_camera_y + kActRaiserAuthenticHeight +
      kActionBgAitosWaterfallBottomExtensionPixels);
  mist.top_extent = 64;
  mist.bottom_extent = 152;
  mist.kind = kActionEffect_AitosWaterfallMist;
  mist.phase = kActionEffectPhase_AitosWaterfallMist;
  mist.render_layer = kActionEffectRenderLayer_Atmosphere;
  mist.geometry.data.rect =
      (ActionEffectLocalRect){-256.0f, -64.0f, 256.0f, 152.0f};
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
  CaptureWallTorches(dst, wram, wram_size, observer->scene_clock);
  CaptureAitosLavaPits(dst, wram, wram_size, observer->scene_clock);
  CaptureAitosSideLavaReservoirs(
      dst, wram, wram_size, observer->scene_clock);
  CaptureAitosWater(dst, wram, wram_size, observer->scene_clock);
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
    } else if (bloodpool_act2_map && IsEnemyFireball(&object)) {
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
    if (kind == kActionEffect_AitosStatueFire ||
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
