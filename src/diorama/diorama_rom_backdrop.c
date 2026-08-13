#include "diorama_rom_backdrop.h"

#include <string.h>

#include "actraiser_game.h"

enum {
  kAssetScriptBase = 0x05 * 0x8000,
  kAssetScriptHeaderBytes = 3,       /* leading "SY\0" pseudo-entry */
  kAssetScriptEnd = 0x06 * 0x8000,
  kChrBytes = 0x2000,
  kChrVramBytes = kChrBytes * 2,
  /* Eight 16-colour BG palettes. Script destinations $00/$40 address the
   * lower/upper halves; $80 and above are OBJ colours and stay out of this
   * standalone BG reconstruction. */
  kPaletteBytes = 0x100,
  kMetatileBytes = 0x800,
  kMapPageBytes = 0x100,
  kMapMaxBytes = 16 * 4 * kMapPageBytes,
  kTileBytes4Bpp = 32,
  /* `$02:B6D3-$B6F6` installs $ECFF for all three action BG word masks.
   * `$02:B4E8-$B54C` then merges $10 into BG1 and $01 into BG2 whenever
   * `$18 != 0`. The native metatile builder applies both to every definition
   * word; omitting this transformed BG2 tile $000 into tile $100 and made a
   * byte-exact map/metatile decode render completely unrelated character art. */
  kActionBgTileWordMask = 0xECFF,
  kActionBg1Attributes = 0x10,
  kActionBg2Attributes = 0x01,
};

typedef struct ActionBgAssets {
  uint8_t chars[kChrVramBytes];
  uint8_t extra_chars[kChrBytes];
  uint8_t palette[kPaletteBytes];
  uint8_t metatiles[2][kMetatileBytes];
  uint8_t map[2][kMapMaxBytes];
  size_t map_size[2];
  bool have_chars[2];
  bool have_extra_chars;
  bool have_palette;
  bool have_metatiles[2];
  bool have_map[2];
} ActionBgAssets;

typedef struct BitReader {
  const uint8_t *data;
  size_t size;
  size_t bit;
} BitReader;

static bool ReadBits(BitReader *reader, unsigned count, unsigned *value) {
  if (!reader || !value || reader->bit > reader->size * 8 ||
      count > reader->size * 8 - reader->bit)
    return false;
  unsigned result = 0;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t byte = reader->data[reader->bit >> 3];
    result = (result << 1) |
        ((byte >> (7 - (reader->bit & 7))) & 1u);
    reader->bit++;
  }
  *value = result;
  return true;
}

/* Byte-exact host form of `$02:C5C9`: packed MSB-first tokens and a 256-byte
 * dictionary filled with spaces, write cursor `$EF`. */
static bool Decompress(const uint8_t *rom, size_t rom_size, size_t offset,
                       uint8_t *out, size_t expected_size) {
  if (!rom || !out || offset > rom_size || rom_size - offset < 2) return false;
  const size_t output_size =
      (size_t)rom[offset] | ((size_t)rom[offset + 1] << 8);
  if (output_size != expected_size) return false;
  BitReader reader = {rom + offset + 2, rom_size - offset - 2, 0};
  uint8_t dictionary[256];
  memset(dictionary, 0x20, sizeof(dictionary));
  unsigned write = 0xEF;
  size_t produced = 0;
  while (produced < output_size) {
    unsigned literal = 0;
    if (!ReadBits(&reader, 1, &literal)) return false;
    if (literal) {
      unsigned value = 0;
      if (!ReadBits(&reader, 8, &value)) return false;
      out[produced++] = (uint8_t)value;
      dictionary[write] = (uint8_t)value;
      write = (write + 1) & 0xFF;
      continue;
    }
    unsigned read = 0, length_code = 0;
    if (!ReadBits(&reader, 8, &read) ||
        !ReadBits(&reader, 4, &length_code))
      return false;
    for (unsigned n = length_code + 2;
         n && produced < output_size; n--) {
      const uint8_t value = dictionary[read];
      read = (read + 1) & 0xFF;
      out[produced++] = value;
      dictionary[write] = value;
      write = (write + 1) & 0xFF;
    }
  }
  return true;
}

static uint32_t Read24(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16);
}

static int HighestBit(uint8_t command) {
  for (int bit = 7; bit >= 0; bit--)
    if (command & (1u << bit)) return bit;
  return -1;
}

static int OperandBytesForBit(int bit) {
  static const uint8_t kOperandBytes[8] = {
    6, 5, 3, 1, 4, 7, 6, 6,
  };
  return bit >= 0 && bit < 8 ? kOperandBytes[bit] : 0;
}

