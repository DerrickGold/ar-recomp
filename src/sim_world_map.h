#ifndef SIM_WORLD_MAP_H
#define SIM_WORLD_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* The Mode-7 world map, reused as an out-of-bounds ground extension under the
 * simulation towns.
 *
 * Three facts make this possible, all established from ROM/WRAM evidence
 * (docs/rendering-engine.md §13c records the derivation):
 *
 *  - the world map is three flat uncompressed ROM blobs — a 128x128 byte
 *    tilemap, 256 8bpp tiles, and a 256-entry palette;
 *  - one world-map tile (8 authentic pixels) covers exactly one town map cell
 *    (16 authentic pixels), so the world map is the town at half linear
 *    resolution and each town is a 32x32-tile window of it;
 *  - `$7E:C000` holds a live 128x128 shadow of that tilemap which stays
 *    coherent while a town is active, so the underlay tracks world state
 *    (built cathedrals, roads, development tiers) rather than freezing at the
 *    ROM baseline.
 *
 * Everything here is read-only with respect to the emulated machine. */

enum {
  kSimWorldMapTiles = 128,
  kSimWorldMapTilePixels = 8,
  kSimWorldMapPixels = kSimWorldMapTiles * kSimWorldMapTilePixels,  /* 1024 */
  kSimWorldMapBytes = kSimWorldMapTiles * kSimWorldMapTiles,
  /* A town cell is 16 authentic pixels and a world tile is 8. */
  kSimWorldMapTownScale = 2,
  /* The town playfield is 32x32 cells = 512x512 authentic pixels, which the
   * BG1 scroll clamps independently corroborate ($22 in [0,$100] over a 256px
   * window, $24 in [0,$11F] over 224). */
  kSimTownCells = 32,
  kSimTownCellPixels = 16,
};

/* Rows 0-7 of the live shadow hold unrelated scratch while a town is active,
 * so they are only adopted from a world-map frame and otherwise keep the ROM
 * baseline. Only Northwall's window (origin y = 0) reaches them at all. */
enum { kSimWorldMapVolatileRows = 8 };

/* Loads the ROM blobs. Safe to call with a short/absent ROM: the module then
 * reports unavailable and every consumer degrades to drawing nothing. */
bool SimWorldMap_Init(const uint8_t *rom_data, size_t rom_size);
void SimWorldMap_Shutdown(void);
bool SimWorldMap_Available(void);

/* Top-left world-map tile of `town`'s 32x32 window. `town` is the raw map
 * number, 1-6 (Fillmore..Northwall); anything else returns false. */
bool SimWorldMap_OriginForTown(uint8_t town, int *tile_x, int *tile_y);

/* Game thread, once a frame. Adopts the live shadow when the current map is
 * one whose code owns that buffer, and bumps the serial when the map changes.
 * Outside those maps the mirror is simply left alone. */
void SimWorldMap_Refresh(const uint8 *wram, uint8_t map_group,
                         uint8_t map_number);

/* Changes whenever the baked image would differ. Zero means "nothing usable
 * yet"; consumers compare against their own last-baked value. */
uint32_t SimWorldMap_Serial(void);

/* Bakes the mirror into `pixels` as ARGB8888, kSimWorldMapPixels square,
 * fully opaque throughout.
 *
 * Deliberately no punch-out for the town being played. An earlier version
 * blanked the town's own 32x32-tile window to guarantee nothing was drawn
 * twice, which left a large black hole: the window is the town's whole
 * 512x512-pixel territory, but the town's ground quad only draws the ~446x224
 * of it the camera can see, so the remainder had nothing to fill it. Overlap
 * is instead handled by draw order — the town's ground quad is opaque and
 * drawn on top — and beyond it the world map's own half-resolution depiction
 * of the town is the correct stand-in for territory that is off-screen. */
bool SimWorldMap_Bake(uint32_t *pixels, int pitch_pixels);

#endif  /* SIM_WORLD_MAP_H */
