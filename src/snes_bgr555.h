#ifndef SNES_BGR555_H
#define SNES_BGR555_H

#include <stdint.h>

/* Expand one 5-bit SNES BGR555 colour component to 8 bits and apply the PPU
 * INIDISP brightness level (0..15) the same way the authentic scanline
 * renderer does. Shared, not duplicated, so a canvas/backdrop pixel and the
 * authentic pixel of the same colour agree exactly: the sim town canvas
 * (sim_town_canvas.c), the sim3D capture, and the ActRaiser_BackdropArgb
 * margin-gap fill (sim3d.c, shared with actraiser_rtl.c) all expand colours
 * this one way. A second copy would be free to drift. static inline keeps it
 * header-only with no translation unit of its own. */
static inline uint8_t ExpandColor5(uint32_t value, int brightness) {
  uint32_t expanded = (value << 3) | (value >> 2);
  return (uint8_t)(expanded * (uint32_t)brightness / 15);
}

#endif  /* SNES_BGR555_H */
