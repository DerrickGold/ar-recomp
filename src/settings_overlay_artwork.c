#include "settings_overlay_artwork.h"
#include "settings_overlay_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "byte_order.h"
#include "quintet_lzss.h"

enum {
  kFontTileBytes = 0x1000,
  kFontAssetOffset = 0xBECFB,
  kFontAtlasWidth = 128,
  kFontAtlasHeight = 128,
  kDebugFontAtlasWidth = 96,
  kDebugFontAtlasHeight = 128,
  /* kDebugGlyphWidth/kDebugGlyphHeight/kDebugLineHeight and kGlyphSize live in
   * settings_overlay_internal.h — shared with the debug panel. */
  kDialogCharAssetOffset = 0x6C000,
  kDialogPaletteAssetOffset = 0xE3F73,
  kDialogAtlasWidth = 24,
  kDialogAtlasHeight = 24,
};

/* Pixel zero is transparent. The remaining entries are the original
 * dialog-font outline, blue shadow, and face colors, plus host-side dim and
 * warning remaps of those same three pixel classes. Every non-transparent
 * color here is a color from the game's own menu CGRAM. */
const uint32_t kTextPalettes[kTextStyle_Count][4] = {
  /* Normal: the game's white face over its blue shadow (CGRAM pal0 #3/#2). */
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 156, 205, 255), ARGB(255, 255, 255, 255) },
  /* Dim: recessed steel for unavailable/passive text. */
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 45, 63, 78), ARGB(255, 91, 111, 126) },
  /* Warning/cursor: the game's warm gold highlight (CGRAM pal0 #6). */
  { ARGB(0, 0, 0, 0), ARGB(255, 38, 21, 3),
    ARGB(255, 164, 98, 20), ARGB(255, 255, 180, 65) },
  /* Value: the game's light menu blue (CGRAM pal0 #2) so a row's value reads
   * distinct from its warm-white label without the neon cyan drift. */
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 49, 82, 164), ARGB(255, 156, 205, 255) },
};

/* Compact 5x8 fallback and supplemental punctuation, five pixels wide in the
 * low bits of each row, sharing a baseline on row 6 so caps and lowercase sit
 * on the same line and row 7 is free for descenders. The ROM font is the
 * normal path for menu chrome, but these host-authored masks keep the menu
 * usable if the supplied ROM does not match the verified asset layout — and
 * they are also the SOURCE of the small proportional-height font the
 * description panel and tab bar draw with, which is why the lowercase set is
 * authored for real rather than aliased onto the capitals. */
static const uint8_t kFallbackFont[128][8] = {
  [' '] = {0, 0, 0, 0, 0, 0, 0},
  ['!'] = {4, 4, 4, 4, 4, 0, 4},
  ['"'] = {10, 10, 10, 0, 0, 0, 0},
  ['#'] = {10, 31, 10, 10, 31, 10, 0},
  ['$'] = {4, 15, 20, 14, 5, 30, 4},
  ['%'] = {24, 25, 2, 4, 8, 19, 3},
  ['('] = {2, 4, 8, 8, 8, 4, 2},
  [')'] = {8, 4, 2, 2, 2, 4, 8},
  ['*'] = {0, 21, 14, 31, 14, 21, 0},
  ['+'] = {0, 4, 4, 31, 4, 4, 0},
  [','] = {0, 0, 0, 0, 4, 4, 8},
  ['-'] = {0, 0, 0, 31, 0, 0, 0},
  ['.'] = {0, 0, 0, 0, 0, 4, 4},
  ['/'] = {1, 2, 2, 4, 8, 8, 16},
  ['0'] = {14, 17, 19, 21, 25, 17, 14},
  ['1'] = {4, 12, 4, 4, 4, 4, 14},
  ['2'] = {14, 17, 1, 2, 4, 8, 31},
  ['3'] = {30, 1, 1, 14, 1, 1, 30},
  ['4'] = {2, 6, 10, 18, 31, 2, 2},
  ['5'] = {31, 16, 16, 30, 1, 1, 30},
  ['6'] = {14, 16, 16, 30, 17, 17, 14},
  ['7'] = {31, 1, 2, 4, 8, 8, 8},
  ['8'] = {14, 17, 17, 14, 17, 17, 14},
  ['9'] = {14, 17, 17, 15, 1, 1, 14},
  [':'] = {0, 4, 4, 0, 4, 4, 0},
  [';'] = {0, 4, 4, 0, 4, 4, 8},
  ['<'] = {2, 4, 8, 16, 8, 4, 2},
  ['='] = {0, 0, 31, 0, 31, 0, 0},
  ['>'] = {8, 4, 2, 1, 2, 4, 8},
  ['?'] = {14, 17, 1, 2, 4, 0, 4},
  ['['] = {14, 8, 8, 8, 8, 8, 14},
  [']'] = {14, 2, 2, 2, 2, 2, 14},
  ['_'] = {0, 0, 0, 0, 0, 0, 31},
  ['A'] = {14, 17, 17, 31, 17, 17, 17},
  ['B'] = {30, 17, 17, 30, 17, 17, 30},
  ['C'] = {15, 16, 16, 16, 16, 16, 15},
  ['D'] = {30, 17, 17, 17, 17, 17, 30},
  ['E'] = {31, 16, 16, 30, 16, 16, 31},
  ['F'] = {31, 16, 16, 30, 16, 16, 16},
  ['G'] = {15, 16, 16, 23, 17, 17, 15},
  ['H'] = {17, 17, 17, 31, 17, 17, 17},
  ['I'] = {31, 4, 4, 4, 4, 4, 31},
  ['J'] = {7, 2, 2, 2, 2, 18, 12},
  ['K'] = {17, 18, 20, 24, 20, 18, 17},
  ['L'] = {16, 16, 16, 16, 16, 16, 31},
  ['M'] = {17, 27, 21, 21, 17, 17, 17},
  ['N'] = {17, 25, 21, 19, 17, 17, 17},
  ['O'] = {14, 17, 17, 17, 17, 17, 14},
  ['P'] = {30, 17, 17, 30, 16, 16, 16},
  ['Q'] = {14, 17, 17, 17, 21, 18, 13},
  ['R'] = {30, 17, 17, 30, 20, 18, 17},
  ['S'] = {15, 16, 16, 14, 1, 1, 30},
  ['T'] = {31, 4, 4, 4, 4, 4, 4},
  ['U'] = {17, 17, 17, 17, 17, 17, 14},
  ['V'] = {17, 17, 17, 17, 17, 10, 4},
  ['W'] = {17, 17, 17, 21, 21, 21, 10},
  ['X'] = {17, 17, 10, 4, 10, 17, 17},
  ['Y'] = {17, 17, 10, 4, 4, 4, 4},
  ['Z'] = {31, 1, 2, 4, 8, 16, 31},
  /* Lowercase: x-height on rows 2-6, ascenders from row 0, descenders on
   * row 7. Only the small font renders these; the 8x8 menu font still folds
   * lowercase onto the capitals (see BuildFallbackFont) to match the ROM
   * dialog font's single-case letterforms. */
  ['a'] = {0, 0, 14, 1, 15, 17, 15, 0},
  ['b'] = {16, 16, 30, 17, 17, 17, 30, 0},
  ['c'] = {0, 0, 14, 16, 16, 16, 14, 0},
  ['d'] = {1, 1, 15, 17, 17, 17, 15, 0},
  ['e'] = {0, 0, 14, 17, 31, 16, 14, 0},
  ['f'] = {6, 8, 28, 8, 8, 8, 8, 0},
  ['g'] = {0, 0, 15, 17, 17, 15, 1, 14},
  ['h'] = {16, 16, 30, 17, 17, 17, 17, 0},
  ['i'] = {4, 0, 12, 4, 4, 4, 14, 0},
  ['j'] = {2, 0, 6, 2, 2, 2, 18, 12},
  ['k'] = {16, 16, 18, 20, 24, 20, 18, 0},
  ['l'] = {12, 4, 4, 4, 4, 4, 14, 0},
  ['m'] = {0, 0, 26, 21, 21, 21, 21, 0},
  ['n'] = {0, 0, 30, 17, 17, 17, 17, 0},
  ['o'] = {0, 0, 14, 17, 17, 17, 14, 0},
  ['p'] = {0, 0, 30, 17, 17, 30, 16, 16},
  ['q'] = {0, 0, 15, 17, 17, 15, 1, 1},
  ['r'] = {0, 0, 22, 25, 16, 16, 16, 0},
  ['s'] = {0, 0, 15, 16, 14, 1, 30, 0},
  ['t'] = {8, 8, 28, 8, 8, 8, 6, 0},
  ['u'] = {0, 0, 17, 17, 17, 19, 13, 0},
  ['v'] = {0, 0, 17, 17, 17, 10, 4, 0},
  ['w'] = {0, 0, 17, 17, 21, 21, 10, 0},
  ['x'] = {0, 0, 17, 10, 4, 10, 17, 0},
  ['y'] = {0, 0, 17, 17, 17, 15, 1, 14},
  ['z'] = {0, 0, 31, 2, 4, 8, 31, 0},
  ['\''] = {4, 4, 0, 0, 0, 0, 0, 0},
  ['&'] = {12, 18, 20, 8, 21, 18, 13, 0},
  ['@'] = {14, 17, 23, 21, 23, 16, 15, 0},
  ['{'] = {6, 8, 8, 16, 8, 8, 6, 0},
  ['}'] = {12, 2, 2, 1, 2, 2, 12, 0},
  ['|'] = {4, 4, 4, 4, 4, 4, 4, 0},
  ['\\'] = {16, 8, 8, 4, 2, 2, 1, 0},
  ['^'] = {4, 10, 17, 0, 0, 0, 0, 0},
  ['~'] = {0, 0, 9, 21, 18, 0, 0, 0},
  ['`'] = {8, 4, 0, 0, 0, 0, 0, 0},
};