static bool ApplyCommand(ActionBgAssets *assets,
                         const uint8_t *rom, size_t rom_size,
                         int bit, const uint8_t *ops) {
  if (!assets || !rom || !ops) return false;
  if (bit == 7) {
    /* `$02:B28E`: operands 0/1 bound a VRAM-word interval, operand 2
     * selects its destination high byte, and 3..5 are a linear ROM pointer.
     * Action BG characters occupy byte ranges $0000-$1FFF/$2000-$3FFF. */
    if (ops[1] <= ops[0]) return false;
    const size_t bytes = (size_t)(ops[1] - ops[0]) << 9;
    const size_t destination = (size_t)ops[2] << 9;
    const size_t source = Read24(ops + 3);
    if (bytes != kChrBytes)
      return true;                    /* valid command, not a BG char bank */
    if (destination < kChrVramBytes) {
      if (bytes > kChrVramBytes - destination ||
          !Decompress(rom, rom_size, source,
                      assets->chars + destination, bytes))
        return false;
      assets->have_chars[destination / kChrBytes] = true;
    } else if (destination == 0x6000) {
      /* The stock script uploads this bank at VRAM word $3000. BG1 metatile
       * words can select it through bit 9; keep it adjacent in the HLE tile
       * index domain rather than inventing a 64-KiB VRAM mirror. */
      if (!Decompress(rom, rom_size, source, assets->extra_chars, bytes))
        return false;
      assets->have_extra_chars = true;
    }
    return true;
  }
  if (bit == 6) {
    /* `$02:B330`: source colour interval [op0,op1), CGRAM destination op2.
     * Only BG palettes 0..7 belong in this standalone reconstruction. */
    if (ops[1] < ops[0]) return false;
    const size_t bytes = (size_t)(ops[1] - ops[0]) * 2;
    const size_t destination = (size_t)ops[2] * 2;
    const size_t source = Read24(ops + 3) + (size_t)ops[0] * 2;
    if (destination >= kPaletteBytes) return true;
    if (!bytes || bytes > kPaletteBytes - destination ||
        source > rom_size || bytes > rom_size - source)
      return false;
    memcpy(assets->palette + destination, rom + source, bytes);
    assets->have_palette = true;
    return true;
  }
  if (bit == 5) {
    /* `$02:B363`: selector op3 is 1=BG1, 2=BG2; pointer is op4..6. */
    if (ops[3] != 1 && ops[3] != 2) return true;
    const unsigned bg = ops[3] - 1;
    if (!Decompress(rom, rom_size, Read24(ops + 4),
                    assets->metatiles[bg], kMetatileBytes))
      return false;
    assets->have_metatiles[bg] = true;
    return true;
  }
  if (bit == 4) {
    /* `$02:B3EB`: selector op0 is 1=BG1, 2=BG2. Map header owns its
     * page dimensions; the compressed byte count must equal pages*256. */
    if (ops[0] != 1 && ops[0] != 2) return true;
    const unsigned bg = ops[0] - 1;
    const size_t source = Read24(ops + 1);
    if (source > rom_size || rom_size - source < 4) return false;
    const size_t pages = (size_t)rom[source] * rom[source + 1];
    if (!pages || pages > kMapMaxBytes / kMapPageBytes) return false;
    const size_t bytes = pages * kMapPageBytes;
    if (!Decompress(rom, rom_size, source + 2, assets->map[bg], bytes))
      return false;
    assets->map_size[bg] = bytes;
    assets->have_map[bg] = true;
    return true;
  }
  return true;                        /* non-graphics script command */
}

static bool LoadAssetsForRoom(const uint8_t *rom, size_t rom_size,
                              uint8_t group, uint8_t map,
                              ActionBgAssets *assets) {
  if (!rom || !assets || !ActRaiser_IsActionMap(group, map) ||
      rom_size <= kAssetScriptBase + kAssetScriptHeaderBytes)
    return false;
  memset(assets, 0, sizeof(*assets));
  size_t cursor = kAssetScriptBase + kAssetScriptHeaderBytes;
  const size_t end = rom_size < kAssetScriptEnd ? rom_size : kAssetScriptEnd;
  bool found = false;
  while (cursor + 3 <= end) {
    const uint8_t entry_group = rom[cursor++];
    const uint8_t entry_map = rom[cursor++];
    const bool apply = entry_group == group && entry_map <= map;
    while (cursor < end) {
      const uint8_t command = rom[cursor++];
      if (!command) break;
      const int bit = HighestBit(command);
      const int operand_bytes = OperandBytesForBit(bit);
      if (!operand_bytes || (size_t)operand_bytes > end - cursor) return false;
      if (apply && !ApplyCommand(
              assets, rom, rom_size, bit, rom + cursor))
        return false;
      cursor += (size_t)operand_bytes;
    }
    if (cursor > end) return false;
    if (entry_group == group && entry_map == map) found = true;
    if (entry_group > group || (entry_group == group && entry_map >= map))
      break;
  }
  return found;
}

