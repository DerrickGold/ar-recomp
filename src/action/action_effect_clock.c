#include "action_effect_clock.h"

#include <string.h>

#include "frame_timing.h"

/* Written and read on the game thread. It is deliberately outside savestates:
 * FrameSlot_ResetActionEffects invalidates the consumer after a load, and the
 * next capture seeds from whichever completed pass serial is current. */
static uint32_t s_completed_pass_serial;

void ActionEffectGameplayClock_CompletePass(void) {
  s_completed_pass_serial++;
}

uint32_t ActionEffectGameplayClock_Serial(void) {
  return s_completed_pass_serial;
}

void ActionEffectTickClock_Reset(ActionEffectTickClock *clock) {
  if (clock) memset(clock, 0, sizeof(*clock));
}

unsigned ActionEffectTickClock_Capture(ActionEffectTickClock *clock) {
  if (!clock) return 0;
  const uint32_t serial = ActionEffectGameplayClock_Serial();
  if (!clock->valid) {
    clock->last_serial = serial;
    clock->valid = 1;
    return 0;
  }

  /* Unsigned subtraction deliberately handles the serial's natural wrap. A
   * savestate/restart reset invalidates this observer before sampling again,
   * so a discontinuity cannot become a false catch-up burst. */
  uint32_t elapsed = serial - clock->last_serial;
  clock->last_serial = serial;
  if (elapsed > kFrameTimingMaximumElapsedTicks)
    elapsed = kFrameTimingMaximumElapsedTicks;
  return (unsigned)elapsed;
}