/* Glyph-cache dimensions. Both are 256 for BYTE-INDEXING reasons and are not
 * related to each other or to any pixel dimension:
 *   TileIds   a tilemap entry names its tile with one byte.
 *   CharCodes the dictionary maps one source byte to one tile id.
 * The glyphs themselves are 8x8, which is why the coordinate guards below
 * compare against 8 rather than either of these. */
enum { kOverlayTileIds = 256, kOverlayCharCodes = 256 };

static SettingsOverlayArtwork s_artwork;
static ArRenderDevice *s_device;
static uint8_t s_font_tiles[kFontTileBytes];
static bool DecodeFontAsset(const uint8_t *rom_data, size_t rom_size) {
  if (!rom_data || rom_size < (size_t)kFontAssetOffset + 2)
    return false;
  const uint8_t *asset = rom_data + kFontAssetOffset;
  size_t output_size = (size_t)asset[0] | ((size_t)asset[1] << 8);
  if (output_size != kFontTileBytes)
    return false;

  QuintetLzssState state;
  if (!QuintetLzss_DecompressAsset(
          asset, rom_size - (size_t)kFontAssetOffset,
          s_font_tiles, output_size, &state))
    return false;

  fprintf(stderr,
          "[settings-menu] decoded ActRaiser font: $%04X bytes from ROM "
          "offset $%06X (%zu compressed bytes consumed)\n",
          kFontTileBytes, kFontAssetOffset,
          (state.bits_consumed + 7) / 8);
  return true;
}

static void SetTilePixel(unsigned tile, int x, int y, unsigned value) {
  if (tile >= kOverlayTileIds || x < 0 || x >= 8 || y < 0 || y >= 8)
    return;
  size_t offset = (size_t)tile * 16 + (size_t)y * 2;
  uint8_t mask = (uint8_t)(1u << (7 - x));
  if (value & 1) s_font_tiles[offset] |= mask;
  else s_font_tiles[offset] &= (uint8_t)~mask;
  if (value & 2) s_font_tiles[offset + 1] |= mask;
  else s_font_tiles[offset + 1] &= (uint8_t)~mask;
}

bool SettingsOverlayArtwork_HasDebugGlyph(unsigned ch) {
  if (ch == ' ') return true;
  if (ch >= 128) return false;
  for (int row = 0; row < 8; row++)
    if (kFallbackFont[ch][row]) return true;
  return false;
}

static void WriteFallbackGlyph(unsigned tile, unsigned source_ch) {
  if (tile >= kOverlayTileIds || source_ch >= 128)
    return;
  memset(s_font_tiles + tile * 16, 0, 16);
  for (int row = 0; row < 7; row++) {
    uint8_t bits = kFallbackFont[source_ch][row];
    for (int col = 0; col < 5; col++) {
      if (!(bits & (1u << (4 - col)))) continue;
      SetTilePixel(tile, col + 2, row + 1, 2);
    }
  }
  for (int row = 0; row < 8; row++) {
    uint8_t bits = kFallbackFont[source_ch][row];
    for (int col = 0; col < 5; col++) {
      if (bits & (1u << (4 - col)))
        SetTilePixel(tile, col + 1, row, 3);
    }
  }
}

