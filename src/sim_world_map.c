#include "sim_world_map.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

/* ROM residency, all uncompressed and all verified byte-for-byte against a
 * live world-map capture (tilemap 16172/16384 and chr 16346/16384 identical,
 * the deltas being exactly the runtime edits; palette 512/512 identical). */
enum {
  kWorldTilemapRomOffset = 0x033341,  /* LoROM $06:B341, 128x128 bytes */
  kWorldTilesRomOffset = 0x070000,    /* LoROM $0E:8000, 256 x 64B 8bpp */
  kWorldPaletteRomOffset = 0x0E3F93,  /* LoROM $1C:BF93, 256 x BGR555 */
  kWorldTileCount = 256,
  kWorldTileBytes = 64,
};

/* Live shadow of the Mode-7 tilemap, row-major, one byte per tile. On the
 * world-map screen this matches the Mode-7 VRAM exactly. */
enum { kWorldShadowWram = 0xC000 };

/* Each town's window origin, derived as (world cathedral icon) minus (the
 * town's own cathedral cell). Every origin lands on a multiple of 16 and the
 * six windows tile the map disjointly, which is what pins the assignment:
 * no other pairing of towns to icons has that property. Bloodpool and
 * Fillmore share the x79/x80 edge, Aitos and Kasandora the y63/y64 edge, so
 * standing at one town's border genuinely shows the neighbour's live state. */
typedef struct SimWorldTownWindow {
  uint8_t tile_x, tile_y;
} SimWorldTownWindow;

static const SimWorldTownWindow kTownWindows[6] = {
  { 80, 48 },   /* 1 Fillmore  */
  { 48, 48 },   /* 2 Bloodpool */
  { 16, 64 },   /* 3 Kasandora */
  { 16, 32 },   /* 4 Aitos     */
  { 64, 96 },   /* 5 Marahna   */
  { 32,  0 },   /* 6 Northwall */
};

static struct {
  bool available;
  uint8_t tilemap[kSimWorldMapBytes];
  uint8_t tiles[kWorldTileCount * kWorldTileBytes];
  uint32_t palette[256];
  uint32_t serial;
} g_world;

static uint32_t ExpandBgr555(uint16_t value) {
  unsigned r = (value & 0x1F) << 3;
  unsigned g = ((value >> 5) & 0x1F) << 3;
  unsigned b = ((value >> 10) & 0x1F) << 3;
  r |= r >> 5;
  g |= g >> 5;
  b |= b >> 5;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool SimWorldMap_Init(const uint8_t *rom_data, size_t rom_size) {
  memset(&g_world, 0, sizeof(g_world));
  size_t needed = kWorldTilesRomOffset + kWorldTileCount * kWorldTileBytes;
  if (!rom_data || rom_size < needed) {
    fprintf(stderr,
            "[sim-worldmap] unavailable: ROM is %zu bytes, need %zu\n",
            rom_size, needed);
    return false;
  }
  memcpy(g_world.tilemap, rom_data + kWorldTilemapRomOffset,
         sizeof(g_world.tilemap));
  memcpy(g_world.tiles, rom_data + kWorldTilesRomOffset, sizeof(g_world.tiles));
  for (int i = 0; i < 256; i++) {
    const uint8_t *entry = rom_data + kWorldPaletteRomOffset + i * 2;
    g_world.palette[i] =
        ExpandBgr555((uint16_t)(entry[0] | ((uint16_t)entry[1] << 8)));
  }
  g_world.available = true;
  g_world.serial = 1;
  return true;
}

void SimWorldMap_Shutdown(void) { memset(&g_world, 0, sizeof(g_world)); }

bool SimWorldMap_Available(void) { return g_world.available; }

bool SimWorldMap_OriginForTown(uint8_t town, int *tile_x, int *tile_y) {
  if (town < 1 || town > 6) return false;
  if (tile_x) *tile_x = kTownWindows[town - 1].tile_x;
  if (tile_y) *tile_y = kTownWindows[town - 1].tile_y;
  return true;
}

/* The shadow is only known to be the world map on the maps whose code owns
 * it. Refusing every other map keeps an action stage's use of the same WRAM
 * from being adopted as terrain. */
static bool ShadowIsTrustworthy(uint8_t map_group, uint8_t map_number) {
  if (map_group != 0) return false;
  return ActRaiser_IsSimulationTown(map_group, map_number) ||
      map_number == kActRaiserNonActionMap_WorldMap;
}

void SimWorldMap_Refresh(const uint8 *wram, uint8_t map_group,
                         uint8_t map_number) {
  if (!g_world.available || !wram) return;
  if (!ShadowIsTrustworthy(map_group, map_number)) return;

  const uint8_t *shadow = wram + kWorldShadowWram;
  /* Rows 0-7 are scratch unless the world map itself is on screen. */
  int first_row = (map_number == kActRaiserNonActionMap_WorldMap)
      ? 0 : kSimWorldMapVolatileRows;
  size_t offset = (size_t)first_row * kSimWorldMapTiles;
  size_t length = sizeof(g_world.tilemap) - offset;
  if (memcmp(g_world.tilemap + offset, shadow + offset, length) == 0) return;
  memcpy(g_world.tilemap + offset, shadow + offset, length);
  if (++g_world.serial == 0) g_world.serial = 1;
}

uint32_t SimWorldMap_Serial(void) {
  return g_world.available ? g_world.serial : 0;
}

bool SimWorldMap_Bake(uint32_t *pixels, int pitch_pixels) {
  if (!g_world.available || !pixels || pitch_pixels < kSimWorldMapPixels)
    return false;

  for (int tile_y = 0; tile_y < kSimWorldMapTiles; tile_y++) {
    for (int tile_x = 0; tile_x < kSimWorldMapTiles; tile_x++) {
      const uint8_t *art =
          g_world.tiles + g_world.tilemap[tile_y * kSimWorldMapTiles + tile_x] *
          kWorldTileBytes;
      for (int row = 0; row < kSimWorldMapTilePixels; row++) {
        uint32_t *out = pixels +
            (size_t)(tile_y * kSimWorldMapTilePixels + row) * pitch_pixels +
            tile_x * kSimWorldMapTilePixels;
        const uint8_t *source = art + row * kSimWorldMapTilePixels;
        for (int column = 0; column < kSimWorldMapTilePixels; column++)
          out[column] = g_world.palette[source[column]];
      }
    }
  }
  return true;
}
