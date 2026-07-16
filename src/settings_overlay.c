#include "settings_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

enum {
  kFontTileBytes = 0x1000,
  kFontAssetOffset = 0xBECFB,
  kFontAtlasWidth = 128,
  kFontAtlasHeight = 128,
  kDialogCharAssetOffset = 0x6C000,
  kDialogPaletteAssetOffset = 0xE3F73,
  kDialogAtlasWidth = 24,
  kDialogAtlasHeight = 24,
  kGlyphSize = 8,
  kRowHeight = 14,
  kMinimumLayoutWidth = 464,
  kMinimumLayoutHeight = 208,
  kMinimumScalePercent = 25,
  kMaximumScalePercent = 800,
};

#define ARGB(a, r, g, b) \
  ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
   (uint32_t)(g) << 8 | (uint32_t)(b))

static const uint32_t kPanel = ARGB(255, 0, 0, 0);
static const uint32_t kFrameDark = ARGB(255, 45, 63, 78);
static const uint32_t kFrameLight = ARGB(255, 164, 196, 219);
static const uint32_t kHighlight = ARGB(255, 22, 57, 83);

typedef enum TextStyle {
  kText_Normal,
  kText_Dim,
  kText_Warning,
  kTextStyle_Count,
} TextStyle;

/* Pixel zero is transparent. The remaining entries are the original
 * dialog-font outline, blue shadow, and face colors, plus host-side dim and
 * warning remaps of those same three pixel classes. */
static const uint32_t kTextPalettes[kTextStyle_Count][4] = {
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 156, 205, 255), ARGB(255, 255, 255, 255) },
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 45, 63, 78), ARGB(255, 91, 111, 126) },
  { ARGB(0, 0, 0, 0), ARGB(255, 38, 21, 3),
    ARGB(255, 164, 98, 20), ARGB(255, 255, 200, 90) },
};

/* Compact 5x7 fallback and supplemental punctuation. The ROM font is the
 * normal path, but these host-authored masks keep the menu usable if the
 * supplied ROM does not match the verified asset layout. */
static const uint8_t kFallbackFont[128][7] = {
  [' '] = {0, 0, 0, 0, 0, 0, 0},
  ['!'] = {4, 4, 4, 4, 4, 0, 4},
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
};

static const SettingCategory kCategoryOrder[] = {
  kSettingCat_Display,
  kSettingCat_Aspect,
  kSettingCat_Audio,
  kSettingCat_Widescreen,
  kSettingCat_Cheats,
  kSettingCat_Qol,
};

typedef struct BitReader {
  const uint8_t *data;
  size_t size;
  size_t bit;
} BitReader;

typedef struct MenuLayout {
  int output_width;
  int output_height;
  int scale_percent;
  int logical_width;
  int logical_height;
  int origin_x;
  int origin_y;
} MenuLayout;

static SDL_Renderer *s_renderer;
static SDL_Texture *s_font_textures[kTextStyle_Count];
static SDL_Texture *s_dialog_frame_texture;
static uint8_t s_font_tiles[kFontTileBytes];
static bool s_glyph_defined[256];
static bool s_open;
static int s_category_slot;
static int s_row;
static int s_top_row;
static int s_visible_rows = 9;
static int s_auto_menu_scale_percent = 100;
static int s_match_game_scale_percent = 100;
static char s_status[48];
static uint32_t s_status_until;

static bool ReadBits(BitReader *reader, int count, unsigned *value) {
  if (!reader || !value || count < 0 ||
      reader->bit + (size_t)count > reader->size * 8)
    return false;
  unsigned result = 0;
  for (int i = 0; i < count; i++) {
    uint8_t byte = reader->data[reader->bit >> 3];
    result = (result << 1) |
             ((byte >> (7 - (reader->bit & 7))) & 1);
    reader->bit++;
  }
  *value = result;
  return true;
}

/* Host equivalent of the game's $02:C5C9 decompressor. Its tokens are packed
 * continuously across byte boundaries: one flag bit followed by either an
 * eight-bit literal, or an eight-bit dictionary offset and four-bit length.
 * The latter copies length+2 bytes from a 256-byte ring initialized to spaces. */
