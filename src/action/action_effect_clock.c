#include "action_effect_clock.h"

#include <string.h>

#include "present/frame_timing.h"

/* Written and read on the game thread for the lifetime of the running game.
 * This counts completed object/OAM passes; inspection snapshots do not
 * restore execution or rewind this serial. */
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

  /* Unsigned subtraction handles the serial's natural wrap. Bound catch-up
   * work when presentation skips multiple completed gameplay passes. */
  uint32_t elapsed = serial - clock->last_serial;
  clock->last_serial = serial;
  if (elapsed > kFrameTimingMaximumElapsedTicks)
    elapsed = kFrameTimingMaximumElapsedTicks;
  return (unsigned)elapsed;
}
