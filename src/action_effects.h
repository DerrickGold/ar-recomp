#ifndef ACTION_EFFECTS_H
#define ACTION_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

/* Presentation-only action-stage effects. The game-thread capture owns every
 * WRAM read and publishes this small value-copy through FrameSlot; present.c
 * must never infer spell identity from live game state. */
typedef enum ActionEffectKind {
  kActionEffect_None = 0,
  kActionEffect_MagicalFire,
} ActionEffectKind;

typedef enum ActionEffectPhase {
  kActionEffectPhase_None = 0,
  kActionEffectPhase_FireIgnition,
  kActionEffectPhase_FireBloom,
} ActionEffectPhase;

/* Action spell parts are emitted into an authentic OBJ priority band. The
 * enhanced pass is currently an intentional world-light overlay (above the
 * already-composited world, below HUD/HD UI), but retaining the source band
 * lets the diorama projection use the exact same authored plane shape and
 * leaves a clean path to priority-aware masking later. */
typedef enum ActionEffectRenderLayer {
  kActionEffectRenderLayer_WorldOverlay = 0,
} ActionEffectRenderLayer;

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
  /* Unsigned distances from the hot point, already selected for the current
   * flip by the animation handler (the ROM draws world-left/world+right). */
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
  uint8_t flags;
  uint8_t obj_priority;
  uint8_t render_layer;
  ActionEffectGeometry geometry;
} ActionEffectInstance;

typedef struct ActionEffectFrame {
  uint16_t game_frame;
  uint8_t controller_kind;
  uint8_t effect_count;
  uint8_t visible_count;
  ActionEffectInstance effects[kActionEffectMaxInstances];
} ActionEffectFrame;

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
  uint8_t kind;
  uint8_t phase;
  uint8_t active;
} ActionEffectObserverTrack;

typedef struct ActionEffectObserver {
  uint32_t next_generation;
  uint32_t next_pulse_generation;
  ActionEffectObserverTrack tracks[kActionEffectObserverTrackCount];
} ActionEffectObserver;

void ActionEffectObserver_Reset(ActionEffectObserver *observer);

/* elapsed_ticks is the producer's clamped emulation-tick delta. Passing zero
 * makes a paused/re-present capture idempotent; particle clocks cannot advance
 * merely because the host redraws a frozen game frame. */
void ActionEffects_CaptureFrame(ActionEffectObserver *observer,
                                ActionEffectFrame *dst,
                                const uint8_t *wram, size_t wram_size,
                                unsigned elapsed_ticks);

#endif  /* ACTION_EFFECTS_H */