static bool DecodeFontAsset(const uint8_t *rom_data, size_t rom_size) {
  if (!rom_data || rom_size < (size_t)kFontAssetOffset + 2)
    return false;
  const uint8_t *asset = rom_data + kFontAssetOffset;
  size_t output_size = (size_t)asset[0] | ((size_t)asset[1] << 8);
  if (output_size != kFontTileBytes)
    return false;

  BitReader reader = {
    asset + 2,
    rom_size - (size_t)kFontAssetOffset - 2,
    0,
  };
  uint8_t dictionary[256];
  memset(dictionary, 0x20, sizeof(dictionary));
  unsigned write_position = 0xef;
  size_t output_position = 0;

  while (output_position < output_size) {
    unsigned literal = 0;
    if (!ReadBits(&reader, 1, &literal))
      return false;
    if (literal) {
      unsigned value = 0;
      if (!ReadBits(&reader, 8, &value))
        return false;
      s_font_tiles[output_position++] = (uint8_t)value;
      dictionary[write_position] = (uint8_t)value;
      write_position = (write_position + 1) & 0xff;
      continue;
    }

    unsigned read_position = 0;
    unsigned length_code = 0;
    if (!ReadBits(&reader, 8, &read_position) ||
        !ReadBits(&reader, 4, &length_code))
      return false;
    unsigned length = length_code + 2;
    for (unsigned i = 0; i < length && output_position < output_size; i++) {
      uint8_t value = dictionary[read_position];
      read_position = (read_position + 1) & 0xff;
      s_font_tiles[output_position++] = value;
      dictionary[write_position] = value;
      write_position = (write_position + 1) & 0xff;
    }
  }

  fprintf(stderr,
          "[settings-menu] decoded ActRaiser font: $%04X bytes from ROM "
          "offset $%06X (%zu compressed bytes consumed)\n",
          kFontTileBytes, kFontAssetOffset, (reader.bit + 7) / 8);
  return true;
}

static void SetTilePixel(unsigned tile, int x, int y, unsigned value) {
  if (tile >= 256 || x < 0 || x >= 8 || y < 0 || y >= 8)
    return;
  size_t offset = (size_t)tile * 16 + (size_t)y * 2;
  uint8_t mask = (uint8_t)(1u << (7 - x));
  if (value & 1) s_font_tiles[offset] |= mask;
  else s_font_tiles[offset] &= (uint8_t)~mask;
  if (value & 2) s_font_tiles[offset + 1] |= mask;
  else s_font_tiles[offset + 1] &= (uint8_t)~mask;
}

static bool FallbackGlyphDefined(unsigned ch) {
  if (ch == ' ') return true;
  if (ch >= 128) return false;
  for (int row = 0; row < 7; row++)
    if (kFallbackFont[ch][row]) return true;
  return false;
}

static void WriteFallbackGlyph(unsigned tile, unsigned source_ch) {
  if (tile >= 256 || source_ch >= 128)
    return;
  memset(s_font_tiles + tile * 16, 0, 16);
  for (int row = 0; row < 7; row++) {
    uint8_t bits = kFallbackFont[source_ch][row];
    for (int col = 0; col < 5; col++) {
      if (!(bits & (1u << (4 - col)))) continue;
      SetTilePixel(tile, col + 2, row + 1, 2);
    }
  }
  for (int row = 0; row < 7; row++) {
    uint8_t bits = kFallbackFont[source_ch][row];
    for (int col = 0; col < 5; col++) {
      if (bits & (1u << (4 - col)))
        SetTilePixel(tile, col + 1, row, 3);
    }
  }
}

static void BuildFallbackFont(void) {
  memset(s_font_tiles, 0, sizeof(s_font_tiles));
  memset(s_glyph_defined, 0, sizeof(s_glyph_defined));
  for (unsigned ch = 0; ch < 128; ch++) {
    unsigned source_ch = ch;
    if (ch >= 'a' && ch <= 'z') source_ch = ch - 'a' + 'A';
    if (!FallbackGlyphDefined(source_ch)) continue;
    WriteFallbackGlyph(ch, source_ch);
    s_glyph_defined[ch] = true;
  }
  s_glyph_defined[' '] = true;
  fprintf(stderr,
          "[settings-menu] ROM font unavailable; using host fallback font\n");
}

