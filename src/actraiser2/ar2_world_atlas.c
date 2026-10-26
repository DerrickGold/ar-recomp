#include "actraiser2/ar2_world_atlas.h"
#include "actraiser/quintet_lzss.h"
#include <string.h>

enum {
  kRomSize = 0x180000,
  kDestinationTable = 0x018000,
  kMapAsset = 0x041AA0,
  kTileAsset = 0x14DE55,
  kAssetBytes = 128 * 128,
};

static uint16_t Word(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }

static uint32_t DirectColor(uint8_t pixel) {
  /* Mode 7 direct color has no tile-palette bits: BBGGGRRR -> RGB555.
   * Expand the 5-bit DAC channels in the same way as our AR1 map baker. */
  unsigned r = (pixel & 7) << 5;
  unsigned g = (pixel & 0x38) << 2;
  unsigned b = pixel & 0xC0;
  r |= r >> 5;
  g |= g >> 5;
  b |= b >> 5;
  return 0xFF000000u | r << 16 | g << 8 | b;
}

static void ReadLabel(char *label, size_t capacity, const uint8_t *source, size_t length) {
  while (length && source[0] == ' ') {
    source++;
    length--;
  }
  while (length && source[length - 1] == ' ')
    length--;
  if (length >= capacity) length = capacity - 1;
  for (size_t i = 0; i < length; ++i) {
    /* Native text uses the x glyph slot for an apostrophe in DEMON'S CAVE. */
    const uint8_t c = source[i];
    label[i] = c == 'x' ? '\'' : c >= 32 && c < 127 ? (char)c : '?';
  }
  label[length] = 0;
}

bool Ar2WorldAtlas_Decode(Ar2WorldAtlas *out, const uint8_t *rom, size_t size) {
  if (!out) return false;
  out->available = false;
  out->destination_count = 0;
  /* Layout guard, not a cryptographic ROM identity check. The importer or
   * launcher owns revision hashing before this adapter is called. */
  if (!rom || size != kRomSize || memcmp(rom + 0xFFC0, "ActRaiser-2 USA", 14) ||
      rom[0xFFD5] != 0x31 || Word(rom + 0xFFDE) != 0xEFCC)
    return false;

  uint8_t map[kAssetBytes], tiles[kAssetBytes];
  if (!QuintetLzss_DecompressAsset(rom + kMapAsset, size - kMapAsset, map, sizeof(map), NULL) ||
      !QuintetLzss_DecompressAsset(rom + kTileAsset, size - kTileAsset, tiles, sizeof(tiles), NULL))
    return false;

  for (unsigned i = 0; i < kAr2WorldDestinationCapacity; ++i) {
    const uint8_t *record = rom + kDestinationTable + i * 16;
    Ar2WorldDestination *d = &out->destinations[i];
    *d = (Ar2WorldDestination){.x_min = Word(record),
                               .x_max = Word(record + 2),
                               .y_min = Word(record + 4),
                               .y_max = Word(record + 6),
                               .flags_and_scene = Word(record + 8),
                               .progress_list = Word(record + 10),
                               .unlock_gate = Word(record + 12),
                               .completion_gate = Word(record + 14),
                               .record_index = i};
    if (!d->x_min || d->x_min >= d->x_max || d->x_max > kAr2WorldAtlasPixels ||
        d->y_min >= d->y_max || d->y_max > kAr2WorldAtlasPixels)
      return false;
  }
  if (Word(rom + kDestinationTable + kAr2WorldDestinationCapacity * 16)) return false;

  /* Captions in native stage scripts; record indices retain the separate
   * town/action and pre/post-progression locations. No unlock flags are applied
   * here: the catalog is data, while campaign/prototype policy is the caller's. */
  static const uint8_t action_records[] = {1, 2, 3, 4, 5, 6, 8, 10, 11, 21, 14, 17, 18, 13};
  for (unsigned i = 0; i < sizeof(action_records); ++i)
    ReadLabel(out->destinations[action_records[i]].label, sizeof(out->destinations[0].label),
              rom + 0x181B5 + i * 21, 14);
  static const uint8_t town_records[] = {0, 20, 7, 9, 12, 15, 16, 19};
  for (unsigned i = 0; i < sizeof(town_records); ++i)
    ReadLabel(out->destinations[town_records[i]].label, sizeof(out->destinations[0].label),
              rom + 0x183DB + i * 30, 14);

  uint32_t palette[256];
  for (unsigned i = 0; i < 256; ++i)
    palette[i] = DirectColor(i);
  for (unsigned y = 0; y < kAr2WorldAtlasPixels; ++y)
    for (unsigned x = 0; x < kAr2WorldAtlasPixels; ++x) {
      const unsigned tile = map[(y / 8) * 128 + x / 8];
      const uint8_t index = tiles[tile * 64 + (y % 8) * 8 + x % 8];
      out->indices[y * kAr2WorldAtlasPixels + x] = index;
      out->pixels[y * kAr2WorldAtlasPixels + x] = palette[index];
    }
  out->destination_count = kAr2WorldDestinationCapacity;
  out->available = true;
  return true;
}
