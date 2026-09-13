#ifndef AR_SIM_PPU_COLOR_MATH_H
#define AR_SIM_PPU_COLOR_MATH_H

#include <stdbool.h>
#include <stdint.h>

/* Callers separately validate main/subscreen ownership. With no colour
 * window/source controls and no designated layers, fixed colour is inert.
 * Do not confuse a leftover register value with an active visual effect. */
static inline bool SimPpuColorMath_IsNoOp(
    uint8_t control, uint8_t designation, uint16_t fixed_color) {
  return control == 0 &&
      ((designation & 0x3f) == 0 ||
       (fixed_color == 0 && (designation & 0xc0) == 0));
}

#endif