static void PrepareRomFont(void) {
  memset(s_glyph_defined, 0, sizeof(s_glyph_defined));
  for (unsigned ch = 0x20; ch < 0x80; ch++)
    s_glyph_defined[ch] = true;
  s_glyph_defined['@'] = false;

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

static SDL_Texture *CreateFontAtlas(TextStyle style) {
  uint32_t *pixels = (uint32_t *)calloc(
      (size_t)kFontAtlasWidth * kFontAtlasHeight, sizeof(uint32_t));
  if (!pixels) return NULL;

  for (unsigned tile = 0; tile < 256; tile++) {
    int tile_x = (int)(tile & 15) * kGlyphSize;
    int tile_y = (int)(tile >> 4) * kGlyphSize;
    for (int y = 0; y < kGlyphSize; y++) {
      for (int x = 0; x < kGlyphSize; x++) {
        unsigned pixel = FontPixel(tile, x, y);
        pixels[(tile_y + y) * kFontAtlasWidth + tile_x + x] =
            kTextPalettes[style][pixel];
      }
    }
  }

  SDL_Texture *texture = SDL_CreateTexture(
      s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
      kFontAtlasWidth, kFontAtlasHeight);
  if (texture &&
      SDL_UpdateTexture(texture, NULL, pixels,
                        kFontAtlasWidth * (int)sizeof(uint32_t)) != 0) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  free(pixels);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
#endif
  }
  return texture;
}

static void DestroyFontTextures(void) {
  for (int i = 0; i < kTextStyle_Count; i++) {
    SDL_DestroyTexture(s_font_textures[i]);
    s_font_textures[i] = NULL;
  }
}

