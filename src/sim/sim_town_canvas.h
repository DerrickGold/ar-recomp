#ifndef SIM_TOWN_CANVAS_H
#define SIM_TOWN_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

/* The complete town ground, for the parts of it the camera cannot see.
 *
 * The PPU only rasterizes the visible window, but the game keeps the whole
 * town's BG1 tilemap resident in WRAM the entire time. `$03:9C43` writes each
 * cell's 2x2 tile block at
 *
 *   $7F:0000 + quadrant*2048 + (cellY & 15)*128 + (cellX & 15)*4
 *
 * with the four words at +$00/+$02/+$40/+$42, so the row stride is 32 tiles,
 * the quadrant stride is 32x32 tiles, and the four quadrant pages together
 * are a 64x64-tile — 512x512 pixel — map of the whole town. That quadrant
 * paging is why a row-major read of the range looks like unrelated data.
 *
 * So this renders rather than accumulates: tilemap from WRAM, character data
 * from VRAM $0000 (the sim town's BG1 char base), colours from CGRAM. The
 * result is always current — construction that happens off-screen shows up
 * on the frame the game commits it, with no camera visit required — and is
 * full resolution everywhere, not just where the player has been.
 *
 * An earlier version accumulated captured frames into the same buffer. It is
 * gone: it could only ever show ground the camera had already passed over,
 * and only as it looked at the time. */

enum {
  /* 64x64 tiles of 8 pixels. Corroborated independently by the town BG1
   * scroll clamps: $22 in [0,$100] over a 256-pixel window and $24 in
   * [0,$11F] over 224 both resolve to exactly 512. */
  kSimTownCanvasPixels = 512,
  kSimTownCanvasTiles = 64,
  kSimTownTilemapWram = 0x10000,   /* $7F:0000 in the flat 128-KiB mirror */
  kSimTownQuadrantWords = 1024,    /* 32x32 tiles per quadrant page */
};

void SimTownCanvas_Reset(void);

/* Game thread, once a frame. Re-renders only when the tilemap, the character
 * data or the palette has actually changed, so a still town costs three
 * memcmps. `brightness` is the PPU's INIDISP level, applied the same way the
 * captured planes apply it. Passing a different town resets first. */
void SimTownCanvas_Render(uint8_t town, const uint8 *wram,
                          const uint16_t *vram, const uint16_t *cgram,
                          int brightness, uint32_t backdrop_argb);

uint8_t SimTownCanvas_Town(void);
/* Changes whenever the rendered image does; zero means nothing to draw. */
uint32_t SimTownCanvas_Serial(void);

/* Region rewritten since the last take, in town pixels. False when nothing
 * changed, so a quiet town uploads nothing. */
bool SimTownCanvas_TakeDirtyRect(int *x, int *y, int *width, int *height);
/* Row-major ARGB8888, kSimTownCanvasPixels square, fully opaque. */
const uint32_t *SimTownCanvas_Pixels(void);

#endif  /* SIM_TOWN_CANVAS_H */
