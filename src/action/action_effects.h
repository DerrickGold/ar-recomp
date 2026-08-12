#ifndef ACTION_EFFECTS_H
#define ACTION_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

/* Presentation-only action-stage effects. The game-thread capture owns every
 * WRAM read and publishes this small value-copy through FrameSlot; present.c
 * must never infer spell identity from live game state. */
/* Values match the ROM's spell IDs at controller +$38 ($00:9F13 dispatches
 * 1/2/3/4 to $9F25/$9F71/$9FBB/$9FFA), so the captured kind IS the game's own
 * identity rather than a presentation-side renumbering. */
typedef enum ActionEffectKind {
  kActionEffect_None = 0,
  kActionEffect_MagicalFire = 1,
  kActionEffect_MagicalStardust = 2,
  kActionEffect_MagicalAura = 3,
  kActionEffect_MagicalLight = 4,
  /* Presentation-only scene accents. These values deliberately follow the
   * ROM spell IDs so the established spell identities remain stable. */
  kActionEffect_WallTorch,
  kActionEffect_EnemyFireball,
  kActionEffect_LightningTrap,
  kActionEffect_BloodpoolBossLightning,
  kActionEffect_SwordBeam,
  kActionEffect_MarahnaFireball,
  kActionEffect_MarahnaLightningLink,
  kActionEffect_MarahnaBossLightning,
  kActionEffect_AitosLavaPit,
  kActionEffect_AitosLavaFireball,
  kActionEffect_AitosMoltenRock,
  kActionEffect_AitosWaterSplash,
  kActionEffect_AitosWaterfall,
  kActionEffect_AitosWaterfallMist,
  kActionEffect_KindCount,
} ActionEffectKind;

/* One entry per authored visual stage a style may want to distinguish. These
 * are presentation phases, not ROM animation states: several map from a state
 * number, others from a visual range where the state is shared (see
 * docs/effects-hook-investigation.md "Spell catalogue"). */
typedef enum ActionEffectPhase {
  kActionEffectPhase_None = 0,
  kActionEffectPhase_FireIgnition,
  kActionEffectPhase_FireBloom,
  /* Stardust relaunches four times per actor; each relaunch restarts the
   * particle clock through pulse_key.
   *
   * PreLaunch is created at the player with zero velocity, BEFORE the launch
   * handler
   * relocates it to the viewport edge. Shares the flying star's animation
   * state and visual, so motion is the only thing that separates them. It is
   * identified (so it is not censused as unknown) but deliberately carries no
   * style, and therefore draws nothing: decorating it put a comet on the
   * player's feet, which is what "stardust spawns in the ground" actually
   * was. */
  kActionEffectPhase_StardustPreLaunch,
  kActionEffectPhase_StardustLaunch,
  kActionEffectPhase_StardustBurst,
  kActionEffectPhase_AuraOrb,
  kActionEffectPhase_LightFlare,
  /* The column's pre-beam stage. Held deliberately dim: the investigation
   * calls out that the 24-tick pre-beam visual must not get full intensity. */
  kActionEffectPhase_LightBeamCharge,
  kActionEffectPhase_LightBeam,
  kActionEffectPhase_WallTorch,
  kActionEffectPhase_EnemyFireballFlight,
  kActionEffectPhase_LightningActive,
  kActionEffectPhase_BossLightningStrike,
  kActionEffectPhase_BossLightningImpact,
  kActionEffectPhase_SwordBeamFlight,
  kActionEffectPhase_MarahnaFireballOrb,
  kActionEffectPhase_MarahnaFireballSplit,
  kActionEffectPhase_MarahnaSnakeFireballShot,
  kActionEffectPhase_MarahnaLightningActive,
  kActionEffectPhase_MarahnaBossLightningCharge,
  kActionEffectPhase_MarahnaBossLightningOrb,
  kActionEffectPhase_MarahnaBossLightningBolt,
  kActionEffectPhase_MarahnaBossLightningGroundCharge,
  kActionEffectPhase_AitosLavaPit,
  kActionEffectPhase_AitosLavaFireballFlight,
  kActionEffectPhase_AitosMoltenRockFlight,
  kActionEffectPhase_AitosWaterSplash,
  kActionEffectPhase_AitosWaterfallFlow,
  kActionEffectPhase_AitosWaterfallMist,
  kActionEffectPhase_Count,
} ActionEffectPhase;

/* Which structural part of a spell an instance is. Most spells are N
 * equivalent clones (Body); Magical Light is the exception the investigation
 * flags — a stationary centre flare plus two mirrored 16x224 beam columns,
 * which want completely different styling from each other. */