static uint16_t ReadLe16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t DialogColor(const uint8_t *palette, unsigned index) {
  if (index == 0) return ARGB(0, 0, 0, 0);
  uint16_t color = ReadLe16(palette + index * 2);
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

static void DecodeDialogAtlasTile(uint32_t *pixels, int atlas_column,
                                  int atlas_row, const uint8_t *characters,
                                  const uint8_t *palette,
                                  unsigned tile_index, bool vertical_flip) {
  const uint8_t *tile = characters + tile_index * 32;
  int destination_x = atlas_column * kGlyphSize;
  int destination_y = atlas_row * kGlyphSize;
  for (int y = 0; y < kGlyphSize; y++) {
    int source_y = vertical_flip ? kGlyphSize - 1 - y : y;
    for (int x = 0; x < kGlyphSize; x++) {
      unsigned pixel = DialogTilePixel(tile, x, source_y);
      pixels[(destination_y + y) * kDialogAtlasWidth +
             destination_x + x] = DialogColor(palette, pixel);
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
static SDL_Texture *CreateDialogFrameTexture(const uint8_t *rom_data,
                                             size_t rom_size) {
  size_t character_end =
      (size_t)kDialogCharAssetOffset + (size_t)(0xff + 1) * 32;
  size_t palette_end = (size_t)kDialogPaletteAssetOffset + 32;
  if (!rom_data || rom_size < character_end || rom_size < palette_end) {
    fprintf(stderr,
            "[settings-menu] native dialog frame unavailable; "
            "using host frame fallback\n");
    return NULL;
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

  SDL_Texture *texture = SDL_CreateTexture(
      s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
      kDialogAtlasWidth, kDialogAtlasHeight);
  if (texture &&
      SDL_UpdateTexture(texture, NULL, pixels,
                        kDialogAtlasWidth * (int)sizeof(uint32_t)) != 0) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
#endif
    fprintf(stderr,
            "[settings-menu] decoded native dialog frame: "
            "chars ROM $%06X, palette ROM $%06X\n",
            kDialogCharAssetOffset, kDialogPaletteAssetOffset);
  }
  return texture;
}

static int CategoryRowCount(SettingCategory category) {
  int count = 0;
  for (int i = 0; i < g_setting_desc_count; i++)
    if (g_setting_descs[i].category == category) count++;
  return count;
}

static void SelectFirstPopulatedCategory(void) {
  const int count = (int)(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));
  if (s_category_slot >= 0 && s_category_slot < count &&
      CategoryRowCount(kCategoryOrder[s_category_slot]) > 0)
    return;
  for (int i = 0; i < count; i++) {
    if (CategoryRowCount(kCategoryOrder[i]) > 0) {
      s_category_slot = i;
      return;
    }
  }
  s_category_slot = 0;
}

static const SettingDesc *SelectedDesc(void) {
  SelectFirstPopulatedCategory();
  SettingCategory category = kCategoryOrder[s_category_slot];
  int row = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->category != category) continue;
    if (row++ == s_row) return desc;
  }
  return NULL;
}

static void SetStatus(const char *text) {
  snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
  s_status_until = SDL_GetTicks() + 2500;
}

static void SaveAcceptedChange(SettingChangeResult result) {
  if (result <= kSettingChange_Unchanged) {
    SetStatus(result == kSettingChange_Rejected ? "NOT EDITABLE" : "UNCHANGED");
    return;
  }
  if (!Settings_Save("settings.ini")) {
    SetStatus("SAVE FAILED");
    fprintf(stderr, "[settings-menu] could not save settings.ini\n");
    return;
  }
  if (result == kSettingChange_RestartPending)
    SetStatus("RESTART REQUIRED - SAVED");
  else if (result == kSettingChange_AppliedStickyDisable)
    SetStatus("STICKY EFFECTS REMAIN - SAVED");
  else
    SetStatus("APPLIED - SAVED");
}

static void ChangeSelectedValue(int direction) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  long value = 0;
  if (!Settings_GetLong(desc, &value)) {
    SetStatus("EDIT IN SETTINGS.INI");
    return;
  }

  long next = value;
  if (desc->type == kSettingType_Bool) {
    next = !value;
  } else {
    if (value == 0 && desc->field == &g_settings.menu_scale_percent)
      value = s_auto_menu_scale_percent;
    if (value == 0 && desc->field == &g_settings.hud_scale_percent)
      value = s_match_game_scale_percent;
    next = value;
    long step = desc->step > 0 ? desc->step : 1;
    next += direction < 0 ? -step : step;
    if (desc->type == kSettingType_Enum) {
      if (next < desc->minval) next = desc->maxval;
      if (next > desc->maxval) next = desc->minval;
    }
  }
  SaveAcceptedChange(Settings_SetLong(desc, next));
}

static void ResetSelectedValue(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  SaveAcceptedChange(Settings_Reset(desc));
}

static void MoveCategory(int direction) {
  const int count = (int)(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));
  for (int attempt = 0; attempt < count; attempt++) {
    s_category_slot = (s_category_slot + direction + count) % count;
    if (CategoryRowCount(kCategoryOrder[s_category_slot]) > 0) break;
  }
  s_row = 0;
  s_top_row = 0;
}

static void EnsureSelectedRowVisible(void) {
  int count = CategoryRowCount(kCategoryOrder[s_category_slot]);
  int visible = s_visible_rows > 0 ? s_visible_rows : 1;
  if (s_row < 0) s_row = 0;
  if (s_row >= count) s_row = count > 0 ? count - 1 : 0;
  if (s_row < s_top_row) s_top_row = s_row;
  if (s_row >= s_top_row + visible)
    s_top_row = s_row - visible + 1;
  int maximum_top = count > visible ? count - visible : 0;
  if (s_top_row > maximum_top) s_top_row = maximum_top;
  if (s_top_row < 0) s_top_row = 0;
}

static void MoveRow(int direction) {
  int count = CategoryRowCount(kCategoryOrder[s_category_slot]);
  if (count <= 0) return;
  s_row = (s_row + direction + count) % count;
  EnsureSelectedRowVisible();
}

