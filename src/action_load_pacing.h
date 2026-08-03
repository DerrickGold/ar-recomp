#ifndef ACTION_LOAD_PACING_H
#define ACTION_LOAD_PACING_H

#include <stdint.h>

/* $00:8433 is the screen-off helper used before large scene loads; its
 * INIDISP write is the first point at which repeating the display is safe.
 *
 * In the matching Fillmore Act 1 replay, snes9x writes the action-mode state
 * and game clock at host frame 1193, then does not advance the game clock
 * again until frame 1517: 323 physical frames pass while NMI is disabled.
 * The recomp already spends eight host frames in the authentic APU ack wait,
 * so the collapsed CPU-side map/graphics work accounts for the other 315.
 * The top-level transition and Advent cue are shared by action destination
 * groups 1-7, so this is intentionally one presentation/audio pacing value,
 * not per-level gameplay timing. Fillmore supplies the frame-exact oracle and
 * therefore the maximum hold. A matching enhanced one-shot may shorten it
 * after naturally completing; NMI and the game clock remain stopped for every
 * frame that is actually inserted.
 */
enum {
  kActionLoadPacingForceBlankBlock = 0x00843E,
  kActionLoadPacingOracleStallFrames = 323,
  kActionLoadPacingExistingWaitFrames = 8,
  kActionLoadPacingInsertedFrames =
      kActionLoadPacingOracleStallFrames -
      kActionLoadPacingExistingWaitFrames,
};

static inline unsigned ActionLoadPacing_Frames(
    uint8_t map_group, uint8_t destination_map_group,
    uint32_t block_pc, uint8_t inidisp) {
  const int is_world_to_action =
      map_group == 0x00 &&
      destination_map_group >= 0x01 && destination_map_group <= 0x07;
  const int is_force_blank = (inidisp & 0x80) != 0;
  return is_world_to_action &&
                 block_pc == kActionLoadPacingForceBlankBlock &&
                 is_force_blank
             ? kActionLoadPacingInsertedFrames
             : 0;
}

typedef enum ActionLoadPacingTriggerDecision {
  /* Not the armed loader's halt write (or currently in an interrupt): leave
   * the arm intact and let the write proceed normally. */
  kActionLoadPacingTrigger_Ignore,
  /* It is the halt write, but the display/mode has left the armed transition:
   * discard the stale arm and let the write proceed. */
  kActionLoadPacingTrigger_Discard,
  /* The exact halt arrived while the action transition is still blank. */
  kActionLoadPacingTrigger_Hold,
} ActionLoadPacingTriggerDecision;

static inline ActionLoadPacingTriggerDecision ActionLoadPacing_EvaluateTrigger(
    unsigned armed_frames, uint8_t map_group, uint8_t inidisp,
    uint8_t apu_port, uint8_t apu_value, int in_interrupt) {
  if (!armed_frames || apu_port != 0 || apu_value != 0xF0 || in_interrupt)
    return kActionLoadPacingTrigger_Ignore;
  if (map_group < 0x01 || map_group > 0x07 || !(inidisp & 0x80))
    return kActionLoadPacingTrigger_Discard;
  return kActionLoadPacingTrigger_Hold;
}

/* The oracle-derived hold remains the upper bound. It may end early only when
 * the exact enhanced one-shot present at the trigger reaches its natural end.
 * Requiring two matching nonzero generation tokens prevents a stopped,
 * failed, looping, authentic, or newly changed track from releasing the hold. */
static inline int ActionLoadPacing_ShouldReleaseForOneShot(
    unsigned remaining_frames, uint64_t latched_token,
    uint64_t current_token, int current_completed) {
  return remaining_frames != 0 && latched_token != 0 &&
         latched_token == current_token && current_completed;
}

#endif  // ACTION_LOAD_PACING_H