typedef enum ActionEffectRole {
  kActionEffectRole_Body = 0,
  kActionEffectRole_Centre,
  kActionEffectRole_Column,
  kActionEffectRole_Count,
} ActionEffectRole;

/* Action spell parts are emitted into an authentic OBJ priority band. The
 * enhanced pass is currently an intentional world-light overlay (above the
 * already-composited world, below HUD/HD UI), but retaining the source band
 * lets the diorama projection use the exact same authored plane shape and
 * leaves a clean path to priority-aware masking later. */
typedef enum ActionEffectRenderLayer {
  kActionEffectRenderLayer_WorldOverlay = 0,
  /* Environmental accents that belong inside the captured BG2 plane. Flat
   * presentation clips these through the PPU's BG2-winner mask; Diorama
   * inserts them immediately after the resolved BG2 draw. */
  kActionEffectRenderLayer_Bg2Plane,
  /* Camera-local atmospheric cover for an intentionally unavailable Diorama
   * BG2 extension. Unlike the source veil, this is unmasked inside the
   * after-BG2 callback so later BG1/OBJ planes remain in front. */
  kActionEffectRenderLayer_Atmosphere,
  kActionEffectRenderLayer_Count,
} ActionEffectRenderLayer;

/* Diorama rooms can shape BG1 and each OBJ priority band independently. Keep
 * the source plane with the captured effect so presentation projects a torch
 * with its wall and an action object with its authentic OBJ band. Flat-mode
 * projection intentionally treats both identically. */
typedef enum ActionEffectProjectionPlane {
  kActionEffectProjectionPlane_Obj = 0,
  kActionEffectProjectionPlane_Bg1,
  kActionEffectProjectionPlane_Bg2,
} ActionEffectProjectionPlane;

typedef enum ActionEffectGeometryKind {
  kActionEffectGeometry_None = 0,
  kActionEffectGeometry_Point,
  kActionEffectGeometry_Rect,
  kActionEffectGeometry_Segment,
} ActionEffectGeometryKind;

typedef struct ActionEffectLocalPoint {
  float x, y;
} ActionEffectLocalPoint;

typedef struct ActionEffectLocalRect {
  float x0, y0, x1, y1;
} ActionEffectLocalRect;

typedef struct ActionEffectLocalSegment {
  float x0, y0, x1, y1;
} ActionEffectLocalSegment;

typedef struct ActionEffectGeometry {
  uint8_t kind;
  union {
    ActionEffectLocalPoint point;
    ActionEffectLocalRect rect;
    ActionEffectLocalSegment segment;
  } data;
} ActionEffectGeometry;

enum {
  /* Seven is the whole action-magic cohort ($06A0-$0820), not a Fire count.
   * Fire/Aura/Stardust use four slots; Light uses the final three. */
  kActionEffectMaxInstances = 7,
  kActionEffectObserverTrackCount = 7,
  kActionSceneEffectMaxInstances = 16,
  /* Map-derived accents have their own capture/render budget. The measured
   * Aitos waterfall window holds 14 platform splashes plus one BG2 veil and
   * one bottom-mist record; none may consume the 16-record actor budget. */
  kActionSceneDecorationMaxInstances = 16,
  kActionSceneEffectObserverTrackCount = 80,
  kActionEffectObjPriorityCount = 4,
  kActionEffectFlag_Visible = 1 << 0,
  kActionEffectFlag_FlipHorizontal = 1 << 1,
  kActionEffectFlag_FlipVertical = 1 << 2,
};

typedef struct ActionEffectInstance {
  uint32_t generation;
  uint32_t pulse_generation;
  uint16_t record_address;
  int16_t world_x, world_y;
  int16_t velocity_x, velocity_y;
  /* Raw composition collision bounds after flip selection. Most actors store
   * unsigned distances from the hot point, but the player sword beam carries
   * signed header offsets here; presentation must use geometry instead. */
  uint16_t left_extent, top_extent, right_extent, bottom_extent;
  uint16_t composition;
  uint16_t visual;
  uint16_t animation_state;
  uint16_t animation_index;
  uint16_t flip_attributes;
  uint16_t age_ticks;
  uint16_t phase_ticks;
  uint16_t pulse_ticks;
  uint8_t kind;
  uint8_t phase;
  uint8_t role;
  uint8_t flags;
  uint8_t obj_priority;
  uint8_t render_layer;
  uint8_t projection_plane;
  ActionEffectGeometry geometry;
} ActionEffectInstance;

