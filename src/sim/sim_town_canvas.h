#ifndef SIM_TOWN_CANVAS_H
#define SIM_TOWN_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/game/types.h"

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

typedef enum SimTownCanvasChange {
  kSimTownCanvasChange_None = 0,
  /* The composed 64x64 tile layout changed. This is the only canvas input
   * that can change enhanced object classification; character animation,
   * palette cycling and display fades are pixel-only publications. */
  kSimTownCanvasChange_Tilemap = 1 << 0,
  kSimTownCanvasChange_Characters = 1 << 1,
  kSimTownCanvasChange_Palette = 1 << 2,
  kSimTownCanvasChange_Display = 1 << 3,
} SimTownCanvasChange;

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
/* Independent source revisions let downstream presentation invalidate only
 * the work owned by that source. A revision advances even when the changed
 * source is currently unused and therefore leaves the image serial alone. */
uint32_t SimTownCanvas_TilemapSerial(void);
uint32_t SimTownCanvas_CharacterSerial(void);
uint32_t SimTownCanvas_PaletteSerial(void);
uint32_t SimTownCanvas_DisplaySerial(void);
uint32_t SimTownCanvas_LastChangeMask(void);

/* Takes one coalesced tile-row region rewritten since the previous drain, in
 * town pixels. Call until false; a quiet town returns false immediately. */
bool SimTownCanvas_TakeDirtyRect(int *x, int *y, int *width, int *height);
/* Row-major ARGB8888, kSimTownCanvasPixels square, fully opaque. */
const uint32_t *SimTownCanvas_Pixels(void);

/* Row-major 0/1 opacity from the same decoded BG1 source generation as
 * SimTownCanvas_Pixels(). The rendered canvas is intentionally opaque, so
 * enhanced consumers use this companion plane when they need the exact SNES
 * silhouette rather than a colour-distance approximation. Owned by the
 * canvas producer and valid until its next render or reset. */
const uint8_t *SimTownCanvas_SourceOpacity(void);

/* Renders one raw 16x16 SIM terrain metatile with the currently cached town
 * character data and palette. This deliberately bypasses the already
 * composed 32x32 cell map: enhanced terrain objects can therefore assemble a
 * clean source stamp even when every visible instance of one metatile is
 * fused with a neighbouring mountain. */
bool SimTownCanvas_RenderTerrainMetatile(const uint8 *wram,
                                         uint8_t metatile,
                                         uint32_t out_pixels[16 * 16]);

#endif  /* SIM_TOWN_CANVAS_H */