/* ── Section nav icons ──────────────────────────────────────────────────
 * Real ActRaiser menu icons, lifted from a Sky Palace status-screen VRAM
 * snapshot (`snesbuild chr-render snapshot` + `chr-render icons`). Each is a
 * 16x16 4bpp index map — the game stores these as framed item/magic/status
 * glyphs — rendered through the game's OWN menu CGRAM palettes: the grey slot
 * palette (pal 14) for an unselected section, and the colored "selected slot"
 * palette (pal 13, red/gold frame) for the current one, exactly as the game
 * lights up the item you are pointing at. Index 14 is the black outline. */
enum {
  kIconAtlasWidth = kIconSize * kOverlayIcon_Count,
  /* Two stacked rows: grey (inactive) at y=0, colored (selected) below. */
  kIconAtlasHeight = kIconSize * 2,
};

/* The two 16-color menu palettes straight from the snapshot CGRAM (BGR555 →
 * RGB). kIconGreyPalette is pal 14 (unselected slots), kIconSelectPalette is
 * pal 13 (the highlighted slot: green/blue glyphs, red/gold frame). Index 0 is
 * transparent so the panel shows through the rounded corners. */
static const uint32_t kIconGreyPalette[16] = {
  0, ARGB(255, 90, 90, 90), ARGB(255, 115, 115, 115), ARGB(255, 164, 164, 164),
  ARGB(255, 189, 189, 189), ARGB(255, 222, 222, 222), ARGB(255, 246, 246, 246),
  ARGB(255, 197, 197, 197), ARGB(255, 238, 238, 238), ARGB(255, 205, 205, 205),
  ARGB(255, 156, 156, 156), ARGB(255, 131, 131, 131), ARGB(255, 82, 82, 82),
  ARGB(255, 255, 255, 255), ARGB(255, 0, 0, 0), ARGB(255, 41, 41, 41),
};
static const uint32_t kIconSelectPalette[16] = {
  0, ARGB(255, 49, 82, 164), ARGB(255, 82, 197, 0), ARGB(255, 180, 230, 0),
  ARGB(255, 115, 180, 230), ARGB(255, 197, 222, 230), ARGB(255, 230, 246, 255),
  ARGB(255, 255, 230, 0), ARGB(255, 255, 213, 172), ARGB(255, 213, 172, 131),
  ARGB(255, 230, 164, 0), ARGB(255, 255, 0, 0), ARGB(255, 164, 123, 82),
  ARGB(255, 255, 255, 255), ARGB(255, 0, 0, 0), ARGB(255, 0, 82, 0),
};

typedef uint8_t IconIndexMap[kIconSize][kIconSize];

