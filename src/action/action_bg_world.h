#ifndef ACTION_BG_WORLD_H
#define ACTION_BG_WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"

/* Pure action-stage background decoder (SPEC-bg-hle BH2).
 *
 * The native loader has already expanded the level into WRAM as 256-byte
 * pages of 16x16 metatile IDs. This module snapshots those pages plus the
 * 8-byte metatile definitions and publishes a complete, bounded array of SNES
 * 8x8 tilemap words. It never reads globals or mutates emulated state. */

enum {
  /* ActRaiser's WRAM mirror is exactly banks $7E-$7F. Keeping the limit in the
   * public input contract prevents a corrupt dimension from turning a caller-
   * supplied size into an unbounded host allocation. */
  kActionBgMaxWramBytes = kActRaiserWramSize,
  kActionBgMetatilePixels = 16,
};

typedef struct ActionBgDecodeInput {
  const uint8_t *wram;
  size_t wram_size;
  /* Pixel dimensions from layer state $2E/$30. The native map is page-based,
   * so both dimensions must be positive multiples of 256. */
  uint16_t world_width;
  uint16_t world_height;
  /* State $46: its high byte selects the first 256-byte WRAM map page. */
  uint16_t map_page;
  /* State $52/$54/$6B: definition-table pointer, mask, and common attribute
   * byte OR'd into every decoded tile word. */
  uint16_t metatile_table;
  uint16_t word_mask;
  uint8_t attributes;
} ActionBgDecodeInput;

/* Validated, read-only view of the native 256-byte BG map pages. This is the
 * one page-addressing contract shared by the full-world decoder and semantic
 * consumers such as presentation-side map-object detection. */
typedef struct ActionBgMapView {
  const uint8_t *map;
  size_t map_size;
  unsigned world_width;
  unsigned world_height;
  unsigned pages_wide;
} ActionBgMapView;

bool ActionBgMapView_Init(ActionBgMapView *view,
                          const uint8_t *wram, size_t wram_size,
                          uint16_t world_width, uint16_t world_height,
                          uint16_t map_page);

/* Looks up the 16x16 metatile containing one world-pixel coordinate. False
 * covers malformed/uninitialized views and coordinates outside the finite
 * world; `metatile` is not modified on failure. */
bool ActionBgMapView_LookupMetatile(const ActionBgMapView *view,
                                    int world_x, int world_y,
                                    uint8_t *metatile);

typedef enum ActionBgLookupResult {
  /* No complete publication is available. The caller must disable the
   * provider for the whole layer rather than mixing in a native tile. */
  kActionBgLookup_ProviderFailure = 0,
  /* A valid finite world was queried outside its bounds. This is a transparent
   * result, not a provider failure. */
  kActionBgLookup_OutsideWorld,
  kActionBgLookup_Tile,
} ActionBgLookupResult;

typedef struct ActionBgWorld ActionBgWorld;

ActionBgWorld *ActionBgWorld_Create(void);
void ActionBgWorld_Destroy(ActionBgWorld *world);

/* Drops the current publication and serial while retaining host allocations
 * for reuse. Used on reset, map transition, ROM unload, and savestate load. */
void ActionBgWorld_Reset(ActionBgWorld *world);

/* Validates and snapshots every byte that can affect decoding, builds into
 * scratch storage, then publishes only after complete success. A malformed
 * input fails closed and makes lookups unavailable for that frame; the last
 * complete allocation is retained only so a later valid update can reuse it. */
bool ActionBgWorld_Update(ActionBgWorld *world,
                          const ActionBgDecodeInput *input);

ActionBgLookupResult ActionBgWorld_Lookup(const ActionBgWorld *world,
                                           int tile_x, int tile_y,
                                           uint16_t *entry);

/* Returns the 16x16 metatile ID containing an expanded 8x8 tile coordinate.
 * This uses the same snapshotted map publication as ActionBgWorld_Lookup, so
 * presentation classification cannot race live WRAM or disagree with the
 * tile word being rendered. */
bool ActionBgWorld_LookupMetatile(const ActionBgWorld *world,
                                  int tile_x, int tile_y,
                                  uint8_t *metatile);

bool ActionBgWorld_IsValid(const ActionBgWorld *world);
uint32_t ActionBgWorld_Serial(const ActionBgWorld *world);
unsigned ActionBgWorld_TileWidth(const ActionBgWorld *world);
unsigned ActionBgWorld_TileHeight(const ActionBgWorld *world);
size_t ActionBgWorld_TileCount(const ActionBgWorld *world);

#endif  /* ACTION_BG_WORLD_H */