bool SettingsOverlay_Init(SDL_Renderer *renderer,
                          const uint8_t *rom_data, size_t rom_size) {
  s_renderer = renderer;
  SelectFirstPopulatedCategory();
  if (!renderer) return true;

  bool rom_font = DecodeFontAsset(rom_data, rom_size);
  if (rom_font) PrepareRomFont();
  else BuildFallbackFont();

  for (int i = 0; i < kTextStyle_Count; i++) {
    s_font_textures[i] = CreateFontAtlas((TextStyle)i);
    if (!s_font_textures[i]) {
      DestroyFontTextures();
      return false;
    }
  }
  s_dialog_frame_texture =
      CreateDialogFrameTexture(rom_data, rom_size);
  return true;
}

void SettingsOverlay_Destroy(void) {
  DestroyFontTextures();
  SDL_DestroyTexture(s_dialog_frame_texture);
  s_dialog_frame_texture = NULL;
  s_renderer = NULL;
  s_open = false;
}

bool SettingsOverlay_IsOpen(void) {
  return s_open;
}

void SettingsOverlay_Open(void) {
  SelectFirstPopulatedCategory();
  s_open = true;
  s_status[0] = 0;
  fprintf(stderr, "[settings-menu] opened\n");
}

void SettingsOverlay_Close(void) {
  if (!s_open) return;
  s_open = false;
  fprintf(stderr, "[settings-menu] closed\n");
}

bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat) {
  if (!s_open) return false;
  if (key == SDLK_F2) return false;
  if (!pressed) return true;

  switch (key) {
    case SDLK_ESCAPE:
    case SDLK_F1:
    case SDLK_z:       /* SNES B */
      if (!repeat) SettingsOverlay_Close();
      break;
    case SDLK_UP:
      MoveRow(-1);
      break;
    case SDLK_DOWN:
      MoveRow(1);
      break;
    case SDLK_LEFT:
      ChangeSelectedValue(-1);
      break;
    case SDLK_RIGHT:
    case SDLK_x:       /* SNES A */
    case SDLK_RETURN:  /* SNES Start */
      ChangeSelectedValue(1);
      break;
    case SDLK_q:       /* SNES L */
      MoveCategory(-1);
      break;
    case SDLK_w:       /* SNES R */
    case SDLK_TAB:
      MoveCategory(1);
      break;
    case SDLK_a:       /* SNES Y */
      ResetSelectedValue();
      break;
    default:
      break;
  }
  return true;
}

static int ScalePosition(int position, int scale_percent) {
  return (position * scale_percent + 50) / 100;
}

static SDL_Rect LogicalRect(const MenuLayout *layout,
                            int x, int y, int width, int height) {
  int x0 = layout->origin_x + ScalePosition(x, layout->scale_percent);
  int y0 = layout->origin_y + ScalePosition(y, layout->scale_percent);
  int x1 = layout->origin_x +
           ScalePosition(x + width, layout->scale_percent);
  int y1 = layout->origin_y +
           ScalePosition(y + height, layout->scale_percent);
  return (SDL_Rect){ x0, y0, x1 - x0, y1 - y0 };
}

static void SetDrawColor(uint32_t color) {
  SDL_SetRenderDrawColor(s_renderer,
      (Uint8)(color >> 16), (Uint8)(color >> 8),
      (Uint8)color, (Uint8)(color >> 24));
}

static void FillPixelRect(int x, int y, int width, int height,
                          uint32_t color) {
  if (width <= 0 || height <= 0) return;
  SDL_Rect rect = { x, y, width, height };
  SetDrawColor(color);
  SDL_RenderFillRect(s_renderer, &rect);
}

static void FillLogicalRect(const MenuLayout *layout,
                            int x, int y, int width, int height,
                            uint32_t color) {
  SDL_Rect rect = LogicalRect(layout, x, y, width, height);
  FillPixelRect(rect.x, rect.y, rect.w, rect.h, color);
}

static void DrawDialogTile(const MenuLayout *layout, int atlas_column,
                           int atlas_row, int x, int y) {
  SDL_Rect source = {
    atlas_column * kGlyphSize,
    atlas_row * kGlyphSize,
    kGlyphSize,
    kGlyphSize,
  };
  SDL_Rect destination =
      LogicalRect(layout, x, y, kGlyphSize, kGlyphSize);
  SDL_RenderCopy(s_renderer, s_dialog_frame_texture,
                 &source, &destination);
}