/* Named identities keep section order independent of atlas order. */
static const IconIndexMap kSectionIconMaps[] = {
  [kOverlayIcon_Video] = { /* Display <- game icon #11 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,11,14,14,14,14,14,14,14,14,14,14,14,14,11,14},
    {14,11,14, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,14,11,14},
    {14,11,14, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,14,14,14},
    {14,11,14, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 6, 6,14},
    {14,14,14, 2, 2, 2, 2, 2, 2, 2, 5, 6, 6, 6, 6,14},
    {14, 6, 5, 3, 3, 3, 3, 3, 3, 5, 6, 6, 6, 6, 6,14},
    {14, 6, 6, 6, 5, 5, 4, 4, 4, 3, 4, 4, 5, 5, 5,14},
    {14, 6, 6, 6, 6, 5, 4, 5, 5, 5, 3, 3, 4, 4, 4,14},
    {14, 5, 5, 5, 4, 3, 3, 5, 5, 5, 5, 5, 2, 2, 3,14},
    {14, 4, 3, 3, 2, 2, 5, 5, 5, 5, 5, 5, 5,14,14,14},
    {14, 2, 2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5,14,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,11,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Action] = { /* Action 3D <- game icon #12 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14, 7, 7, 7, 7, 7, 7, 7, 7,14, 7, 7,14, 7, 7,14},
    {14, 7,14,14,14,14,14,14,14,14, 7,10,14,14, 7,14},
    {14, 7,14,15,15,15,15,15,15, 7,10,15,15,14, 7,14},
    {14, 7,14,15,15,15,15,15, 7, 7,12,15,15,14, 7,14},
    {14, 7,14,15, 7, 7, 7,10,10,10,10,10,10,14, 7,14},
    {14, 7,14,15,15,15,15, 6, 3, 5,15,15,15,14, 7,14},
    {14, 7,14,15,15,15, 5, 3, 5,15,15,15,15,14, 7,14},
    {14, 7,14,15,15,15, 6, 3, 5,15,15,15,15,14, 7,14},
    {14, 7,14,15,15, 6, 3, 5,15,15,15,15,15,14, 7,14},
    {14, 7,14,15, 6, 6, 3, 5,15,15,15,15,15,14, 7,14},
    {14, 7,14,13, 6, 3, 5,15,15,15,15,15,15,14, 7,14},
    {14, 7,14,13, 6, 4, 5,15,15,15,15,15,15,14, 7,14},
    {14, 7,14,13, 6, 6, 6,14,14,14,14,14,14,14, 7,14},
    {14, 7,14,13,13,13,14, 7, 7, 7, 7, 7, 7, 7, 7,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Town] = { /* Simulation <- game icon #10 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,14, 6,13,13, 6,14,11,11,11,11,14},
    {14,11,14,14,14,14,13,14,14,13,14,14,14,14,11,14},
    {14,11,14,15,15, 7, 6,13,13, 6, 7,15,15,14,11,14},
    {14,11,14,15, 7,10,10, 7,10, 7, 7, 7,15,14,11,14},
    {14,11,14,15, 7, 9, 8, 8, 8, 8,10, 7,15,14,11,14},
    {14,14,14,15, 7, 8, 1, 8, 1, 8, 8, 7,15,14,14,14},
    {14, 6,13, 6,10, 9, 1, 9, 1, 9, 9,10, 6,13, 6,14},
    {14, 6,13, 6, 9, 8, 8,12, 8, 8, 9, 9, 6,13, 6,14},
    {14,14, 6, 5,12, 9, 8, 8, 8, 9, 9,12, 5, 6,14,14},
    {14,11,14, 9, 8,12,12,12,12,12,12, 8, 9,14,11,14},
    {14,11,14, 8,15, 9, 8, 8, 8, 8, 8,12, 8,14,11,14},
    {14,11,14,15,15, 9, 8, 8, 8, 8, 8,12,15,14,11,14},
    {14,11,14,14,14,12, 8, 9, 9, 8, 9,12,14,14,11,14},
    {14,11,11,11,11,14, 9, 8,14, 8, 9,14,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Audio] = { /* Audio <- game icon #53 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,14, 9, 9,14,11,11,11,11,14,14, 9, 9,14,14},
    {14,11,14,10, 9,14,14,14,14,14,14,14, 9,10,14,14},
    {14,11,14,15, 9,15,15,15,15,15,15,15, 9,14,11,14},
    {14,11,14,15, 9,12,10,10,10,10,10,12, 9,14,11,14},
    {14,11,14,10, 9,15, 5,15, 5,15, 5,15, 9,10,14,14},
    {14,11,14, 9, 9,15,13,15,13,15,13,15, 9, 9,14,14},
    {14,11,14, 9,10,15,13,15,13,15,13,15,10, 9,14,14},
    {14,11,14, 9,10,15,13,15,13,15,13,15,10, 9,14,14},
    {14,11,14, 9,10,15,13,15,13,15,13,15,10, 9,14,14},
    {14,11,14, 9, 9,15,13,15,13,15,13,15, 9, 9,14,14},
    {14,11,14,10, 9, 9, 5,15, 5,15, 5, 9, 9,10,14,14},
    {14,11,14,15,10, 9, 9, 9, 9, 9, 9, 9,10,14,11,14},
    {14,11,14,14,14,10, 9,10,10,10, 9,10,14,14,11,14},
    {14,11,11,14,10, 9,10,10, 9,10,10, 9,10,14,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Controls] = { /* Input <- game icon #18 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14, 7, 7, 7, 7, 7, 7,14, 1,14, 7, 7, 7, 7, 7,14},
    {14, 7,14,14,14,14,14, 1,14,14,14,14,14,14, 7,14},
    {14, 7,14,15,15,15,15, 1,15,15,15,15,15,14, 7,14},
    {14, 7,14,15,15,15,15, 3,15,15,15,15,15,14, 7,14},
    {14,14, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 3,14,14},
    {14, 3, 5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 3, 5, 3,14},
    {14, 5, 5, 5,14, 5, 5, 5, 5, 3,11, 5, 3, 3, 5,14},
    {14, 5, 5,14,14,14, 5, 5, 5, 3, 3, 3,11, 3, 5,14},
    {14, 5, 5, 5,14, 5, 5, 5, 5, 3,11, 5, 3, 3, 5,14},
    {14, 3, 5, 5, 5, 5, 5, 5, 5, 5, 3, 3,11, 5, 3,14},
    {14,14, 3, 5, 5, 5, 3,15,15, 3, 5, 5, 5, 3,14,14},
    {14, 7,14,15,15,15,15,15,15,15,15,15,15,14, 7,14},
    {14, 7,14,14,14,14,14,14,14,14,14,14,14,14, 7,14},
    {14, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Cheats] = { /* Cheats <- game icon #14 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,11,14,14,14,14,11,11,14,14,13,14},
    {14,11,14,14,14,14, 5,13,13, 5,14,14, 5,13,14,14},
    {14,11,14,15, 5,13, 7, 7, 7, 7,13,13,13, 5,14,14},
    {14,11,14, 5,13, 7,15,15,15,15, 7,13,13,14,11,14},
    {14,11,14,13, 7,15, 4, 6, 6, 4,15, 7,13,14,11,14},
    {14,14, 5, 7,15, 4, 6,13,13, 6, 4,15, 7, 5,14,14},
    {14,14,13, 7,15, 6,13,13,13,13, 6,15, 7,13,14,14},
    {14,14,13, 7,15, 6,13,13,13,13, 6,15, 7,13,14,14},
    {14,14, 5, 7,15, 4, 6,13,13, 6, 4,15, 7, 5,14,14},
    {14,11,14,13, 7,15, 4, 6, 6, 4,15, 7,13,14,11,14},
    {14,11,14,13,13, 7,15,15,15,15, 7,13, 5,14,11,14},
    {14,14, 5,13,13,13, 7, 7, 7, 7,13, 5,15,14,11,14},
    {14,14,13, 5,14,14, 5,13,13, 5,14,14,14,14,11,14},
    {14,13,14,14,11,11,14,14,14,14,11,11,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Save] = { /* Save <- game icon #16 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,14, 7,13,13, 7,14,14,14,14,14,11,11,11,14},
    {14,11,14,14,10, 7,13, 7, 5, 6, 6, 4,14,14,11,14},
    {14,11,14,15,15,10, 7, 7, 6,13,13,13, 6, 4,14,14},
    {14,11,14,15, 1, 4,10, 7, 7, 6, 6,13,13, 6,14,14},
    {14,11,14,15, 4, 5,12,10, 7, 7,10,13,10,13,14,14},
    {14,11,14,15, 4,12, 4, 5, 4, 6, 8, 8, 9,13,14,14},
    {14,11,14,15, 4, 5, 5, 4, 8, 6, 8, 1, 8,14,11,14},
    {14,11,14,15, 4, 5, 4, 8, 8, 5, 8, 8, 8,14,11,14},
    {14,11,14,15, 4, 5, 5, 9, 8, 8, 8, 8, 8,14,11,14},
    {14,11,14, 4, 4, 4, 5,12, 9, 8, 8, 8,15,14,11,14},
    {14,14, 4, 4, 5, 5, 6,12,12, 9, 8, 9,15,14,11,14},
    {14,14,12,10, 7, 7, 7, 5, 6,12,15,15,15,14,11,14},
    {14,12,10, 7, 7, 8,13, 7, 5, 6,13,14,14,14,11,14},
    {14,12,10, 7, 7, 7, 7, 7, 7, 5, 6,13,14,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Manual] = { /* Manual -- an open book. Drawn rather than lifted from the ROM: no game
     * icon reads as "documentation", and this section had NO icon at all until
     * 2026-08-03, which silently shifted every icon after it by one and left
     * the last section (Layers) on the zero-filled tail of this array. */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,11,14,14,14,14,14,14,14,14,14,14,14,14,11,14},
    {14,11,14, 6, 6, 6, 6,14,14, 6, 6, 6, 6,14,11,14},
    {14,11,14,13,13,13,13,14,14,13,13,13,13,14,11,14},
    {14,11,14,13,13,13,13,14,14,13,13,13,13,14,11,14},
    {14,11,14,13, 4, 4, 4,14,14, 4, 4, 4,13,14,11,14},
    {14,11,14,13,13,13,13,14,14,13,13,13,13,14,11,14},
    {14,11,14,13, 4, 4, 4,14,14, 4, 4, 4,13,14,11,14},
    {14,11,14,13,13,13,13,14,14,13,13,13,13,14,11,14},
    {14,11,14,13, 4, 4, 4,14,14, 4, 4, 4,13,14,11,14},
    {14,11,14,13,13,13,13,14,14,13,13,13,13,14,11,14},
    {14,11,14,12,12,12,12,12,12,12,12,12,12,14,11,14},
    {14,11,14,14,14,14,14,14,14,14,14,14,14,14,11,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_System] = { /* Extras <- game icon #54 */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,14,14,14,14,11,11,11,11,11,11,11,14},
    {14,11,14,14, 4, 4, 3, 3,14,14,14,14,14,14,11,14},
    {14,11,14,15, 6, 6, 6, 5, 3, 4,15,15,15,14,11,14},
    {14,11,14,15, 6, 5, 5, 5, 6, 5, 3, 4,15,14,11,14},
    {14,11,14,15, 4,14,14,14, 4,14,14, 6,15,14,11,14},
    {14,11,14,15, 5, 4, 5, 5, 5, 5, 4, 6,15,14,11,14},
    {14,11,14,15, 4,14,14, 4,14,14,14, 5,15,14,11,14},
    {14,11,14,15, 4, 5, 5, 5, 5, 5, 5, 4,15,14,11,14},
    {14,11,14,15, 5,14,14,14,14, 3,14, 5,15,14,11,14},
    {14,11,14,15, 4, 5, 4, 5, 5, 5, 5, 5,15,14,11,14},
    {14,11,14,15, 4, 5,14,14, 4, 5, 4, 5,15,14,11,14},
    {14,11,14,15, 4, 5, 4, 5, 5, 5, 5, 4,15,14,11,14},
    {14,11,14,14, 4, 4, 4, 5, 5, 5, 5, 3,14,14,11,14},
    {14,11,11,11,14,14,14,14,14,14,14,14,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Localization] = { /* Localization: an independently drawn speech bubble. */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,15, 3, 3, 3, 3, 3, 3, 3, 3,15,15,15,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,15, 3, 3, 3, 3, 3, 3, 3, 3,15,15,15,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,15, 3, 3, 3, 3, 3,15,15,15,15,15,15,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,15,15,15,15,15,15,15,15,15,15,15,15,11,14},
    {14,11,11,11,11,11,11,15,15,11,11,11,11,11,11,14},
    {14,14,14,14,14,14,11,15,11,14,14,14,14,14,14,14},
    {14,14,14,14,14,11,15,11,14,14,14,14,14,14,14,14},
    {14,14,14,14,11,11,11,14,14,14,14,14,14,14,14,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Randomizer] = { /* Randomizer <- sim composition $01:D128, the red/blue double arrow the
     * game uses for its own swap/exchange menu glyph. Lifted from the sim
     * object catalog (docs/research/sim-object-catalog/spawn_compositions_01.png,
     * row 4 col 1) rather than the Sky Palace sheet the icons above came from,
     * so its colours were quantised onto kIconSelectPalette: dark green, yellow,
     * red and pale blue land within 2 units, the vivid blue and orange are the
     * nearest available. */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,14},
    {14, 7,14,14,14,14,14,14,14,14,14,14,14,14, 7,14},
    {14, 7,14,15,15,15,15,15,15,15,15,15,15,14, 7,14},
    {14, 7,14,15,11,15,15,15,15,15,15, 1,15,14, 7,14},
    {14, 7,14,11,11,15,15,15,15,15,15, 1, 1,14, 7,14},
    {14,14,11,11,11,11,11,11, 1, 1, 1, 1, 1, 1,14,14},
    {14,11,11,11,11,11,11,11, 1, 1, 1, 1, 1, 1, 1,14},
    {14, 5,11,11,11,11,11,11, 1, 1, 1, 1, 1, 1,10,14},
    {14,14, 5,11,11, 5, 5, 5,10,10,10, 1, 1,10,14,14},
    {14, 7,14, 5,11,15,15,15,15,15,15, 1,10,14, 7,14},
    {14, 7,14,15, 5,15,15,15,15,15,15,10,15,14, 7,14},
    {14, 7,14,15,15,15,15,15,15,15,15,15,15,14, 7,14},
    {14, 7,14,14,14,14,14,14,14,14,14,14,14,14, 7,14},
    {14, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
  [kOverlayIcon_Layers] = { /* Layers -- three stacked planes receding in depth, which is what the
     * section authors. Drawn here rather than lifted from the ROM because no
     * game icon depicts layered planes; it follows the same framed convention
     * (index 14 transparent, 11 frame, 15 field) as the borrowed ones. */
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,11,14,14,14,14,14,14,14,14,14,14,14,14,11,14},
    {14,11,14,14, 3, 3, 3, 3, 3, 3, 3, 3,14,14,11,14},
    {14,11,14, 3, 5, 5, 5, 5, 5, 5, 5, 5, 3,14,11,14},
    {14,11,14,14, 4, 4, 4, 4, 4, 4, 4, 4,14,14,11,14},
    {14,11,14,15,14,14,14,14,14,14,14,14,15,14,11,14},
    {14,11,14,14, 6, 6, 6, 6, 6, 6, 6, 6,14,14,11,14},
    {14,11,14, 6,13,13,13,13,13,13,13,13, 6,14,11,14},
    {14,11,14,14,12,12,12,12,12,12,12,12,14,14,11,14},
    {14,11,14,15,14,14,14,14,14,14,14,14,15,14,11,14},
    {14,11,14,14, 9, 9, 9, 9, 9, 9, 9, 9,14,14,11,14},
    {14,11,14, 9,10,10,10,10,10,10,10,10, 9,14,11,14},
    {14,11,14,14, 8, 8, 8, 8, 8, 8, 8, 8,14,14,11,14},
    {14,11,11,11,11,11,11,11,11,11,11,11,11,11,11,14},
    {14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14},
  },
};
_Static_assert((int)(sizeof(kSectionIconMaps) / sizeof(kSectionIconMaps[0]))
                   == kOverlayIcon_Count,
               "one nav icon per menu section");

