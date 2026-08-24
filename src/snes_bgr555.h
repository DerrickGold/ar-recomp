#ifndef SNES_BGR555_H
#define SNES_BGR555_H

#include <stdint.h>

/* Expand one SNES BGR555 component to 8 bits, then apply INIDISP brightness
 * (0..15) consistently across authentic and enhanced renderers. */
static inline uint8_t ExpandColor5(uint32_t value, int brightness) {
  value &= UINT32_C(0x1F);
  uint32_t expanded = (value << 3) | (value >> 2);
  return (uint8_t)(expanded * (uint32_t)brightness / 15);
}

#endif  /* SNES_BGR555_H */
