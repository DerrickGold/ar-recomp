#include "sim_world_map.h"

#include <stdio.h>
#include <string.h>

/* ROM residency, all uncompressed and all verified byte-for-byte against a
 * live world-map capture (tilemap 16172/16384 and chr 16346/16384 identical;
 * the 38 CHR deltas are exactly the animated water in tiles $00/$AA; palette
 * 512/512 identical). */
enum {
  kWorldWaterFramesRomOffset = 0x053000, /* LoROM $0A:B000, 4 x 64B */
  kWorldTilemapRomOffset = 0x033341,  /* LoROM $06:B341, 128x128 bytes */
  kWorldTilesRomOffset = 0x070000,    /* LoROM $0E:8000, 256 x 64B 8bpp */
  kWorldPaletteRomOffset = 0x0E3F93,  /* LoROM $1C:BF93, 256 x BGR555 */
  kWorldTileCount = 256,
  /* Palette entries. 256 because an 8bpp pixel is one byte -- it happens to
   * equal kWorldTileCount and is not derived from it: the tile COUNT is how many
   * tiles the ROM blob holds, this is how many colours a pixel can name. */
  kWorldPaletteEntries = 256,
  kWorldTileBytes = 64,
  kWorldWaterFrameCount = 4,
  /* kWorldWaterSourceFirst / kWorldWaterSourceStride are shared with the
   * builder — see sim_world_map.h. */
  kWorldWaterTileFirst = 0x00,
  kWorldWaterTileSecond = 0xAA,
};

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
  bool developed;
  uint8_t tilemap[kSimWorldMapBytes];
  uint8_t tiles[kWorldTileCount * kWorldTileBytes];
  uint8_t water_frames[kWorldWaterFrameCount][kWorldTileBytes];
  uint16_t water_source;
  bool water_source_valid;
  uint32_t palette[kWorldPaletteEntries];
  uint32_t serial;
  /* One flag per tile (tilemap is one byte per tile, so this is indexed
   * identically). A tile is dirty when its tilemap byte changed since it was
   * last baked; Init marks all of them so the first bake is a full one. */
  uint8_t dirty[kSimWorldMapBytes];
  /* Persistent CPU-side baked image (ARGB8888, kSimWorldMapPixels square, tight
   * pitch). The palette expansion writes here and only for dirty tiles; every
   * bake then full-copies this into the caller's buffer. That copy is not
   * optional: the caller hands us a streaming-texture lock whose contents are
   * write-only and undefined, so baking only dirty tiles into it would leave
   * the rest as garbage from a previous unrelated use. */
  uint32_t pixels[kSimWorldMapPixels * kSimWorldMapPixels];
  /* Pristine base copied into the pure builder before it stamps the current
   * simulation development over it. */
  uint8_t baseline[kSimWorldMapBytes];
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
  size_t needed = kWorldPaletteRomOffset + kWorldPaletteEntries * 2;
  if (!rom_data || rom_size < needed) {
    fprintf(stderr,
            "[sim-worldmap] unavailable: ROM is %zu bytes, need %zu\n",
            rom_size, needed);
    return false;
  }
  memcpy(g_world.tilemap, rom_data + kWorldTilemapRomOffset,
         sizeof(g_world.tilemap));
  /* Retain the pristine base for every owned build. */
  memcpy(g_world.baseline, g_world.tilemap, sizeof(g_world.baseline));
  memcpy(g_world.tiles, rom_data + kWorldTilesRomOffset, sizeof(g_world.tiles));
  memcpy(g_world.water_frames, rom_data + kWorldWaterFramesRomOffset,
         sizeof(g_world.water_frames));
  for (int i = 0; i < kWorldPaletteEntries; i++) {
    const uint8_t *entry = rom_data + kWorldPaletteRomOffset + i * 2;
    g_world.palette[i] =
        ExpandBgr555((uint16_t)(entry[0] | ((uint16_t)entry[1] << 8)));
  }
  /* Force the first bake to be a full one: everything is "dirty" relative to
   * the (undefined) contents of the persistent pixels buffer. */
  memset(g_world.dirty, 1, sizeof(g_world.dirty));
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

int SimWorldMap_PublishBuiltTilemap(const uint8_t *tilemap) {
  if (!g_world.available || !tilemap) return 0;
  g_world.developed = true;
  int changed = 0;
  for (size_t i = 0; i < sizeof(g_world.tilemap); i++) {
    if (g_world.tilemap[i] == tilemap[i]) continue;
    g_world.tilemap[i] = tilemap[i];
    g_world.dirty[i] = 1;
    changed++;
  }
  if (!changed) return 0;
  if (++g_world.serial == 0) g_world.serial = 1;
  return changed;
}