ArRenderTexture SettingsOverlayArtwork_CreateAtlas(ArRenderDevice *device, int width, int height,
                                                   const uint32_t *pixels) {
  ArRenderTexture texture = ArRenderTexture_Invalid();
  const ArRenderTextureDesc desc = {
      .width = width,
      .height = height,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Static,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(device, &desc, &texture) ||
      !ArRenderDevice_UpdateTexture(device, texture, NULL, pixels, width * (int)sizeof(*pixels))) {
    ArRenderDevice_DestroyTexture(device, texture);
    return ArRenderTexture_Invalid();
  }
  return texture;
}

static ArRenderTexture CreateIconAtlas(void) {
  uint32_t *pixels =
      (uint32_t *)calloc((size_t)kIconAtlasWidth * kIconAtlasHeight, sizeof(uint32_t));
  if (!pixels) return ArRenderTexture_Invalid();

  /* Row 0 = grey (unselected), row 1 = colored (selected), each icon straight
   * through the game palette so the colors are the game's own. */
  for (int row = 0; row < 2; row++) {
    const uint32_t *palette = row == 0 ? kIconGreyPalette : kIconSelectPalette;
    for (int section = 0; section < kOverlayIcon_Count; section++) {
      for (int y = 0; y < kIconSize; y++) {
        for (int x = 0; x < kIconSize; x++) {
          uint8_t index = kSectionIconMaps[section][y][x];
          uint32_t color = palette[index];
          if ((color >> 24) == 0) continue; /* transparent index */
          pixels[(row * kIconSize + y) * kIconAtlasWidth + section * kIconSize + x] = color;
        }
      }
    }
  }

  const ArRenderTexture texture =
      SettingsOverlayArtwork_CreateAtlas(s_device, kIconAtlasWidth, kIconAtlasHeight, pixels);
  free(pixels);
  return texture;
}

