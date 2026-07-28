#ifndef SIM_WORLD_MAP_H
#define SIM_WORLD_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 *  - the game's bounded world-map overlay routine can be run transactionally
 *    against the current simulation state, producing the developed 128x128
 *    tilemap without presenting the world-map screen or trusting its shared
 *    `$7E:C000` scratch buffer.
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

/* Loads the ROM blobs. Safe to call with a short/absent ROM: the module then
 * reports unavailable and every consumer degrades to drawing nothing. */
bool SimWorldMap_Init(const uint8_t *rom_data, size_t rom_size);
void SimWorldMap_Shutdown(void);
bool SimWorldMap_Available(void);

/* Top-left world-map tile of `town`'s 32x32 window. `town` is the raw map
 * number, 1-6 (Fillmore..Northwall); anything else returns false. */
bool SimWorldMap_OriginForTown(uint8_t town, int *tile_x, int *tile_y);

/* Publish a complete tilemap produced by the owned ROM builder. Marks only
 * changed tiles dirty and bumps the serial once if the image changed. Returns
 * the number of changed tilemap bytes. */
int SimWorldMap_PublishBuiltTilemap(const uint8_t *tilemap);

/* Changes whenever the baked image would differ. Zero means "nothing usable
 * yet"; consumers compare against their own last-baked value. */
uint32_t SimWorldMap_Serial(void);

/* The retained pristine ROM tilemap (kSimWorldMapBytes), or NULL if the
 * module is unavailable. */
const uint8_t *SimWorldMap_Baseline(void);

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

/* Box-downsamples the baked image by `divisor` into `pixels`, which must be
 * (kSimWorldMapPixels / divisor) square. `divisor` must be >= 1 and divide
 * kSimWorldMapPixels exactly; anything else returns false.
 *
 * This exists because the source it reads MUST be the module's own persistent
 * CPU image, never a mapped streaming-texture lock. SDL_LockTexture is
 * documented write-only ("the pixels made available for editing don't
 * necessarily contain the old texture data", SDL_render.h) — on Metal the
 * mapping happens to be readable, so reading a just-written lock appears to
 * work, while a Vulkan/Mesa backend can hand back write-combined or staging
 * memory whose reads return unpredictable content. Downsampling from the lock
 * therefore produced a correct mip on macOS and a garbled one on Steam Deck.
 * Keeping the read on this side of the wall makes that mistake unavailable to
 * callers. Same hazard as finding O2 (the town canvas), in the read direction.
 *
 * Costs nothing extra: SimWorldMap_Bake already maintains the full-resolution
 * image this reads, which is what the caller's lock was a copy OF. */
bool SimWorldMap_Downsample(uint32_t *pixels, int pitch_pixels, int divisor);

#endif  /* SIM_WORLD_MAP_H */
