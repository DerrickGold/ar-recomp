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
 *  - a pure host equivalent of the game's bounded overlay routine composes the
 *    developed 128x128 tilemap from explicit ROM tables and simulation inputs,
 *    without presenting the world-map screen or touching its shared
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
  /* Shared stock-SIM geography. The town playfield is 32x32 cells = 512x512
   * authentic pixels, which the BG1 scroll clamps independently corroborate
   * ($22 in [0,$100] over a 256px window, $24 in [0,$11F] over 224). Terrain,
   * voxel classification, and world-map composition all consume this same
   * six-town coordinate space. */
  kSimTownCount = 6,
  kSimTownCells = 32,
  kSimTownCellPixels = 16,
};

/* The water-animation source descriptor is one of $B000/$B040/$B080/$B0C0
 * (see docs/rom-map.md). sim_world_map_build.c CONSTRUCTS the value from these
 * and sim_world_map.c VALIDATES against them, so the producer and the validator
 * must share one definition rather than each holding a copy free to drift. */
enum {
  kWorldWaterSourceFirst = 0xB000,
  kWorldWaterSourceStride = 0x40,
  kWorldWaterFrameCount = 4,
};

/* Loads the ROM blobs. Safe to call with a short/absent ROM: the module then
 * reports unavailable and every consumer degrades to drawing nothing. */
bool SimWorldMap_Init(const uint8_t *rom_data, size_t rom_size);
void SimWorldMap_Shutdown(void);
bool SimWorldMap_Available(void);
/* True only after the owned HLE has published a complete composition. Unlike
 * Available, this never mistakes the pristine ROM baseline for current
 * development. */
bool SimWorldMap_DevelopedAvailable(void);

/* Top-left world-map tile of `town`'s 32x32 window. `town` is the raw map
 * number, 1-6 (Fillmore..Northwall); anything else returns false. */
bool SimWorldMap_OriginForTown(uint8_t town, int *tile_x, int *tile_y);

/* Publish a complete tilemap produced by the owned HLE builder. Marks only
 * changed tiles dirty and bumps the serial once if the image changed. Returns
 * the number of changed tilemap bytes. */
int SimWorldMap_PublishBuiltTilemap(const uint8_t *tilemap);

/* Synchronize the two animated water tiles with the source retained in the
 * authentic world-navigation DMA descriptor at $7E:00D7. Valid sources are
 * the four 64-byte frames $0A:B000/$B040/$B080/$B0C0. The frame bytes come
 * from immutable ROM; this never reads PPU VRAM or mutates emulated state.
 * Returns the number of visible tilemap cells dirtied, or zero for an invalid
 * or already-current source. */
int SimWorldMap_SetWaterAnimationSource(uint16_t source);

/* Current validated animation identity, not a clock approximation. Unknown
 * initial ROM art is not frame zero. Failure leaves the output unchanged. */
bool SimWorldMap_WaterAnimationFrame(uint8_t *frame);

/* Marks cells whose developed OR baseline pixels can change with a wave
 * phase, including the one-cell halo needed by Scale2x. Does not consume the
 * shared bake's dirty state. Caller supplies kSimWorldMapBytes bytes. */
bool SimWorldMap_WaterAnimationCells(uint8_t *cells);

/* Changes whenever the baked image would differ. Zero means "nothing usable
 * yet"; consumers compare against their own last-baked value. */
uint32_t SimWorldMap_Serial(void);

/* Changes only with geography, not the eight-tick water animation. Relief
 * meshes must not be reclassified (or visibly breathe) on each wave frame. */
uint32_t SimWorldMap_GeographySerial(void);

/* Fraction of the cell painted with the authored mountain-rock palette
 * ($40-$45). Desert sand ($2C-$2F), snow, forests and buildings are separate
 * materials. Zero for unavailable/out-of-range cells. */
float SimWorldMap_MountainCoverage(int tile_x, int tile_y);
/* True only when every texel in the developed cell belongs to the authored
 * ocean/wave palette ($10/$11). Mixed shores and unknown cells return false.
 * Animated tiles must qualify in every wave phase, so this is geography,
 * not a per-frame RGB classification. */
bool SimWorldMap_CellIsOpenWater(int tile_x, int tile_y);
/* Per-texel version of the same semantic test (8x8 bytes, 0 or 1). A pixel
 * must remain water in every animation phase. Includes mixed coastal cells;
 * never classifies by RGB. Invalid/unavailable inputs leave output unchanged. */
bool SimWorldMap_OpenWaterMask(int tile_x, int tile_y, uint8_t mask[64]);
/* Copy one owned 8x8 world-art tile and optional palette identities. Index
 * zero remains opaque here, just as in Mode 7. Never borrows mutable storage;
 * unavailable/invalid destinations are left unchanged. */
bool SimWorldMap_CopyTileArt(uint8_t tile, uint32_t pixels[64], uint8_t indices[64]);
/* Copy categorical rock samples from one developed 8x8 cell. Zero is any
 * non-rock material; 1..6 order the authored rock colours darkest to lightest.
 * This is palette identity, not an RGB classifier or a mutable atlas view. */
bool SimWorldMap_MountainShades(int tile_x, int tile_y, uint8_t shades[64]);

/* The retained pristine ROM tilemap (kSimWorldMapBytes), or NULL if the
 * module is unavailable. */
const uint8_t *SimWorldMap_Baseline(void);

/* Bakes the mirror into `pixels` as ARGB8888, kSimWorldMapPixels square,
 * fully opaque throughout.
 *
 * Deliberately no punch-out for the town being played. An earlier version
 * blanked the town's own 32x32-tile window to guarantee nothing was drawn
 * twice, which left a large black hole: the window is the town's whole
 * 512x512-pixel territory, but the town's ground quad draws at most the
 * 496x224 live view
 * of it the camera can see, so the remainder had nothing to fill it. Overlap
 * is instead handled by draw order — the town's ground quad is opaque and
 * drawn on top — and beyond it the world map's own half-resolution depiction
 * of the town is the correct stand-in for territory that is off-screen. */
bool SimWorldMap_Bake(uint32_t *pixels, int pitch_pixels);

/* Expands the pristine ROM tilemap through the current palette and animated
 * water tiles. World navigation uses this as the material below a feathered
 * town boundary; it is deliberately separate from the persistent developed
 * bake so town presentation keeps its exact authored pixels. */
bool SimWorldMap_BakeBaseline(uint32_t *pixels, int pitch_pixels);

/* Returns the persistent, tightly packed ARGB8888 bake after applying every
 * pending dirty tile, or NULL when the world map is unavailable. The pointer
 * remains owned by this module and is valid until SimWorldMap_Shutdown.
 *
 * Render backends that accept an ordinary upload pointer should prefer this
 * over allocating or mapping a native streaming texture merely to give Bake a
 * destination. Callers must treat the returned pixels as immutable. */
const uint32_t *SimWorldMap_BakedPixels(void);

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