/* Independently drawn miniature flags/emblem, using the menu's red, blue,
 * white and gold. Fixed pixels + nearest sampling, not platform emoji. These
 * are a separate atlas: adding provenance badges cannot reorder nav icons. */
static const char kRegionBadgeMasks[kOverlayRegionBadge_Count][kRegionBadgeHeight][kRegionBadgeWidth + 1] = {
  [kOverlayRegionBadge_US] = {
    " GGGGGGGGGGGGGGGGGGGGGG ",
    "Gbbbbbbbbbbrrrrrrrrrrrrg",
    "GbwbbbwbbbwWWWWWWWWWWWWg",
    "Gbbbwbbbwbbrrrrrrrrrrrrg",
    "GbwbbbwbbbwWWWWWWWWWWWWg",
    "Gbbbwbbbwbbrrrrrrrrrrrrg",
    "GbwbbbwbbbwWWWWWWWWWWWWg",
    "Gbbbbbbbbbbrrrrrrrrrrrrg",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "Grrrrrrrrrrrrrrrrrrrrrrg",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "Grrrrrrrrrrrrrrrrrrrrrrg",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "Grrrrrrrrrrrrrrrrrrrrrrg",
    " gggggggggggggggggggggg ",
    "  ssssssssssssssssssssss",
  },
  [kOverlayRegionBadge_Japan] = {
    " GGGGGGGGGGGGGGGGGGGGGG ",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "GWWWWWWWWWWrrWWWWWWWWWWg",
    "GWWWWWWWWrrrrrrWWWWWWWWg",
    "GWWWWWWWrrrrrrrrWWWWWWWg",
    "GWWWWWWWrrrrrrrrWWWWWWWg",
    "GWWWWWWWrrrrrrrrWWWWWWWg",
    "GWWWWWWWrrrrrrrrWWWWWWWg",
    "GWWWWWWWrrrrrrrrWWWWWWWg",
    "GWWWWWWWWrrrrrrWWWWWWWWg",
    "GWWWWWWWWWWrrWWWWWWWWWWg",
    "GWWWWWWWWWWWWWWWWWWWWWWg",
    "Gwwwwwwwwwwwwwwwwwwwwwwg",
    " gggggggggggggggggggggg ",
    "  ssssssssssssssssssssss",
  },
  [kOverlayRegionBadge_Europe] = {
    " GGGGGGGGGGGGGGGGGGGGGG ",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    "GbbbbbbbbbbGbbbbbbbbbbbg",
    "GbbbbbbbbGbbbGbbbbbbbbbg",
    "GbbbbbbGbbbbbbbGbbbbbbbg",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    "GbbbbbGbbbbbbbbbGbbbbbbg",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    "GbbbbbbGbbbbbbbGbbbbbbbg",
    "GbbbbbbbbGbbbGbbbbbbbbbg",
    "GbbbbbbbbbbGbbbbbbbbbbbg",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    "Gbbbbbbbbbbbbbbbbbbbbbbg",
    " gggggggggggggggggggggg ",
    "  ssssssssssssssssssssss",
  },
  [kOverlayRegionBadge_Mixed] = {
    " GGGGGGGGGGGGGGGGGGGGGG ",
    "Gssssssssssssssssssssssg",
    "Gssssssssssssssssssssssg",
    "Gsssrrrrrrsswwwwwwsssssg",
    "GsssrrrrrrssWWWWWWsssssg",
    "GsssrrrrrrssWWWWWWsssssg",
    "GsssrrrrrrssWWWWWWsssssg",
    "Gssssssssssssssssssssssg",
    "GsssbbbbbbssGGGGGGsssssg",
    "GsssbbbbbbssGGGGGGsssssg",
    "GsssbbbbbbssGGGGGGsssssg",
    "Gsssbbbbbbssggggggsssssg",
    "Gssssssssssssssssssssssg",
    "Gssssssssssssssssssssssg",
    " gggggggggggggggggggggg ",
    "  ssssssssssssssssssssss",
  },
};