static void DrawDialogPanel(const MenuLayout *layout,
                            int x, int y, int width, int height) {
  if (width < 16 || height < 16) return;
  if (!s_dialog_frame_texture) {
    FillLogicalRect(layout, x, y, width, height, kFrameDark);
    FillLogicalRect(layout, x + 2, y + 2,
                    width - 4, height - 4, kFrameLight);
    FillLogicalRect(layout, x + 4, y + 4,
                    width - 8, height - 8, kPanel);
    return;
  }

  FillLogicalRect(layout, x + kGlyphSize, y + kGlyphSize,
                  width - kGlyphSize * 2, height - kGlyphSize * 2,
                  kPanel);
  for (int tile_x = x + kGlyphSize;
       tile_x < x + width - kGlyphSize; tile_x += kGlyphSize) {
    DrawDialogTile(layout, 1, 0, tile_x, y);
    DrawDialogTile(layout, 1, 2, tile_x,
                   y + height - kGlyphSize);
  }
  for (int tile_y = y + kGlyphSize;
       tile_y < y + height - kGlyphSize; tile_y += kGlyphSize) {
    DrawDialogTile(layout, 0, 1, x, tile_y);
    DrawDialogTile(layout, 2, 1,
                   x + width - kGlyphSize, tile_y);
  }
  DrawDialogTile(layout, 0, 0, x, y);
  DrawDialogTile(layout, 2, 0,
                 x + width - kGlyphSize, y);
  DrawDialogTile(layout, 0, 2,
                 x, y + height - kGlyphSize);
  DrawDialogTile(layout, 2, 2,
                 x + width - kGlyphSize,
                 y + height - kGlyphSize);
}

static void DrawGlyph(const MenuLayout *layout, int x, int y,
                      unsigned char ch, TextStyle style) {
  if (ch == ' ') return;
  if (!s_glyph_defined[ch]) ch = '?';
  if (!s_glyph_defined[ch]) return;
  SDL_Texture *texture = s_font_textures[style];
  if (!texture) return;
  SDL_Rect source = {
    (ch & 15) * kGlyphSize,
    (ch >> 4) * kGlyphSize,
    kGlyphSize,
    kGlyphSize,
  };
  SDL_Rect destination = LogicalRect(
      layout, x, y, kGlyphSize, kGlyphSize);
  SDL_RenderCopy(s_renderer, texture, &source, &destination);
}

static void DrawTextN(const MenuLayout *layout, int x, int y,
                      const char *text, int max_chars, TextStyle style) {
  if (!text || max_chars <= 0) return;
  for (int i = 0; text[i] && i < max_chars; i++)
    DrawGlyph(layout, x + i * kGlyphSize, y,
              (unsigned char)text[i], style);
}

static void DrawText(const MenuLayout *layout, int x, int y,
                     const char *text, TextStyle style) {
  DrawTextN(layout, x, y, text, 256, style);
}

static int CappedTextLength(const char *text, int max_chars) {
  if (!text || max_chars <= 0) return 0;
  int length = (int)strlen(text);
  return length < max_chars ? length : max_chars;
}

static void DrawTextRight(const MenuLayout *layout, int right, int y,
                          const char *text, int max_chars, TextStyle style) {
  int length = CappedTextLength(text, max_chars);
  DrawTextN(layout, right - length * kGlyphSize, y,
            text, length, style);
}

static void DrawWrappedText(const MenuLayout *layout, int x, int y,
                            const char *text, int max_chars,
                            int max_lines, TextStyle style) {
  const char *cursor = text;
  if (max_chars > 63) max_chars = 63;
  for (int line = 0; line < max_lines && cursor && *cursor; line++) {
    int length = (int)strlen(cursor);
    if (length > max_chars) {
      length = max_chars;
      while (length > 1 && cursor[length] != ' ') length--;
      if (length <= 1) length = max_chars;
    }
    char buffer[64];
    memcpy(buffer, cursor, (size_t)length);
    buffer[length] = 0;
    DrawText(layout, x, y + line * 10, buffer, style);
    cursor += length;
    while (*cursor == ' ') cursor++;
  }
}

