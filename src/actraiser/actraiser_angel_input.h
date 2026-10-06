#ifndef AR_ACTRAISER_ANGEL_INPUT_H
#define AR_ACTRAISER_ANGEL_INPUT_H
/* Pure stick normalization and transport. No CPU, SDL or mutable game state.
 * Components use kAngelStickScale units; zero denotes digital input, while
 * an enabled sample with zero components denotes analog stick rest. */

#include <stdint.h>

enum {
  kAngelStickScale = 16383,
  kAngelDeadzoneDefaultPercent = 12,
  kAngelDeadzoneMaximumPercent = 60,
};
#define AR_ANGEL_STICK_ENABLED UINT32_C(0x80000000)

/* Radial deadzone, rescaled magnitude and circular speed cap. */
uint32_t ActRaiserAngel_EncodeStick(int x, int y, int deadzone_percent);
int ActRaiserAngel_StickX(uint32_t stick);
int ActRaiserAngel_StickY(uint32_t stick);

#endif
