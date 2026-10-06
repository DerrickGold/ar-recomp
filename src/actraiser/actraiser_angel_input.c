#include "actraiser_angel_input.h"

#include <math.h>

/* Signed 15-bit X/Y components and one enable bit fit alongside the buttons
 * in the producer's atomic input sample. Keep encoding details in this owner. */
static int Component(uint32_t stick, unsigned shift) {
  const unsigned value = (stick >> shift) & 0x7fffu;
  return value & 0x4000u ? (int)value - 0x8000 : (int)value;
}

int ActRaiserAngel_StickX(uint32_t stick) { return Component(stick, 0); }
int ActRaiserAngel_StickY(uint32_t stick) { return Component(stick, 15); }

uint32_t ActRaiserAngel_EncodeStick(int x, int y, int deadzone_percent) {
  if (x < -32767) x = -32767;
  if (x > 32767) x = 32767;
  if (y < -32767) y = -32767;
  if (y > 32767) y = 32767;
  if (deadzone_percent < 0) deadzone_percent = 0;
  if (deadzone_percent > kAngelDeadzoneMaximumPercent)
    deadzone_percent = kAngelDeadzoneMaximumPercent;
  const double length = sqrt((double)x * x + (double)y * y);
  const double deadzone = 32767.0 * deadzone_percent / 100.0;
  if (length <= deadzone) return AR_ANGEL_STICK_ENABLED;
  const double magnitude = fmin(length, 32767.0);
  const double gain = (magnitude - deadzone) / (32767.0 - deadzone) / length;
  const int dx = (int)lround(x * gain * kAngelStickScale);
  const int dy = (int)lround(y * gain * kAngelStickScale);
  return AR_ANGEL_STICK_ENABLED | ((uint32_t)dx & 0x7fffu) |
      (((uint32_t)dy & 0x7fffu) << 15);
}
