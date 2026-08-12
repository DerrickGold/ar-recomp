#ifndef ACTION_EFFECT_CLOCK_H
#define ACTION_EFFECT_CLOCK_H

#include <stdint.h>

/* The emulator continues producing vblanks while ActRaiser's native pause
 * screen is open, but the ROM skips its action object/OAM pass at $00:8C98.
 * This game-thread-only seam publishes only successfully completed passes and
 * converts their monotonic serial into the bounded delta consumed by action
 * lighting and particles. */
typedef struct ActionEffectTickClock {
  uint32_t last_serial;
  uint8_t valid;
} ActionEffectTickClock;

void ActionEffectGameplayClock_CompletePass(void);
uint32_t ActionEffectGameplayClock_Serial(void);

void ActionEffectTickClock_Reset(ActionEffectTickClock *clock);
unsigned ActionEffectTickClock_Capture(ActionEffectTickClock *clock);

#endif  /* ACTION_EFFECT_CLOCK_H */