static ArRenderTexture CreateRegionBadgeAtlas(void) {
  enum { width = kRegionBadgeWidth * kOverlayRegionBadge_Count };
  uint32_t pixels[width * kRegionBadgeHeight];
  for (int badge = 0; badge < kOverlayRegionBadge_Count; ++badge) {
    for (int y = 0; y < kRegionBadgeHeight; ++y) {
      for (int x = 0; x < kRegionBadgeWidth; ++x) {
        uint32_t color = 0;
        switch (kRegionBadgeMasks[badge][y][x]) {
          case 'G': color = kIconSelectPalette[7]; break;
          case 'g': color = kIconSelectPalette[10]; break;
          case 'b': color = kIconSelectPalette[1]; break;
          case 'r': color = kIconSelectPalette[11]; break;
          case 'W': color = kIconSelectPalette[13]; break;
          case 'w': color = kIconSelectPalette[5]; break;
          case 's': color = ARGB(255, 0, 0, 0); break;
        }
        pixels[y * width + badge * kRegionBadgeWidth + x] = color;
      }
    }
  }
  return SettingsOverlayArtwork_CreateAtlas(s_device, width, kRegionBadgeHeight, pixels);
}

static void BuildFallbackFont(void) {
  memset(s_font_tiles, 0, sizeof(s_font_tiles));
  memset(s_artwork.glyph_defined, 0, sizeof(s_artwork.glyph_defined));
  for (unsigned ch = 0; ch < 128; ch++) {
    unsigned source_ch = ch;
    if (ch >= 'a' && ch <= 'z') source_ch = ch - 'a' + 'A';
    if (!SettingsOverlayArtwork_HasDebugGlyph(source_ch)) continue;
    WriteFallbackGlyph(ch, source_ch);
    s_artwork.glyph_defined[ch] = true;
  }
  s_artwork.glyph_defined[' '] = true;
  fprintf(stderr, "[settings-menu] ROM font unavailable; using host fallback font\n");
}

static void PrepareRomFont(void) {
  memset(s_artwork.glyph_defined, 0, sizeof(s_artwork.glyph_defined));
  for (unsigned ch = 0x20; ch < 0x80; ch++) s_artwork.glyph_defined[ch] = true;
  s_artwork.glyph_defined['@'] = false;

  /* These nominal ASCII slots contain game-specific symbols rather than text.
   * Supply host-authored punctuation while retaining every real alphabetic,
   * numeric, and selector tile from the ROM. */
  WriteFallbackGlyph(':', ':');
  WriteFallbackGlyph('%', '%');
  WriteFallbackGlyph('$', '$');
  WriteFallbackGlyph('*', '*');
}

static unsigned FontPixel(unsigned tile, int x, int y) {
  size_t offset = (size_t)tile * 16 + (size_t)y * 2;
  uint8_t mask = (uint8_t)(1u << (7 - x));
  unsigned plane0 = (s_font_tiles[offset] & mask) != 0;
  unsigned plane1 = (s_font_tiles[offset + 1] & mask) != 0;
  return plane0 | (plane1 << 1);
}

static ArRenderTexture CreateFontAtlas(TextStyle style) {
  uint32_t *pixels =
      (uint32_t *)calloc((size_t)kFontAtlasWidth * kFontAtlasHeight, sizeof(uint32_t));
  if (!pixels) return ArRenderTexture_Invalid();

  for (unsigned tile = 0; tile < kOverlayTileIds; tile++) {
    int tile_x = (int)(tile & 15) * kGlyphSize;
    int tile_y = (int)(tile >> 4) * kGlyphSize;
    for (int y = 0; y < kGlyphSize; y++) {
      for (int x = 0; x < kGlyphSize; x++) {
        unsigned pixel = FontPixel(tile, x, y);
        pixels[(tile_y + y) * kFontAtlasWidth + tile_x + x] = kTextPalettes[style][pixel];
      }
    }
  }

  const ArRenderTexture texture =
      SettingsOverlayArtwork_CreateAtlas(s_device, kFontAtlasWidth, kFontAtlasHeight, pixels);
  free(pixels);
  return texture;
}

static ArRenderTexture CreateDebugFontAtlas(void) {
  uint32_t *pixels =
      (uint32_t *)calloc((size_t)kDebugFontAtlasWidth * kDebugFontAtlasHeight, sizeof(uint32_t));
  if (!pixels) return ArRenderTexture_Invalid();

  for (unsigned ch = 0; ch < kOverlayCharCodes; ch++) {
    unsigned source_ch = ch;
    /* Lowercase is authored for real now; fold onto the capital only for the
     * handful of codepoints that still have no lowercase mask. */
    if (source_ch >= 128 || !SettingsOverlayArtwork_HasDebugGlyph(source_ch)) {
      if (source_ch >= 'a' && source_ch <= 'z')
        source_ch = source_ch - 'a' + 'A';
      else
        source_ch = '?';
    }
    if (source_ch >= 128 || !SettingsOverlayArtwork_HasDebugGlyph(source_ch)) source_ch = '?';
    int cell_x = (int)(ch & 15) * kDebugGlyphWidth;
    int cell_y = (int)(ch >> 4) * kDebugGlyphHeight;
    for (int row = 0; row < 8; row++) {
      uint8_t bits = kFallbackFont[source_ch][row];
      for (int col = 0; col < 5; col++) {
        if (bits & (1u << (4 - col)))
          pixels[(cell_y + row) * kDebugFontAtlasWidth + cell_x + col] = ARGB(255, 255, 255, 255);
      }
    }
  }

  const ArRenderTexture texture = SettingsOverlayArtwork_CreateAtlas(s_device, kDebugFontAtlasWidth,
                                                                     kDebugFontAtlasHeight, pixels);
  free(pixels);
  return texture;
}

