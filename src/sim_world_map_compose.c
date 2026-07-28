#include "sim_world_map_compose.h"

#include <string.h>

enum {
  /* LoROM $02:8000, $02:8100, and $02:87A5. */
  kOrdinaryTranslationRomOffset = 0x010000,
  kSpecialTranslationRomOffset = 0x010100,
  kTownDestinationRomOffset = 0x0107A5,

  /* $02:8672 clears this 8x8 block when $7F:9101 bit 0 is zero. */
  kFlagClearOffset = 0x0660,
  kFlagClearExtent = 8,

  kTownExtent = 32,
  kTownPageExtent = 16,
  kWorldStride = kSimWorldMapTiles,
  kSpecialFirstCell = 0xE3,
};

static uint16_t ReadLe16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool SimWorldMap_LoadRomTables(SimWorldMapRomTables *out,
                               const uint8_t *rom_data, size_t rom_size) {
  const size_t needed =
      kTownDestinationRomOffset + kSimWorldMapTownCount * 2;
  if (!out || !rom_data || rom_size < needed) return false;

  SimWorldMapRomTables decoded;
  memcpy(decoded.ordinary, rom_data + kOrdinaryTranslationRomOffset,
         sizeof(decoded.ordinary));
  memcpy(decoded.special, rom_data + kSpecialTranslationRomOffset,
         sizeof(decoded.special));
  for (int town = 0; town < kSimWorldMapTownCount; town++)
    decoded.town_destination[town] =
        ReadLe16(rom_data + kTownDestinationRomOffset + town * 2);
  *out = decoded;
  return true;
}

/* Convert a logical x/y within a town to the game's four-page storage:
 * TL, TR, BL, BR, with each page a row-major 16x16 byte array. */
static size_t TownCellIndex(int x, int y) {
  const int page_x = x / kTownPageExtent;
  const int page_y = y / kTownPageExtent;
  const int page = page_y * 2 + page_x;
  return (size_t)page * kTownPageExtent * kTownPageExtent +
      (size_t)(y % kTownPageExtent) * kTownPageExtent +
      (size_t)(x % kTownPageExtent);
}

static bool TownDestinationIsValid(uint16_t destination) {
  const size_t last = (size_t)destination +
      (kTownExtent - 1) * kWorldStride + (kTownExtent - 1);
  return last < kSimWorldMapBytes;
}

bool SimWorldMap_ComposeDeveloped(
    uint8_t out[kSimWorldMapBytes],
    const uint8_t baseline[kSimWorldMapBytes],
    const uint8_t town_maps[kSimWorldMapTownCount][kSimWorldMapTownCells],
    const uint16_t town_enabled[kSimWorldMapTownCount],
    uint8_t world_flags,
    const SimWorldMapRomTables *tables) {
  if (!out || !baseline || !town_maps || !town_enabled || !tables)
    return false;

  /* Reject malformed tables before touching the output. */
  for (int town = 0; town < kSimWorldMapTownCount; town++)
    if (town_enabled[town] &&
        !TownDestinationIsValid(tables->town_destination[town]))
      return false;

  memcpy(out, baseline, kSimWorldMapBytes);

  if (!(world_flags & 1)) {
    for (int row = 0; row < kFlagClearExtent; row++)
      memset(out + kFlagClearOffset + row * kWorldStride, 0,
             kFlagClearExtent);
  }

  for (int town = 0; town < kSimWorldMapTownCount; town++) {
    if (!town_enabled[town]) continue;
    const uint8_t *cells = town_maps[town];
    const size_t destination = tables->town_destination[town];

    /* $02:86D1: translate every cell, preserving the baseline wherever the
     * ordinary translation table returns zero. */
    for (int y = 0; y < kTownExtent; y++) {
      for (int x = 0; x < kTownExtent; x++) {
        const uint8_t cell = cells[TownCellIndex(x, y)];
        const uint8_t tile = tables->ordinary[cell];
        if (tile)
          out[destination + (size_t)y * kWorldStride + x] = tile;
      }
    }

    /* $02:8726: special cells $E3-$EF occur at the top-left of an aligned
     * 2x2 cell and replace the ordinary pass with four explicit world tiles. */
    for (int y = 0; y < kTownExtent; y += 2) {
      for (int x = 0; x < kTownExtent; x += 2) {
        const uint8_t cell = cells[TownCellIndex(x, y)];
        if (cell < kSpecialFirstCell ||
            cell >= kSpecialFirstCell + kSimWorldMapSpecialTranslationCount)
          continue;
        const uint8_t *replacement =
            tables->special[cell - kSpecialFirstCell];
        const size_t at = destination + (size_t)y * kWorldStride + x;
        out[at] = replacement[0];
        out[at + 1] = replacement[1];
        out[at + kWorldStride] = replacement[2];
        out[at + kWorldStride + 1] = replacement[3];
      }
    }
  }
  return true;
}