static int SnappedFitScale(int output_width, int output_height) {
  int fit_x = output_width * 100 / kMinimumLayoutWidth;
  int fit_y = output_height * 100 / kMinimumLayoutHeight;
  int fit = fit_x < fit_y ? fit_x : fit_y;
  fit = (fit / 25) * 25;
  if (fit < kMinimumScalePercent) fit = kMinimumScalePercent;
  if (fit > kMaximumScalePercent) fit = kMaximumScalePercent;
  return fit;
}

static MenuLayout BuildLayout(int output_width, int output_height) {
  int fit_scale = SnappedFitScale(output_width, output_height);
  s_auto_menu_scale_percent = fit_scale;
  int scale = g_settings.menu_scale_percent > 0
      ? g_settings.menu_scale_percent : fit_scale;
  if (scale > fit_scale) scale = fit_scale;
  if (scale < kMinimumScalePercent) scale = kMinimumScalePercent;

  int logical_width = output_width * 100 / scale;
  int logical_height = output_height * 100 / scale;
  int used_width = ScalePosition(logical_width, scale);
  int used_height = ScalePosition(logical_height, scale);
  return (MenuLayout){
    output_width,
    output_height,
    scale,
    logical_width,
    logical_height,
    (output_width - used_width) / 2,
    (output_height - used_height) / 2,
  };
}

static int SnapPanelEdge(int origin, int edge) {
  return origin + ((edge - origin) / kGlyphSize) * kGlyphSize;
}

