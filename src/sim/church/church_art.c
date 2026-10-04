#include "sim/church/church_art.h"

/* Palace and temple share the raw 4bpp bank at $0D:C000. These are the
 * Palace's right-hand capital and unobstructed shaft, before its BG2 dialogue
 * box. The original scanout's Y+1 and $F8 scroll put screen row 31 at the
 * capital's first row. Palette 1 is the native warm stone ramp ($1C:BEB3).
 * Tile references, flips and transparent silhouettes are layout metadata;
 * all actual texels and colors come from the user's ROM. */
/* The scene script relocates the palette between releases. Read its declared
 * raw assets rather than assuming US offsets or recognizing pixel values. */
static size_t Read24(const uint8_t *p) { return p[0] | ((size_t)p[1] << 8) | ((size_t)p[2] << 16); }

static bool FindAssets(const uint8_t *rom, size_t size, size_t *characters, size_t *palette) {
  enum { kScript = 0x28000, kScriptEnd = 0x30000 };
  static const uint8_t lengths[8] = {6, 5, 3, 1, 4, 7, 6, 6};
  if (!rom || size < kScriptEnd || rom[kScript] != 'S' || rom[kScript + 1] != 'Y' ||
      rom[kScript + 2])
    return false;
  size_t at = kScript + 3;
  while (at + 2 < kScriptEnd) {
    const bool palace = rom[at] == 0 && rom[at + 1] == 7;
    at += 2;
    while (at < kScriptEnd && rom[at]) {
      const uint8_t command = rom[at++];
      int bit = 7;
      while (!(command & (1u << bit)))
        bit--;
      if (at + lengths[bit] > kScriptEnd) return false;
      const uint8_t *op = rom + at;
      if (palace && bit == 7 && op[0] == 0x80 && op[1] == 0x20 && op[2] == 0)
        *characters = Read24(op + 3);
      if (palace && bit == 6 && op[0] == 0 && op[1] == 0x80 && op[2] == 0)
        *palette = Read24(op + 3) + 32;
      at += lengths[bit];
    }
    if (at >= kScriptEnd) return false;
    at++;
    if (palace)
      return *characters && *palette && *characters <= size && *palette <= size &&
             size - *characters >= 0x4000 && size - *palette >= 32;
  }
  return false;
}
static uint32_t s_columns[kChurchColumnWidth * kChurchColumnHeight];
static bool s_valid;

static uint32_t Pixel(const uint8_t *characters, const uint8_t *palette, uint16_t word, int x,
                      int y) {
  if (word & 0x4000) x = 7 - x;
  if (word & 0x8000) y = 7 - y;
  const uint8_t *tile = characters + (word & 0x3ff) * 32;
  unsigned index = 0;
  for (int plane = 0; plane < 4; plane++)
    index |= ((tile[(plane / 2) * 16 + y * 2 + plane % 2] >> (7 - x)) & 1) << plane;
  if (!index) return 0;
  const uint8_t *entry = palette + index * 2;
  const unsigned color = entry[0] | (entry[1] << 8);
  const unsigned red = color & 31, green = (color >> 5) & 31, blue = (color >> 10) & 31;
  return 0xff000000u | (((red << 3) | (red >> 2)) << 16) | (((green << 3) | (green >> 2)) << 8) |
         ((blue << 3) | (blue >> 2));
}

bool ChurchArt_Init(const uint8_t *rom, size_t size) {
  s_valid = false;
  size_t characters = 0, palette = 0;
  if (!FindAssets(rom, size, &characters, &palette)) return false;
  static const int inset[14] = {7, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  static const uint16_t capital[2][2] = {{0x6403, 0x6402}, {0x6413, 0x6412}};
  static const uint16_t shaft[4] = {0x6000, 0x2411, 0x6411, 0x6000};
  for (int y = 0; y < kChurchColumnHeight; y++) {
    const int cap = y < 14 ? y : y >= 130 ? 143 - y : -1;
    for (int x = 0; x < kChurchColumnWidth; x++) {
      const int sx = cap >= 0 ? (x < 14 ? 13 - x : x - 14) : 2 + x;
      const uint16_t tile = cap >= 0 ? capital[cap / 8][sx / 8] : shaft[sx / 8];
      s_columns[y * kChurchColumnWidth + x] =
          cap >= 0 && (x < inset[cap] || x >= kChurchColumnWidth - inset[cap])
              ? 0
              : Pixel(rom + characters, rom + palette, tile, sx & 7, cap >= 0 ? cap & 7 : 3);
    }
  }
  s_valid = true;
  return true;
}

const uint32_t *ChurchArt_ColumnPixels(void) { return s_valid ? s_columns : NULL; }