int SimWorldMap_SetWaterAnimationSource(uint16_t source) {
  if (!g_world.available || source < kWorldWaterSourceFirst) return 0;
  const unsigned displacement = source - kWorldWaterSourceFirst;
  if (displacement % kWorldWaterSourceStride != 0) return 0;
  const unsigned frame = displacement / kWorldWaterSourceStride;
  if (frame >= kWorldWaterFrameCount ||
      (g_world.water_source_valid && g_world.water_source == source))
    return 0;

  /* $02:AF86 performs two high-byte-only VRAM DMAs from the same 64-byte
   * source: one to Mode-7 tile $00 and one to tile $AA. Mirror that operation
   * in host-owned art rather than observing the resulting live VRAM. */
  memcpy(g_world.tiles + kWorldWaterTileFirst * kWorldTileBytes,
         g_world.water_frames[frame], kWorldTileBytes);
  memcpy(g_world.tiles + kWorldWaterTileSecond * kWorldTileBytes,
         g_world.water_frames[frame], kWorldTileBytes);
  g_world.water_source = source;
  g_world.water_source_valid = true;

  int changed = 0;
  for (size_t i = 0; i < sizeof(g_world.tilemap); i++) {
    const uint8_t tile = g_world.tilemap[i];
    if (tile != kWorldWaterTileFirst && tile != kWorldWaterTileSecond)
      continue;
    g_world.dirty[i] = 1;
    changed++;
  }
  if (changed && ++g_world.serial == 0) g_world.serial = 1;
  return changed;
}

uint32_t SimWorldMap_Serial(void) {
  return g_world.available ? g_world.serial : 0;
}

bool SimWorldMap_DevelopedAvailable(void) {
  return g_world.available && g_world.developed;
}

/* The retained pristine ROM tilemap. Exposed as the pure builder's immutable
 * base without this module having to know how composition works. */
const uint8_t *SimWorldMap_Baseline(void) {
  return g_world.available ? g_world.baseline : NULL;
}

/* Palette-expand only the tiles that changed since their last bake into the
 * persistent CPU image, then clear their flags. This is the expensive part
 * (a palette lookup per pixel), and it is the whole point of the dirty
 * tracking: a single-tile edit touches 64 pixels, not 1,048,576.
 *
 * Idempotent, so both Bake and Downsample can call it: the second call in a
 * frame finds nothing dirty and does no work. Downsample must not skip it —
 * otherwise the mip would be built from a stale image whenever it ran before
 * the frame's Bake.  */
static void RefreshPersistentImage(void) {
  for (int tile_y = 0; tile_y < kSimWorldMapTiles; tile_y++) {
    for (int tile_x = 0; tile_x < kSimWorldMapTiles; tile_x++) {
      int tile_index = tile_y * kSimWorldMapTiles + tile_x;
      if (!g_world.dirty[tile_index]) continue;
      const uint8_t *art =
          g_world.tiles + g_world.tilemap[tile_index] * kWorldTileBytes;
      for (int row = 0; row < kSimWorldMapTilePixels; row++) {
        uint32_t *out = g_world.pixels +
            (size_t)(tile_y * kSimWorldMapTilePixels + row) * kSimWorldMapPixels +
            tile_x * kSimWorldMapTilePixels;
        const uint8_t *source = art + row * kSimWorldMapTilePixels;
        for (int column = 0; column < kSimWorldMapTilePixels; column++)
          out[column] = g_world.palette[source[column]];
      }
      g_world.dirty[tile_index] = 0;
    }
  }
}

bool SimWorldMap_Bake(uint32_t *pixels, int pitch_pixels) {
  if (!g_world.available || !pixels || pitch_pixels < kSimWorldMapPixels)
    return false;

  RefreshPersistentImage();

  /* Always copy the whole persistent image into the caller's buffer. The
   * caller's `pixels` is a streaming-texture lock: write-only with undefined
   * prior contents, so anything not written this call would be garbage. Copy
   * row by row because the caller's pitch may exceed kSimWorldMapPixels. */
  for (int y = 0; y < kSimWorldMapPixels; y++)
    memcpy(pixels + (size_t)y * pitch_pixels,
           g_world.pixels + (size_t)y * kSimWorldMapPixels,
           (size_t)kSimWorldMapPixels * sizeof(uint32_t));
  return true;
}

bool SimWorldMap_Downsample(uint32_t *pixels, int pitch_pixels, int divisor) {
  if (!g_world.available || !pixels || divisor < 1) return false;
  if (kSimWorldMapPixels % divisor != 0) return false;
  const int extent = kSimWorldMapPixels / divisor;
  if (pitch_pixels < extent) return false;

  RefreshPersistentImage();

  /* Reads g_world.pixels, never a caller-supplied mapping — see the header. */
  const uint32_t taps = (uint32_t)divisor * (uint32_t)divisor;
  for (int y = 0; y < extent; y++) {
    uint32_t *out = pixels + (size_t)y * pitch_pixels;
    for (int x = 0; x < extent; x++) {
      uint32_t alpha = 0, red = 0, green = 0, blue = 0;
      for (int sy = 0; sy < divisor; sy++) {
        const uint32_t *row = g_world.pixels +
            (size_t)(y * divisor + sy) * kSimWorldMapPixels +
            (size_t)x * divisor;
        for (int sx = 0; sx < divisor; sx++) {
          uint32_t texel = row[sx];
          alpha += (texel >> 24) & 0xFF;
          red += (texel >> 16) & 0xFF;
          green += (texel >> 8) & 0xFF;
          blue += texel & 0xFF;
        }
      }
      out[x] = ((alpha / taps) << 24) | ((red / taps) << 16) |
          ((green / taps) << 8) | (blue / taps);
    }
  }
  return true;
}