static void DrawMenu(const MenuLayout *layout) {
  const int margin = 8;
  const int gap = 8;
  const int left_width = 144;
  const int bottom_height = 64;
  const int panel_right =
      SnapPanelEdge(margin, layout->logical_width - margin);
  const int panel_bottom =
      SnapPanelEdge(margin, layout->logical_height - margin);
  const int bottom_y = panel_bottom - bottom_height;
  const int top_y = margin;
  const int top_height = bottom_y - gap - top_y;
  const int left_x = margin;
  const int right_x = left_x + left_width + gap;
  const int right_width = panel_right - right_x;
  const int bottom_width = panel_right - margin;

  DrawDialogPanel(layout, left_x, top_y, left_width, top_height);
  DrawDialogPanel(layout, right_x, top_y, right_width, top_height);
  DrawDialogPanel(layout, margin, bottom_y,
                  bottom_width, bottom_height);

  const int left_text_x = left_x + 12;
  const int left_title_y = top_y + 12;
  const int category_first_y = top_y + 30;
  const int right_text_x = right_x + 12;
  const int right_title_y = top_y + 12;
  const int first_row_y = top_y + 30;
  const int selector_x = right_x + 12;
  const int label_x = right_x + 22;
  const int value_right = right_x + right_width - 12;
  const int value_chars = 12;
  const int value_left = value_right - value_chars * kGlyphSize;
  const int restart_x = value_left - 12;
  int label_chars = (restart_x - label_x - 4) / kGlyphSize;
  if (label_chars < 1) label_chars = 1;

  DrawText(layout, left_text_x, left_title_y,
           "SYSTEM SETTINGS", kText_Normal);
  if (s_status[0] && SDL_TICKS_PASSED(SDL_GetTicks(), s_status_until))
    s_status[0] = 0;
  SelectFirstPopulatedCategory();
  SettingCategory category = kCategoryOrder[s_category_slot];
  const char *category_name = Settings_CategoryName(category);
  DrawTextN(layout, right_text_x, right_title_y,
            category_name, 15, kText_Normal);
  if (s_status[0]) {
    int category_length = CappedTextLength(category_name, 15);
    int status_chars =
        (right_width - 24) / kGlyphSize - category_length - 2;
    if (status_chars > 24) status_chars = 24;
    if (status_chars > 0)
      DrawTextRight(layout, value_right, right_title_y,
                    s_status, status_chars, kText_Warning);
  }

  int category_y = category_first_y;
  const int category_count =
      (int)(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));
  for (int slot = 0; slot < category_count; slot++) {
    SettingCategory listed_category = kCategoryOrder[slot];
    if (CategoryRowCount(listed_category) <= 0) continue;
    TextStyle style = slot == s_category_slot ? kText_Normal : kText_Dim;
    if (slot == s_category_slot)
      DrawGlyph(layout, left_text_x + ((SDL_GetTicks() / 300) & 1),
                category_y, '>', kText_Warning);
    DrawTextN(layout, left_text_x + 10, category_y,
              Settings_CategoryName(listed_category), 14, style);
    category_y += kRowHeight;
  }

  s_visible_rows =
      (top_y + top_height - 12 - first_row_y) / kRowHeight + 1;
  if (s_visible_rows < 1) s_visible_rows = 1;
  EnsureSelectedRowVisible();

  int category_row = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->category != category) continue;
    int row = category_row++;
    if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
    int y = first_row_y + (row - s_top_row) * kRowHeight;
    bool selected = row == s_row;
    bool available = Settings_IsAvailable(desc);
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2,
                      right_width - 18,
                      11, kHighlight);
      DrawGlyph(layout, selector_x + ((SDL_GetTicks() / 250) & 1),
                y, '>', kText_Warning);
    }
    TextStyle style = available ? kText_Normal : kText_Dim;
    DrawTextN(layout, label_x, y, desc->label, label_chars, style);

    char value[512];
    Settings_FormatValue(desc, value, sizeof(value));
    if (!value[0]) snprintf(value, sizeof(value), "CUSTOM");
    if (desc->field == &g_settings.display_mode) {
      static const char *const short_modes[] = {
        "4:3 AUTH", "WIDE RAW", "WIDE FULL", "CUSTOM"
      };
      int mode = g_settings.display_mode;
      if (mode >= 0 &&
          mode < (int)(sizeof(short_modes) / sizeof(short_modes[0])))
        snprintf(value, sizeof(value), "%s", short_modes[mode]);
    }
    DrawTextRight(layout, value_right, y,
                  value, value_chars, style);
    if (desc->apply == kApply_Restart)
      DrawGlyph(layout, restart_x, y, '*', kText_Warning);
  }

  const SettingDesc *selected = SelectedDesc();
  if (selected) {
    int tooltip_chars = (bottom_width - 24) / kGlyphSize;
    DrawWrappedText(layout, margin + 12, bottom_y + 12,
                    selected->tooltip,
                    tooltip_chars, 3,
                    Settings_IsAvailable(selected)
                        ? kText_Normal : kText_Dim);
  }
  DrawText(layout, margin + 12, bottom_y + bottom_height - 18,
           "L/R TAB  D-PAD EDIT  Y RESET  B CLOSE", kText_Dim);
}

void SettingsOverlay_Render(SDL_Rect game_viewport) {
  if (!s_open || !s_renderer || !s_font_textures[kText_Normal]) return;
  int output_width = 0;
  int output_height = 0;
  if (SDL_GetRendererOutputSize(
          s_renderer, &output_width, &output_height) != 0 ||
      output_width <= 0 || output_height <= 0)
    return;

  s_match_game_scale_percent =
      ((game_viewport.h * 100 + 112) / 224 + 12) / 25 * 25;
  if (s_match_game_scale_percent < 25) s_match_game_scale_percent = 25;
  if (s_match_game_scale_percent > 400) s_match_game_scale_percent = 400;

  SDL_BlendMode old_blend_mode = SDL_BLENDMODE_NONE;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  SDL_GetRenderDrawBlendMode(s_renderer, &old_blend_mode);
  SDL_GetRenderDrawColor(s_renderer, &old_r, &old_g, &old_b, &old_a);
  SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

  MenuLayout layout = BuildLayout(output_width, output_height);
  DrawMenu(&layout);

  SDL_SetRenderDrawBlendMode(s_renderer, old_blend_mode);
  SDL_SetRenderDrawColor(s_renderer, old_r, old_g, old_b, old_a);
}