static void DestroyFontTextures(void) {
  for (int i = 0; i < kTextStyle_Count; i++) {
    ArRenderDevice_DestroyTexture(s_device, s_artwork.fonts[i]);
    s_artwork.fonts[i] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(s_device, s_artwork.debug_font);
  s_artwork.debug_font = ArRenderTexture_Invalid();
}

static uint32_t DialogColor(const uint8_t *palette, unsigned index) {
  if (index == 0) return ARGB(0, 0, 0, 0);
  uint16_t color = ByteOrder_ReadLe16(palette + index * 2);
  unsigned red = (color & 0x1f) * 255 / 31;
  unsigned green = ((color >> 5) & 0x1f) * 255 / 31;
  unsigned blue = ((color >> 10) & 0x1f) * 255 / 31;
  return ARGB(255, red, green, blue);
}

static unsigned DialogTilePixel(const uint8_t *tile, int x, int y) {
  unsigned mask = 1u << (7 - x);
  unsigned plane0 = (tile[y * 2] & mask) != 0;
  unsigned plane1 = (tile[y * 2 + 1] & mask) != 0;
  unsigned plane2 = (tile[16 + y * 2] & mask) != 0;
  unsigned plane3 = (tile[16 + y * 2 + 1] & mask) != 0;
  return plane0 | (plane1 << 1) | (plane2 << 2) | (plane3 << 3);
}

static void DecodeDialogAtlasTile(uint32_t *pixels, int atlas_column, int atlas_row,
                                  const uint8_t *characters, const uint8_t *palette,
                                  unsigned tile_index, bool vertical_flip) {
  const uint8_t *tile = characters + tile_index * 32;
  int destination_x = atlas_column * kGlyphSize;
  int destination_y = atlas_row * kGlyphSize;
  for (int y = 0; y < kGlyphSize; y++) {
    int source_y = vertical_flip ? kGlyphSize - 1 - y : y;
    for (int x = 0; x < kGlyphSize; x++) {
      unsigned pixel = DialogTilePixel(tile, x, source_y);
      pixels[(destination_y + y) * kDialogAtlasWidth + destination_x + x] =
          DialogColor(palette, pixel);
    }
  }
}

/* Sky Palace's 16x16 metatiles mix the lower dialog corners with tile $18,
 * which is palace scenery. Decode the six actual 8x8 frame characters instead:
 *
 *   $CE  vflip($EE)  $CF
 *   $DE      $FF      $DF
 *   vflip($CE) $EE   vflip($CF)
 *
 * $FF is the opaque black center. Palette index zero remains transparent so
 * the beveled corner cutouts and gutters show the paused game underneath. */
static ArRenderTexture CreateDialogFrameTexture(const uint8_t *rom_data, size_t rom_size) {
  size_t character_end = (size_t)kDialogCharAssetOffset + (size_t)(0xff + 1) * 32;
  size_t palette_end = (size_t)kDialogPaletteAssetOffset + 32;
  if (!rom_data || rom_size < character_end || rom_size < palette_end) {
    fprintf(stderr,
            "[settings-menu] native dialog frame unavailable; "
            "using host frame fallback\n");
    return ArRenderTexture_Invalid();
  }

  const uint8_t *characters = rom_data + kDialogCharAssetOffset;
  const uint8_t *palette = rom_data + kDialogPaletteAssetOffset;
  uint32_t pixels[kDialogAtlasWidth * kDialogAtlasHeight];
  memset(pixels, 0, sizeof(pixels));
  DecodeDialogAtlasTile(pixels, 0, 0, characters, palette, 0xce, false);
  DecodeDialogAtlasTile(pixels, 1, 0, characters, palette, 0xee, true);
  DecodeDialogAtlasTile(pixels, 2, 0, characters, palette, 0xcf, false);
  DecodeDialogAtlasTile(pixels, 0, 1, characters, palette, 0xde, false);
  DecodeDialogAtlasTile(pixels, 1, 1, characters, palette, 0xff, false);
  DecodeDialogAtlasTile(pixels, 2, 1, characters, palette, 0xdf, false);
  DecodeDialogAtlasTile(pixels, 0, 2, characters, palette, 0xce, true);
  DecodeDialogAtlasTile(pixels, 1, 2, characters, palette, 0xee, false);
  DecodeDialogAtlasTile(pixels, 2, 2, characters, palette, 0xcf, true);

  const ArRenderTexture texture =
      SettingsOverlayArtwork_CreateAtlas(s_device, kDialogAtlasWidth, kDialogAtlasHeight, pixels);
  if (ArRenderTexture_IsValid(texture)) {
    fprintf(stderr,
            "[settings-menu] decoded native dialog frame: "
            "chars ROM $%06X, palette ROM $%06X\n",
            kDialogCharAssetOffset, kDialogPaletteAssetOffset);
  }
  return texture;
}

/* (Re)create every overlay texture from the already-decoded font tiles and
 * the ROM dialog assets. Shared by Init and the device-reset reload. */
static bool CreateOverlayTextures(const uint8_t *rom_data, size_t rom_size) {
  for (int i = 0; i < kTextStyle_Count; i++) {
    s_artwork.fonts[i] = CreateFontAtlas((TextStyle)i);
    if (!ArRenderTexture_IsValid(s_artwork.fonts[i])) {
      DestroyFontTextures();
      return false;
    }
  }
  s_artwork.debug_font = CreateDebugFontAtlas();
  if (!ArRenderTexture_IsValid(s_artwork.debug_font)) {
    DestroyFontTextures();
    return false;
  }
  /* Host-authored section icons, independent of which text font loaded. */
  s_artwork.icons = CreateIconAtlas();
  if (!ArRenderTexture_IsValid(s_artwork.icons)) {
    DestroyFontTextures();
    return false;
  }
  s_artwork.region_badges = CreateRegionBadgeAtlas();
  if (!ArRenderTexture_IsValid(s_artwork.region_badges)) {
    ArRenderDevice_DestroyTexture(s_device, s_artwork.icons);
    s_artwork.icons = ArRenderTexture_Invalid();
    DestroyFontTextures();
    return false;
  }
  s_artwork.dialog_frame = CreateDialogFrameTexture(rom_data, rom_size);
  return true;
}

const SettingsOverlayArtwork *SettingsOverlayArtwork_Get(void) { return &s_artwork; }

bool SettingsOverlayArtwork_Init(ArRenderDevice *device, const uint8_t *rom, size_t size) {
  s_device = device;
  if (DecodeFontAsset(rom, size))
    PrepareRomFont();
  else
    BuildFallbackFont();
  return CreateOverlayTextures(rom, size);
}

void SettingsOverlayArtwork_Destroy(void) {
  DestroyFontTextures();
  ArRenderDevice_DestroyTexture(s_device, s_artwork.icons);
  s_artwork.icons = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(s_device, s_artwork.region_badges);
  s_artwork.region_badges = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(s_device, s_artwork.dialog_frame);
  s_artwork.dialog_frame = ArRenderTexture_Invalid();
  s_device = NULL;
}

bool SettingsOverlayArtwork_Reload(const uint8_t *rom, size_t size) {
  ArRenderDevice *device = s_device;
  SettingsOverlayArtwork_Destroy();
  s_device = device;
  return CreateOverlayTextures(rom, size);
}