/* Diagnostic payload: the raw identity of a cohort slot that was ACTIVE while
 * the controller was live but which no spell rule matched. Nothing renders
 * from this — it exists so a single play session can correct the rule table
 * against real WRAM instead of against the ROM analysis it was written from.
 * Fire's rules are measured; the other three are transcribed and unproven. */
typedef struct ActionEffectUnmatched {
  uint16_t record_address;
  uint16_t status;
  uint16_t animation_address;
  uint16_t animation_state;
  uint16_t visual;
  uint16_t composition;
  uint16_t flip_attributes;
  uint8_t animation_bank;
} ActionEffectUnmatched;

typedef struct ActionEffectFrame {
  uint16_t game_frame;
  uint8_t controller_kind;
  uint8_t effect_count;
  uint8_t visible_count;
  ActionEffectInstance effects[kActionEffectMaxInstances];
  uint8_t unmatched_count;
  ActionEffectUnmatched unmatched[kActionEffectMaxInstances];
} ActionEffectFrame;

/* Non-spell scene effects use the same renderer-independent instance contract,
 * but remain a separate frame. Dynamic actors and map-derived decorations have
 * independent bounded lists: a dense camera window must never evict actor
 * effects. Each list fails closed independently if its own capture would be
 * partial. */
typedef struct ActionSceneEffectFrame {
  uint16_t game_frame;
  uint8_t effect_count;
  uint8_t visible_count;
  uint8_t overflow;
  ActionEffectInstance effects[kActionSceneEffectMaxInstances];
  uint8_t decoration_count;
  uint8_t decoration_visible_count;
  uint8_t decoration_overflow;
  ActionEffectInstance decorations[kActionSceneDecorationMaxInstances];
} ActionSceneEffectFrame;

/* Observer state is explicit so savestate/restart boundaries can reset it and
 * tests do not depend on process-global history. pulse_key is capture-private:
 * future repeated-launch spells can advance pulse_generation without ending
 * the actor's outer generation. */
typedef struct ActionEffectObserverTrack {
  uint32_t generation;
  uint32_t pulse_generation;
  uint16_t age_ticks;
  uint16_t phase_ticks;
  uint16_t pulse_ticks;
  uint16_t pulse_key;
  /* Scene-only continuity sample. Spell tracks leave continuity_valid clear;
   * their controller/cohort contract supplies the outer lifetime. */
  uint32_t continuity_key;
  int16_t last_world_x;
  int16_t last_world_y;
  int16_t last_velocity_x;
  int16_t last_velocity_y;
  uint8_t kind;
  uint8_t phase;
  uint8_t active;
  uint8_t continuity_valid;
} ActionEffectObserverTrack;

typedef struct ActionEffectObserver {
  uint32_t next_generation;
  uint32_t next_pulse_generation;
  /* Map-backed emitters have no actor slot from which to derive a lifecycle
   * clock. Seed this from the authentic game frame on action entry, then
   * advance it only by gameplay ticks so native pause cannot animate torches
   * or lava while their source BG is frozen. */
  uint16_t scene_clock;
  uint8_t scene_clock_valid;
  ActionEffectObserverTrack tracks[kActionEffectObserverTrackCount];
  ActionEffectObserverTrack
      scene_tracks[kActionSceneEffectObserverTrackCount];
} ActionEffectObserver;

void ActionEffectObserver_Reset(ActionEffectObserver *observer);

/* elapsed_ticks is the producer's clamped action-gameplay-tick delta. Passing
 * zero makes both native pause and host re-present captures idempotent;
 * particle clocks cannot advance merely because an emulated frame ran. */
void ActionEffects_CaptureFrame(ActionEffectObserver *observer,
                                ActionEffectFrame *dst,
                                const uint8_t *wram, size_t wram_size,
                                unsigned elapsed_ticks);

/* Captures exact, measured scene identities used by the enhanced action pass:
 * Bloodpool/Marahna BG1 torch and Aitos lava-pit signatures, the measured
 * enemy projectile families, the vertical trap, linked Marahna lightning and
 * Marahna boss-lightning families, the Bloodpool map-$08 boss lightning
 * sequence, the global player sword beam, and the exact two-child Aitos boss
 * sword volley. Unknown objects are ignored;
 * presentation never guesses from pixels or palette colours. */
void ActionSceneEffects_CaptureFrame(ActionEffectObserver *observer,
                                     ActionSceneEffectFrame *dst,
                                     const uint8_t *wram, size_t wram_size,
                                     unsigned elapsed_ticks);

#endif  /* ACTION_EFFECTS_H */