bool DioramaRomBackdrop_DecompressAsset(const uint8_t *packed,
                                        size_t packed_size,
                                        uint8_t *out,
                                        size_t expected_size) {
  return Decompress(packed, packed_size, 0, out, expected_size);
}

static uint16_t Read16(const uint8_t *bytes) {
  return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint8_t Expand5(unsigned value) {
  value &= 31;
  return (uint8_t)((value << 3) | (value >> 2));
}

static unsigned TilePixel4Bpp(const uint8_t *chars, unsigned tile,
                              unsigned x, unsigned y) {
  const uint8_t *art = chars + tile * kTileBytes4Bpp;
  const unsigned shift = 7 - x;
  const uint16_t low = Read16(art + y * 2);
  const uint16_t high = Read16(art + (y + 8) * 2);
  return ((low >> shift) & 1u) | (((low >> (shift + 8)) & 1u) << 1) |
      (((high >> shift) & 1u) << 2) |
      (((high >> (shift + 8)) & 1u) << 3);
}

bool DioramaRomBackdrop_LoadActionBg(const uint8_t *rom, size_t rom_size,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer,
                                     uint32_t *out_argb,
                                     size_t out_pixel_count) {
  if (!rom || !out_argb ||
      out_pixel_count <
          (size_t)kDioramaRomBackdropPixels * kDioramaRomBackdropPixels ||
      (bg_layer != 1 && bg_layer != 2))
    return false;

  ActionBgAssets assets;
  const unsigned bg = bg_layer - 1;
  if (!LoadAssetsForRoom(rom, rom_size, map_group, map_number, &assets) ||
      !assets.have_chars[0] || !assets.have_chars[1] ||
      !assets.have_palette || !assets.have_metatiles[bg] ||
      !assets.have_map[bg] || assets.map_size[bg] < kMapPageBytes)
    return false;

  uint32_t palette[128];
  for (unsigned i = 0; i < 128; i++) {
    const uint16_t colour = Read16(assets.palette + i * 2);
    palette[i] = 0xFF000000u | (uint32_t)Expand5(colour) << 16 |
        (uint32_t)Expand5(colour >> 5) << 8 | Expand5(colour >> 10);
  }
  const uint16_t attributes = (uint16_t)(
      (bg_layer == 1 ? kActionBg1Attributes : kActionBg2Attributes) << 8);

  for (unsigned tile_y = 0; tile_y < 32; tile_y++) {
    for (unsigned tile_x = 0; tile_x < 32; tile_x++) {
      const uint8_t id = assets.map[bg][((tile_y >> 1) & 15u) * 16u +
                                        ((tile_x >> 1) & 15u)];
      const unsigned quadrant = ((tile_y & 1u) << 1) | (tile_x & 1u);
      /* `$02:B3CE` swaps every metatile word while copying it to WRAM. */
      const uint8_t *source = assets.metatiles[bg] +
          (size_t)id * 8 + quadrant * 2;
      const uint16_t definition =
          (uint16_t)(source[1] | ((uint16_t)source[0] << 8));
      const uint16_t entry =
          (definition & kActionBgTileWordMask) | attributes;
      const unsigned tile = entry & 0x3FFu;
      const unsigned palette_base = ((entry >> 10) & 7u) * 16u;
      const bool flip_x = (entry & 0x4000u) != 0;
      const bool flip_y = (entry & 0x8000u) != 0;
      const uint8_t *tile_chars = assets.chars;
      unsigned tile_index = tile;
      if (tile >= (sizeof(assets.chars) / kTileBytes4Bpp)) {
        if (!assets.have_extra_chars || tile < 0x200 || tile >= 0x300)
          return false;
        tile_chars = assets.extra_chars;
        tile_index = tile - 0x200;
      }
      for (unsigned py = 0; py < 8; py++) {
        for (unsigned px = 0; px < 8; px++) {
          const unsigned sx = flip_x ? 7 - px : px;
          const unsigned sy = flip_y ? 7 - py : py;
          const unsigned colour = TilePixel4Bpp(
              tile_chars, tile_index, sx, sy);
          /* Backdrop is an opaque replacement plane. Hardware colour zero
           * exposes CGRAM[0], which the named room-1 palette owns. */
          out_argb[(size_t)(tile_y * 8 + py) * kDioramaRomBackdropPixels +
                   tile_x * 8 + px] = palette[colour ? palette_base + colour : 0];
        }
      }
    }
  }
  return true;
}

bool DioramaRomBackdrop_LoadAitosSky(const uint8_t *rom, size_t rom_size,
                                    uint32_t *out_argb,
                                    size_t out_pixel_count) {
  return DioramaRomBackdrop_LoadActionBg(
      rom, rom_size, 0x04, 0x01, 2, out_argb, out_pixel_count);
}
