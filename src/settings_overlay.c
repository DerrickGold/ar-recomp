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
  kDebugFontAtlasWidth = 96,
  kDebugFontAtlasHeight = 128,
  kDebugGlyphWidth = 6,
  kDebugGlyphHeight = 8,
  kDebugLineHeight = 10,
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
  kNavRowHeight = 10,
};

#define ARGB(a, r, g, b) \
  ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
   (uint32_t)(g) << 8 | (uint32_t)(b))

static const uint32_t kPanel = ARGB(255, 0, 0, 0);
static const uint32_t kFrameDark = ARGB(255, 45, 63, 78);
static const uint32_t kFrameLight = ARGB(255, 164, 196, 219);
static const uint32_t kHighlight = ARGB(255, 22, 57, 83);

typedef enum DebugTextStyle {
  kDebugText_Normal,
  kDebugText_Label,
  kDebugText_Value,
  kDebugText_Target,
  kDebugText_Warning,
  kDebugText_Dim,
  kDebugTextStyle_Count,
} DebugTextStyle;

/* The scene inspector is a host debugger, not an in-game dialog. Keep its
 * information hierarchy legible independently of the ROM font palette. */
static const SDL_Color kDebugTextColors[kDebugTextStyle_Count] = {
  { 218, 229, 238, 255 }, /* ordinary punctuation/text */
  {  92, 196, 255, 255 }, /* field names */
  { 255, 207,  92, 255 }, /* addresses and numeric values */
  { 105, 232, 157, 255 }, /* BG/OBJ targets and source policies */
  { 255, 145,  76, 255 }, /* missing candidates and warnings */
  { 119, 139, 154, 255 }, /* controls and explanatory notes */
};

typedef enum TextStyle {
  kText_Normal,
  kText_Dim,
  kText_Warning,
  kText_Value,
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
  /* M2 (followup doc): row values get a cool-cyan face so they read
   * distinct from warm-white labels. Reuses the resize-grip cyan already
   * in-tree (ARGB(255,92,196,255), ~line 1610) for palette harmony. */
  { ARGB(0, 0, 0, 0), ARGB(255, 0, 0, 0),
    ARGB(255, 40, 120, 140), ARGB(255, 92, 196, 255) },
};

/* Compact 5x7 fallback and supplemental punctuation. The ROM font is the
 * normal path, but these host-authored masks keep the menu usable if the
 * supplied ROM does not match the verified asset layout. */
static const uint8_t kFallbackFont[128][7] = {
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
};

/* M1(a) (followup doc): the three graphics-related categories (Display,
 * Diorama i.e. kSettingCat_Presentation, Graphics) sit adjacent to
 * Widescreen — the granular ws_* flags that Display's display_mode row
 * presets — instead of Audio wedging between them. */
static const SettingCategory kCategoryOrder[] = {
  kSettingCat_Display,
  kSettingCat_Presentation,
  kSettingCat_Simulation,
  kSettingCat_Graphics,
  kSettingCat_Widescreen,
  kSettingCat_Audio,
  kSettingCat_Cheats,
  kSettingCat_Save,
  kSettingCat_Extras,
  kSettingCat_Inspector,
};

typedef struct TopLevelItem {
  const char *key;
  const char *nav_label;
} TopLevelItem;

/* Destructive host lifecycle commands stay direct primary-navigation leaves. */
static const TopLevelItem kTopLevelItems[] = {
  { "restart_game", "Restart game" },
  { "exit_desktop", "Exit desktop" },
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
static SDL_Texture *s_debug_font_texture;
static SDL_Texture *s_dialog_frame_texture;
static uint8_t s_font_tiles[kFontTileBytes];
static bool s_glyph_defined[256];
static bool s_open;
static bool s_submenu_open;
static int s_category_slot;
/* The primary-navigation list has its own scroll window.  The right-hand
 * submenu already scrolls through s_top_row/s_visible_rows; sharing those
 * values would make moving either pane unexpectedly reposition the other. */
static int s_nav_top_row;
static int s_nav_visible_rows = 9;
static int s_row;
static int s_top_row;
static int s_visible_rows = 9;
static int s_auto_menu_scale_percent = 100;
static int s_match_game_scale_percent = 100;
static char s_status[48];
static Uint64 s_status_until;
static bool s_editing;
static char s_edit_buffer[512];
static SDL_Rect s_debug_panel_rect;
static SDL_Rect s_debug_panel_drag_rect;
static SDL_Rect s_debug_panel_resize_rect;
static bool s_debug_panel_visible;
static bool s_debug_panel_dragging;
static bool s_debug_panel_resizing;
static bool s_debug_panel_user_position;
static int s_debug_panel_scale_percent;
static int s_debug_panel_render_scale_percent;
static int s_debug_panel_output_x;
static int s_debug_panel_output_y;
static int s_debug_panel_drag_offset_x;
static int s_debug_panel_drag_offset_y;
static int s_debug_panel_resize_start_x;
static int s_debug_panel_resize_start_y;
static int s_debug_panel_resize_start_width;
static int s_debug_panel_resize_start_height;
static int s_debug_panel_resize_start_scale;
static SettingsOverlayInspectorInfoProvider s_inspector_info_provider;

void SettingsOverlay_SetInspectorInfoProvider(
    SettingsOverlayInspectorInfoProvider provider) {
  s_inspector_info_provider = provider;
}

/* SDL3 render primitives take float rects. Layout math stays integer (it also
 * feeds the public panel-rect API), so convert only at the draw call. */
static SDL_FRect ToFRect(SDL_Rect r) {
  return (SDL_FRect){ (float)r.x, (float)r.y, (float)r.w, (float)r.h };
}

/* SDL3 SDL_StartTextInput/SDL_StopTextInput require the target window. The
 * overlay only holds a renderer; recover the window from it. A headless
 * software renderer (unit tests) has no window, so these become no-ops. */
static SDL_Window *OverlayWindow(void) {
  return s_renderer ? SDL_GetRenderWindow(s_renderer) : NULL;
}

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

/* ── M3 (followup doc): category nav icons ──────────────────────────────
 * Tiles 128-255 are never touched by the ROM font (0x20-0x7F) or the
 * fallback font (0-127, see BuildFallbackFont) — free real estate for a
 * handful of host-authored 8x8 icon glyphs, reusing the exact same
 * two-bitplane tile storage + DrawGlyph blit path as every other glyph, so
 * they automatically pick up the kText_Normal/kText_Dim/kText_Value/
 * kText_Warning tinting already baked per style (CreateFontAtlas). Unlike
 * WriteFallbackGlyph's 5x7-in-8x8 letterforms, icons use the full 8x8
 * canvas: '#' marks a face pixel, an outline pixel is auto-added on any
 * blank cell 4-adjacent to a face pixel (same outline-behind-face visual
 * language as the text glyphs, just derived instead of hand-placed). */
static void WriteIconGlyph(unsigned tile, const char rows[8][9]) {
  if (tile >= 256) return;
  memset(s_font_tiles + tile * 16, 0, 16);
  bool face[8][8];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      face[y][x] = rows[y][x] == '#';
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      if (face[y][x]) continue;
      bool touches = (x > 0 && face[y][x - 1]) ||
                     (x < 7 && face[y][x + 1]) ||
                     (y > 0 && face[y - 1][x]) ||
                     (y < 7 && face[y + 1][x]);
      if (touches) SetTilePixel(tile, x, y, 2);
    }
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      if (face[y][x]) SetTilePixel(tile, x, y, 3);
}

enum {
  kIconTile_Display = 128,
  kIconTile_Diorama,
  kIconTile_Simulation,
  kIconTile_Graphics,
  kIconTile_Widescreen,
  kIconTile_Audio,
  kIconTile_Cheats,
  kIconTile_Save,
  kIconTile_Extras,
  kIconTile_Inspector,
  kIconTile_Count_ = kIconTile_Inspector + 1,
};

static void WriteHostIcons(void) {
  WriteIconGlyph(kIconTile_Display, (const char[8][9]){
    "..####..", ".######.", ".#....#.", ".#....#.",
    ".######.", "...##...", "..####..", "........",
  });
  /* Diorama: a tilted plane (diamond outline) for the 3D camera/perspective
   * mode. A stacked-bars "receding planes" design was tried first, but the
   * outline dilation (see WriteIconGlyph) bridges the gap between any two
   * wide bars only 1 row apart — it rendered as a single solid funnel
   * instead of separate bars. The diamond's outline never gets that close
   * to itself, so it stays hollow. */
  WriteIconGlyph(kIconTile_Diorama, (const char[8][9]){
    "...##...", "..#..#..", ".#....#.", "#......#",
    ".#....#.", "..#..#..", "...##...", "........",
  });
  /* Simulation: a horizon over a receding ground grid. */
  WriteIconGlyph(kIconTile_Simulation, (const char[8][9]){
    "........", "........", "########", "...##...",
    "..#..#..", ".#....#.", "#......#", "........",
  });
  /* Graphics: a four-point sparkle for the GPU shader effects. Each ray is
   * ≥2 cells from its neighbors so the outline dilation can't bridge them
   * into a bowtie (the earlier diamond+cross design touched at the
   * center and read as one blob). */
  WriteIconGlyph(kIconTile_Graphics, (const char[8][9]){
    "...##...", "...##...", "........", "##....##",
    "##....##", "........", "...##...", "...##...",
  });
  /* Widescreen: arrows pointing outward, aspect stretching left/right. */
  WriteIconGlyph(kIconTile_Widescreen, (const char[8][9]){
    "........", "#.....#.", "##...##.", "#.#.#.#.",
    "#.#.#.#.", "##...##.", "#.....#.", "........",
  });
  /* Audio: speaker cone plus two sound-wave dots, distinct from Graphics'
   * symmetric sparkle. */
  WriteIconGlyph(kIconTile_Audio, (const char[8][9]){
    "..#.....", ".##....#", ".####..#", "######.#",
    ".####..#", ".##....#", "..#.....", "........",
  });
  WriteIconGlyph(kIconTile_Cheats, (const char[8][9]){
    "...#....", "..###...", ".#####..", "########",
    "..###...", ".#.#.#..", "#..#..#.", "........",
  });
  WriteIconGlyph(kIconTile_Save, (const char[8][9]){
    "########", "#......#", "#.####.#", "#.#..#.#",
    "#.####.#", "#......#", "#......#", "########",
  });
  WriteIconGlyph(kIconTile_Extras, (const char[8][9]){
    "........", "...##...", "...##...", ".######.",
    ".######.", "...##...", "...##...", "........",
  });
  WriteIconGlyph(kIconTile_Inspector, (const char[8][9]){
    ".####...", "#....#..", "#....#..", "#....#..",
    ".####...", "...##...", "....##..", ".....##.",
  });
  for (unsigned t = kIconTile_Display; t < kIconTile_Count_; t++)
    s_glyph_defined[t] = true;
}

/* Which icon (if any) represents a settings category in the nav column. */
static int CategoryIconTile(SettingCategory category) {
  switch (category) {
    case kSettingCat_Display:      return kIconTile_Display;
    case kSettingCat_Presentation: return kIconTile_Diorama;
    case kSettingCat_Simulation:   return kIconTile_Simulation;
    case kSettingCat_Graphics:     return kIconTile_Graphics;
    case kSettingCat_Widescreen:   return kIconTile_Widescreen;
    case kSettingCat_Audio:        return kIconTile_Audio;
    case kSettingCat_Cheats:       return kIconTile_Cheats;
    case kSettingCat_Save:         return kIconTile_Save;
    case kSettingCat_Extras:       return kIconTile_Extras;
    case kSettingCat_Inspector:    return kIconTile_Inspector;
  }
  return -1;
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
      !SDL_UpdateTexture(texture, NULL, pixels,
                         kFontAtlasWidth * (int)sizeof(uint32_t))) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  free(pixels);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    /* SDL3 textures default to linear filtering; the pixel-art atlases must
     * sample nearest so glyph edges stay crisp at every scale. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  }
  return texture;
}

static SDL_Texture *CreateDebugFontAtlas(void) {
  uint32_t *pixels = (uint32_t *)calloc(
      (size_t)kDebugFontAtlasWidth * kDebugFontAtlasHeight,
      sizeof(uint32_t));
  if (!pixels) return NULL;

  for (unsigned ch = 0; ch < 256; ch++) {
    unsigned source_ch = ch;
    if (source_ch >= 'a' && source_ch <= 'z')
      source_ch = source_ch - 'a' + 'A';
    if (source_ch >= 128 || !FallbackGlyphDefined(source_ch))
      source_ch = '?';
    int cell_x = (int)(ch & 15) * kDebugGlyphWidth;
    int cell_y = (int)(ch >> 4) * kDebugGlyphHeight;
    for (int row = 0; row < 7; row++) {
      uint8_t bits = kFallbackFont[source_ch][row];
      for (int col = 0; col < 5; col++) {
        if (bits & (1u << (4 - col)))
          pixels[(cell_y + row) * kDebugFontAtlasWidth + cell_x + col] =
              ARGB(255, 255, 255, 255);
      }
    }
  }

  SDL_Texture *texture = SDL_CreateTexture(
      s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
      kDebugFontAtlasWidth, kDebugFontAtlasHeight);
  if (texture &&
      !SDL_UpdateTexture(texture, NULL, pixels,
                         kDebugFontAtlasWidth * (int)sizeof(uint32_t))) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  free(pixels);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    /* SDL3 textures default to linear filtering; the pixel-art atlases must
     * sample nearest so glyph edges stay crisp at every scale. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  }
  return texture;
}

static void DestroyFontTextures(void) {
  for (int i = 0; i < kTextStyle_Count; i++) {
    SDL_DestroyTexture(s_font_textures[i]);
    s_font_textures[i] = NULL;
  }
  SDL_DestroyTexture(s_debug_font_texture);
  s_debug_font_texture = NULL;
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
      !SDL_UpdateTexture(texture, NULL, pixels,
                         kDialogAtlasWidth * (int)sizeof(uint32_t))) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    fprintf(stderr,
            "[settings-menu] decoded native dialog frame: "
            "chars ROM $%06X, palette ROM $%06X\n",
            kDialogCharAssetOffset, kDialogPaletteAssetOffset);
  }
  return texture;
}

static int CategoryRowCount(SettingCategory category) {
  int count = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    bool promoted = false;
    for (int top = 0;
         top < (int)(sizeof(kTopLevelItems) / sizeof(kTopLevelItems[0]));
         top++) {
      if (!strcmp(desc->key, kTopLevelItems[top].key)) {
        promoted = true;
        break;
      }
    }
    if (desc->category == category && !promoted &&
        Settings_IsMenuVisible(desc))
      count++;
  }
  return count;
}

static int CategorySlotCount(void) {
  return (int)(sizeof(kCategoryOrder) / sizeof(kCategoryOrder[0]));
}

static int TopLevelItemCount(void) {
  return (int)(sizeof(kTopLevelItems) / sizeof(kTopLevelItems[0]));
}

static int NavSlotCount(void) {
  return CategorySlotCount() + TopLevelItemCount();
}

static const TopLevelItem *TopLevelItemForSlot(int slot) {
  int index = slot - CategorySlotCount();
  if (index < 0 || index >= TopLevelItemCount()) return NULL;
  return &kTopLevelItems[index];
}

static const SettingDesc *TopLevelDescForSlot(int slot) {
  const TopLevelItem *item = TopLevelItemForSlot(slot);
  return item ? Settings_Find(item->key) : NULL;
}

static bool IsTopLevelDesc(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;
  for (int i = 0; i < TopLevelItemCount(); i++)
    if (!strcmp(desc->key, kTopLevelItems[i].key)) return true;
  return false;
}

static int NavRowCount(int slot) {
  if (slot < 0 || slot >= NavSlotCount()) return 0;
  if (slot < CategorySlotCount())
    return CategoryRowCount(kCategoryOrder[slot]);
  return TopLevelDescForSlot(slot) ? 1 : 0;
}

static void SelectFirstPopulatedCategory(void);

static int NavPopulatedCount(void) {
  int populated = 0;
  for (int slot = 0; slot < NavSlotCount(); slot++)
    if (NavRowCount(slot) > 0) populated++;
  return populated;
}

static int NavOrdinalForSlot(int selected_slot) {
  int ordinal = 0;
  for (int slot = 0; slot < NavSlotCount(); slot++) {
    if (NavRowCount(slot) <= 0) continue;
    if (slot == selected_slot) return ordinal;
    ordinal++;
  }
  return 0;
}

static void EnsureSelectedNavVisible(void) {
  SelectFirstPopulatedCategory();
  int count = NavPopulatedCount();
  int visible = s_nav_visible_rows > 0 ? s_nav_visible_rows : 1;
  int selected = NavOrdinalForSlot(s_category_slot);
  if (selected < s_nav_top_row) s_nav_top_row = selected;
  if (selected >= s_nav_top_row + visible)
    s_nav_top_row = selected - visible + 1;
  int maximum_top = count > visible ? count - visible : 0;
  if (s_nav_top_row > maximum_top) s_nav_top_row = maximum_top;
  if (s_nav_top_row < 0) s_nav_top_row = 0;
}

static const char *NavSlotLabel(int slot) {
  if (slot < 0 || slot >= NavSlotCount()) return "";
  if (slot < CategorySlotCount())
    return Settings_CategoryName(kCategoryOrder[slot]);
  const TopLevelItem *item = TopLevelItemForSlot(slot);
  return item ? item->nav_label : "";
}

static void SelectFirstPopulatedCategory(void) {
  const int count = NavSlotCount();
  if (s_category_slot >= 0 && s_category_slot < count &&
      NavRowCount(s_category_slot) > 0)
    return;
  for (int i = 0; i < count; i++) {
    if (NavRowCount(i) > 0) {
      s_category_slot = i;
      return;
    }
  }
  s_category_slot = 0;
}

static const SettingDesc *SelectedDesc(void) {
  SelectFirstPopulatedCategory();
  const SettingDesc *top_level = TopLevelDescForSlot(s_category_slot);
  if (top_level) return top_level;
  SettingCategory category = kCategoryOrder[s_category_slot];
  int row = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->category != category || IsTopLevelDesc(desc) ||
        !Settings_IsMenuVisible(desc))
      continue;
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
  const char *settings_path = getenv("AR_OVERLAY_TEST_SETTINGS_PATH");
  if (!settings_path || !settings_path[0]) settings_path = "settings.ini";
  if (!Settings_Save(settings_path)) {
    SetStatus("SAVE FAILED");
    fprintf(stderr, "[settings-menu] could not save %s\n", settings_path);
    return;
  }
  if (result == kSettingChange_RestartPending)
    SetStatus("RESTART REQUIRED - SAVED");
  else if (result == kSettingChange_AppliedStickyDisable)
    SetStatus("STICKY EFFECTS REMAIN - SAVED");
  else
    SetStatus("APPLIED - SAVED");
}

static void StopEditing(void) {
  if (!s_editing) return;
  s_editing = false;
  if (OverlayWindow()) SDL_StopTextInput(OverlayWindow());
}

static void BeginEditing(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc) ||
      desc->type == kSettingType_Bool ||
      desc->type == kSettingType_Enum ||
      desc->type == kSettingType_Action) {
    SetStatus("NOT TEXT EDITABLE");
    return;
  }
  Settings_FormatValue(desc, s_edit_buffer, sizeof(s_edit_buffer));
  if (desc->apply == kApply_Save &&
      !strcmp(s_edit_buffer, "Leave as-is"))
    s_edit_buffer[0] = 0;
  s_editing = true;
  if (OverlayWindow()) SDL_StartTextInput(OverlayWindow());
  SetStatus("TYPE VALUE - RETURN APPLIES");
}

static void CommitEditing(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!s_editing || !desc) return;
  SettingChangeResult result = Settings_SetText(desc, s_edit_buffer);
  if (result == kSettingChange_Rejected) {
    SetStatus("INVALID VALUE");
    return;
  }
  StopEditing();
  SaveAcceptedChange(result);
}

static void InvokeSelectedAction(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || desc->type != kSettingType_Action) return;
  SetStatus(Settings_InvokeAction(desc) ? "ACTION COMPLETE" : "ACTION FAILED");
}

static void ChangeSelectedValue(int direction) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  if (desc->type == kSettingType_Action) {
    if (direction > 0) InvokeSelectedAction();
    return;
  }
  if (desc->type == kSettingType_Custom) {
    BeginEditing();
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

static void ActivateSelectedRow(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  if (desc->type == kSettingType_Action) {
    InvokeSelectedAction();
  } else if (desc->type == kSettingType_Int ||
             desc->type == kSettingType_Mask ||
             desc->type == kSettingType_Custom) {
    BeginEditing();
  } else {
    ChangeSelectedValue(1);
  }
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
  const int count = NavSlotCount();
  for (int attempt = 0; attempt < count; attempt++) {
    s_category_slot = (s_category_slot + direction + count) % count;
    if (NavRowCount(s_category_slot) > 0) break;
  }
  s_row = 0;
  s_top_row = 0;
  EnsureSelectedNavVisible();
}

static void EnsureSelectedRowVisible(void) {
  int count = NavRowCount(s_category_slot);
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
  int count = NavRowCount(s_category_slot);
  if (count <= 0) return;
  s_row = (s_row + direction + count) % count;
  EnsureSelectedRowVisible();
}

static void ActivateTopLevelSelection(void) {
  const SettingDesc *direct = TopLevelDescForSlot(s_category_slot);
  if (direct) {
    /* Promoted settings/actions are leaves, not one-row submenus. */
    ChangeSelectedValue(1);
    return;
  }
  s_submenu_open = true;
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
  /* M3: host-authored nav icons, independent of which text font loaded. */
  WriteHostIcons();

  for (int i = 0; i < kTextStyle_Count; i++) {
    s_font_textures[i] = CreateFontAtlas((TextStyle)i);
    if (!s_font_textures[i]) {
      DestroyFontTextures();
      return false;
    }
  }
  s_debug_font_texture = CreateDebugFontAtlas();
  if (!s_debug_font_texture) {
    DestroyFontTextures();
    return false;
  }
  s_dialog_frame_texture =
      CreateDialogFrameTexture(rom_data, rom_size);
  return true;
}

void SettingsOverlay_Destroy(void) {
  StopEditing();
  DestroyFontTextures();
  SDL_DestroyTexture(s_dialog_frame_texture);
  s_dialog_frame_texture = NULL;
  s_renderer = NULL;
  s_open = false;
  s_submenu_open = false;
  s_debug_panel_visible = false;
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
  s_debug_panel_user_position = false;
  s_debug_panel_scale_percent = 0;
  s_debug_panel_render_scale_percent = 0;
  s_inspector_info_provider = NULL;
}

bool SettingsOverlay_IsOpen(void) {
  return s_open;
}

void SettingsOverlay_Open(void) {
  StopEditing();
  SelectFirstPopulatedCategory();
  s_submenu_open = false;
  s_open = true;
  s_status[0] = 0;
  fprintf(stderr, "[settings-menu] opened\n");
}

void SettingsOverlay_Close(void) {
  if (!s_open) return;
  StopEditing();
  s_submenu_open = false;
  s_open = false;
  fprintf(stderr, "[settings-menu] closed\n");
}

const char *SettingsOverlay_SelectedKey(void) {
  if (!s_open) return "";
  const SettingDesc *desc = SelectedDesc();
  return desc && desc->key ? desc->key : "";
}

bool SettingsOverlay_GetNavigationState(int *selected_ordinal,
                                        int *top_ordinal,
                                        int *visible_rows,
                                        int *total_rows) {
  if (!s_open) return false;
  SelectFirstPopulatedCategory();
  EnsureSelectedNavVisible();
  if (selected_ordinal)
    *selected_ordinal = NavOrdinalForSlot(s_category_slot);
  if (top_ordinal) *top_ordinal = s_nav_top_row;
  if (visible_rows) *visible_rows = s_nav_visible_rows;
  if (total_rows) *total_rows = NavPopulatedCount();
  return true;
}

bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat) {
  if (!s_open) return false;
  if (key == SDLK_F2) return false;
  if (!pressed) return true;

  if (s_editing) {
    switch (key) {
      case SDLK_ESCAPE:
        StopEditing();
        SetStatus("EDIT CANCELLED");
        break;
      case SDLK_X:       /* SNES A = cancel/back */
        StopEditing();
        s_submenu_open = false;
        SetStatus("EDIT CANCELLED");
        break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (!repeat) CommitEditing();
        break;
      case SDLK_BACKSPACE: {
        size_t length = strlen(s_edit_buffer);
        if (length) s_edit_buffer[length - 1] = 0;
        break;
      }
      default:
        break;
    }
    return true;
  }

  if (!s_submenu_open) {
    switch (key) {
      case SDLK_ESCAPE:
      case SDLK_F1:
      case SDLK_X:       /* SNES A = back/close */
        if (!repeat) SettingsOverlay_Close();
        break;
      case SDLK_UP:
        MoveCategory(-1);
        break;
      case SDLK_DOWN:
        MoveCategory(1);
        break;
      case SDLK_Z:       /* SNES B = game-style confirm */
      case SDLK_RETURN:  /* keyboard confirm */
      case SDLK_KP_ENTER:
        if (!repeat) ActivateTopLevelSelection();
        break;
      default:
        break;
    }
    return true;
  }

  switch (key) {
    case SDLK_ESCAPE:
    case SDLK_F1:
      if (!repeat) SettingsOverlay_Close();
      break;
    case SDLK_X:       /* SNES A = back */
      if (!repeat) s_submenu_open = false;
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
      ChangeSelectedValue(1);
      break;
    case SDLK_Z:       /* SNES B = game-style confirm */
    case SDLK_RETURN:  /* keyboard confirm */
    case SDLK_KP_ENTER:
      ActivateSelectedRow();
      break;
    case SDLK_A:       /* SNES Y */
      ResetSelectedValue();
      break;
    default:
      break;
  }
  return true;
}

bool SettingsOverlay_HandleText(const char *text) {
  if (!s_open || !s_editing) return false;
  if (!text) return true;
  size_t used = strlen(s_edit_buffer);
  size_t available = sizeof(s_edit_buffer) - used - 1;
  if (available) strncat(s_edit_buffer, text, available);
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
  SDL_FRect rect = { (float)x, (float)y, (float)width, (float)height };
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
  SDL_FRect destination =
      ToFRect(LogicalRect(layout, x, y, kGlyphSize, kGlyphSize));
  SDL_FRect source_f = ToFRect(source);
  SDL_RenderTexture(s_renderer, s_dialog_frame_texture,
                    &source_f, &destination);
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
  SDL_FRect source = {
    (float)((ch & 15) * kGlyphSize),
    (float)((ch >> 4) * kGlyphSize),
    (float)kGlyphSize,
    (float)kGlyphSize,
  };
  SDL_FRect destination = ToFRect(LogicalRect(
      layout, x, y, kGlyphSize, kGlyphSize));
  SDL_RenderTexture(s_renderer, texture, &source, &destination);
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

static void DrawDebugGlyph(const MenuLayout *layout, int x, int y,
                           unsigned char ch, DebugTextStyle style) {
  if (ch == ' ' || !s_debug_font_texture) return;
  if (ch >= 128 || !FallbackGlyphDefined(ch)) {
    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
    else ch = '?';
  }
  const SDL_Color color = kDebugTextColors[style];
  SDL_SetTextureColorMod(s_debug_font_texture, color.r, color.g, color.b);
  SDL_SetTextureAlphaMod(s_debug_font_texture, color.a);
  SDL_FRect source = {
    (float)((ch & 15) * kDebugGlyphWidth),
    (float)((ch >> 4) * kDebugGlyphHeight),
    (float)kDebugGlyphWidth,
    (float)kDebugGlyphHeight,
  };
  SDL_FRect destination = ToFRect(LogicalRect(
      layout, x, y, kDebugGlyphWidth, kDebugGlyphHeight));
  SDL_RenderTexture(s_renderer, s_debug_font_texture, &source, &destination);
}

static void DrawDebugTextN(const MenuLayout *layout, int x, int y,
                           const char *text, int length,
                           DebugTextStyle style) {
  if (!text || length <= 0) return;
  for (int i = 0; i < length; i++)
    DrawDebugGlyph(layout, x + i * kDebugGlyphWidth, y,
                   (unsigned char)text[i], style);
}

static bool DebugWordEquals(const char *word, int length,
                            const char *expected) {
  return (int)strlen(expected) == length &&
      !strncmp(word, expected, (size_t)length);
}

static bool DebugWordIsTarget(const char *word, int length) {
  static const char *const targets[] = {
    "BG", "OBJ", "M7", "MODE", "MODE7", "CENTER", "WIDE", "MARGIN",
    "CLAMP", "MIRROR", "REPEAT", "GAP", "HUD-LEFT", "HUD-CENTER",
    "HUD-RIGHT", "ON", "OFF", "TRUE", "FALSE",
  };
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
    if (DebugWordEquals(word, length, targets[i])) return true;
  return false;
}

static bool DebugLineStartsWith(const char *text, int length,
                                const char *prefix) {
  size_t prefix_length = strlen(prefix);
  return prefix_length <= (size_t)length &&
      !strncmp(text, prefix, prefix_length);
}

static void DrawDebugHighlightedLine(const MenuLayout *layout,
                                     int x, int y, const char *text,
                                     int length) {
  if (DebugLineStartsWith(text, length, "LEFT CLICK")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Dim);
    return;
  }
  if (DebugLineStartsWith(text, length, "NO VISIBLE") ||
      DebugLineStartsWith(text, length, "...")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Warning);
    return;
  }
  if (DebugLineStartsWith(text, length, "CANDIDATES") ||
      DebugLineStartsWith(text, length, "HASHES")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Dim);
    return;
  }

  for (int at = 0; at < length;) {
    unsigned char ch = (unsigned char)text[at];
    DebugTextStyle style = kDebugText_Normal;
    int end = at + 1;
    if (ch == '$') {
      while (end < length &&
             ((text[end] >= '0' && text[end] <= '9') ||
              (text[end] >= 'A' && text[end] <= 'F') ||
              (text[end] >= 'a' && text[end] <= 'f')))
        end++;
      style = kDebugText_Value;
    } else if (ch >= '0' && ch <= '9') {
      while (end < length && text[end] >= '0' && text[end] <= '9') end++;
      style = kDebugText_Value;
    } else if ((ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == '_') {
      while (end < length &&
             ((text[end] >= 'A' && text[end] <= 'Z') ||
              (text[end] >= 'a' && text[end] <= 'z') ||
              text[end] == '_' || text[end] == '-'))
        end++;
      style = DebugWordIsTarget(text + at, end - at)
          ? kDebugText_Target : kDebugText_Label;
    }
    DrawDebugTextN(layout, x + at * kDebugGlyphWidth, y,
                   text + at, end - at, style);
    at = end;
  }
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

static void DrawInspectorInfo(const MenuLayout *layout, int x, int y,
                              int max_chars, int max_lines) {
  if (!s_inspector_info_provider || max_chars <= 0 || max_lines <= 0) return;
  char buffer[768];
  buffer[0] = 0;
  s_inspector_info_provider(buffer, sizeof(buffer));
  const char *line = buffer;
  for (int row = 0; row < max_lines && line && *line; row++) {
    const char *end = strchr(line, '\n');
    int length = end ? (int)(end - line) : (int)strlen(line);
    if (length > max_chars) length = max_chars;
    DrawTextN(layout, x, y + row * 10, line, length, kText_Dim);
    line = end ? end + 1 : NULL;
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

static MenuLayout BuildLayoutAtScale(int output_width, int output_height,
                                     int scale) {
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

static MenuLayout BuildLayout(int output_width, int output_height) {
  int fit_scale = SnappedFitScale(output_width, output_height);
  s_auto_menu_scale_percent = fit_scale;
  int scale = g_settings.menu_scale_percent > 0
      ? g_settings.menu_scale_percent : fit_scale;
  if (scale > fit_scale) scale = fit_scale;
  if (scale < kMinimumScalePercent) scale = kMinimumScalePercent;
  return BuildLayoutAtScale(output_width, output_height, scale);
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
  /* Save-state/item labels need up to 18 characters (for example
   * "Act 2 cleared" and "Strength of Angel"). Town rows deliberately use the
   * shorter "X State" label so this wider value column still leaves ample
   * room. Label width is computed per row from the actual formatted value, so
   * short values such as RUN do not waste the rest of that reservation. */
  const int value_chars = 18;

  DrawText(layout, left_text_x, left_title_y,
           "SYSTEM SETTINGS", kText_Normal);
  /* SDL3 removed SDL_TICKS_PASSED; SDL_GetTicks is now 64-bit and never wraps
   * in practice, so a direct comparison is exact. */
  if (s_status[0] && SDL_GetTicks() >= s_status_until)
    s_status[0] = 0;
  SelectFirstPopulatedCategory();
  const SettingDesc *top_level = TopLevelDescForSlot(s_category_slot);
  SettingCategory category = s_category_slot < CategorySlotCount()
      ? kCategoryOrder[s_category_slot] : kSettingCat_Extras;
  const char *category_name = top_level
      ? NavSlotLabel(s_category_slot) : Settings_CategoryName(category);
  char save_category_name[32];
  if (!top_level && category == kSettingCat_Save &&
      g_settings.save_editor_page >= 0 &&
      g_settings.save_editor_page < kSaveEditorPage_Count) {
    static const char *const page_names[] = {
      "Progress", "Status", "Magic", "Items", "Scores",
    };
    snprintf(save_category_name, sizeof(save_category_name), "Save: %s",
             page_names[g_settings.save_editor_page]);
    category_name = save_category_name;
  }
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

  const int category_count = CategorySlotCount();
  const int nav_count = NavSlotCount();
  const int nav_total = NavPopulatedCount();
  s_nav_visible_rows =
      (top_y + top_height - 12 - category_first_y) / kNavRowHeight + 1;
  if (s_nav_visible_rows < 1) s_nav_visible_rows = 1;
  EnsureSelectedNavVisible();

  int nav_ordinal = 0;
  for (int slot = 0; slot < nav_count; slot++) {
    if (NavRowCount(slot) <= 0) continue;
    int ordinal = nav_ordinal++;
    if (ordinal < s_nav_top_row ||
        ordinal >= s_nav_top_row + s_nav_visible_rows)
      continue;
    int category_y =
        category_first_y + (ordinal - s_nav_top_row) * kNavRowHeight;
    /* Keep lifecycle actions visually separated without spending another row;
     * the previous fixed four-pixel gap was what pushed Exit past the panel. */
    if (slot == category_count && ordinal > s_nav_top_row)
      FillLogicalRect(layout, left_x + 12, category_y - 1,
                      left_width - 24, 1, 0x6080A0C0u);
    TextStyle style = slot == s_category_slot ? kText_Normal : kText_Dim;
    if (slot == s_category_slot && !s_submenu_open)
      DrawGlyph(layout, left_text_x + ((SDL_GetTicks() / 300) & 1),
                category_y, '>', kText_Warning);
    /* M3: a small category icon sits in the gutter between the selector
     * cursor and the label; top-level command leaves (Restart game, Exit
     * desktop) have no icon and just leave that gutter blank, keeping every
     * row's label starting column aligned. */
    if (slot < category_count) {
      int icon_tile = CategoryIconTile(kCategoryOrder[slot]);
      if (icon_tile >= 0)
        DrawGlyph(layout, left_text_x + 10, category_y,
                  (unsigned char)icon_tile, style);
    }
    DrawTextN(layout, left_text_x + 20, category_y,
              NavSlotLabel(slot), 13, style);
  }
  /* Indicators live in the otherwise-unused far-right gutter so they do not
   * consume a navigation row or collide with the selection cursor. */
  const int nav_indicator_x = left_x + left_width - 12;
  if (s_nav_top_row > 0)
    DrawGlyph(layout, nav_indicator_x, category_first_y,
              '^', kText_Warning);
  if (s_nav_top_row + s_nav_visible_rows < nav_total)
    DrawGlyph(layout, nav_indicator_x,
              category_first_y + (s_nav_visible_rows - 1) * kNavRowHeight,
              'v', kText_Warning);

  s_visible_rows =
      (top_y + top_height - 12 - first_row_y) / kRowHeight + 1;
  if (s_visible_rows < 1) s_visible_rows = 1;
  if (!top_level) EnsureSelectedRowVisible();

  int category_row = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (top_level || desc->category != category || IsTopLevelDesc(desc) ||
        !Settings_IsMenuVisible(desc))
      continue;
    int row = category_row++;
    if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
    int y = first_row_y + (row - s_top_row) * kRowHeight;
    if (category == kSettingCat_Save &&
        (!strcmp(desc->key, "save_editor_page") ||
         !strcmp(desc->key, "save_apply_session")))
      FillLogicalRect(layout, right_x + 12, y - 3,
                      right_width - 24, 1, 0x6080A0C0u);
    bool selected = s_submenu_open && row == s_row;
    bool available = Settings_IsAvailable(desc);
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2,
                      right_width - 18,
                      11, kHighlight);
      DrawGlyph(layout, selector_x + ((SDL_GetTicks() / 250) & 1),
                y, '>', kText_Warning);
    }
    TextStyle style = available && s_submenu_open
        ? kText_Normal : kText_Dim;

    char value[512];
    if (selected && s_editing) {
      snprintf(value, sizeof(value), "%s", s_edit_buffer);
    } else {
      Settings_FormatValue(desc, value, sizeof(value));
    }
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
    int shown_value_chars = CappedTextLength(value, value_chars);
    int row_value_left = value_right - shown_value_chars * kGlyphSize;
    int restart_x = row_value_left - 12;
    int label_chars = (restart_x - label_x - 4) / kGlyphSize;
    if (label_chars < 1) label_chars = 1;
    DrawTextN(layout, label_x, y, desc->label, label_chars, style);
    /* M2 (followup doc): values render in kText_Value (cool cyan) so they
     * read distinct from labels, but only for normal/enabled rows — a
     * dim/unavailable row's style must win so it stays visibly greyed,
     * matching M4's dim-when-unavailable intent instead of lighting up in
     * bright cyan. */
    DrawTextRight(layout, value_right, y, value, value_chars,
                  style == kText_Normal ? kText_Value : style);
    if (desc->apply == kApply_Restart)
      DrawGlyph(layout, restart_x, y, '*', kText_Warning);
  }

  if (!top_level && category == kSettingCat_Inspector) {
    int info_y = first_row_y + category_row * kRowHeight + 5;
    FillLogicalRect(layout, right_x + 12, info_y - 4,
                    right_width - 24, 1, 0x6080A0C0u);
    DrawText(layout, right_text_x, info_y, "LIVE SCENE", kText_Normal);
    DrawInspectorInfo(layout, right_text_x, info_y + 12,
                      (right_width - 24) / kGlyphSize, 5);
  }

  const SettingDesc *selected = top_level
      ? top_level : (s_submenu_open ? SelectedDesc() : NULL);
  if (selected) {
    int tooltip_chars = (bottom_width - 24) / kGlyphSize;
    DrawWrappedText(layout, margin + 12, bottom_y + 12,
                    selected->tooltip,
                    tooltip_chars, 3,
                    Settings_IsAvailable(selected)
                        ? kText_Normal : kText_Dim);
  }
  DrawText(layout, margin + 12, bottom_y + bottom_height - 18,
           s_editing
               ? "TYPE VALUE  RETURN APPLY  A/ESC CANCEL"
               : (s_submenu_open
                    ? "D-PAD SELECT/CHANGE B EDIT/RUN Y RESET A BACK"
                    : "UP/DOWN SELECT  B OPEN/RUN  A CLOSE"),
           kText_Dim);
}

void SettingsOverlay_Render(SDL_Rect game_viewport) {
  if (!s_open || !s_renderer || !s_font_textures[kText_Normal]) return;
  int output_width = 0;
  int output_height = 0;
  if (!SDL_GetRenderOutputSize(
          s_renderer, &output_width, &output_height) ||
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

void SettingsOverlay_RenderDebugPanel(const char *title, const char *text,
                                      SDL_Point avoid_point) {
  if (!s_renderer || !s_debug_font_texture || !text || !text[0])
    return;
  int output_width = 0, output_height = 0;
  if (!SDL_GetRenderOutputSize(s_renderer, &output_width, &output_height) ||
      output_width <= 0 || output_height <= 0)
    return;

  SDL_BlendMode old_blend_mode = SDL_BLENDMODE_NONE;
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  SDL_GetRenderDrawBlendMode(s_renderer, &old_blend_mode);
  SDL_GetRenderDrawColor(s_renderer, &old_r, &old_g, &old_b, &old_a);
  SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);

  MenuLayout layout = BuildLayout(output_width, output_height);
  /* Debug reports should remain information-dense even when the settings
   * menu itself is enlarged for couch-distance use. A lower-right resize grip
   * can then override this automatic scale without changing the report's
   * logical width or truncating additional columns. */
  int automatic_scale = layout.scale_percent;
  if (automatic_scale > 250) automatic_scale = 250;
  int maximum_scale = SnappedFitScale(output_width, output_height);
  if (maximum_scale > 250) maximum_scale = 250;
  int debug_scale = s_debug_panel_scale_percent > 0
      ? s_debug_panel_scale_percent : automatic_scale;
  if (debug_scale > maximum_scale) debug_scale = maximum_scale;
  if (debug_scale < 50) debug_scale = 50;
  layout = BuildLayoutAtScale(output_width, output_height, debug_scale);
  s_debug_panel_render_scale_percent = debug_scale;
  const char *debug_title = title ? title : "DEBUG";
  int lines = 0;
  int longest_line = 0;
  const char *measure = text;
  while (*measure && lines < 12) {
    const char *newline = strchr(measure, '\n');
    int length = newline ? (int)(newline - measure) : (int)strlen(measure);
    if (length > longest_line) longest_line = length;
    lines++;
    if (!newline) break;
    measure = newline + 1;
  }
  if (lines < 1) lines = 1;
  int panel_height = 30 + lines * 10;
  if (panel_height > layout.logical_height - 16)
    panel_height = layout.logical_height - 16;
  int title_length = (int)strlen(debug_title);
  int content_chars = longest_line > title_length
      ? longest_line : title_length;
  int panel_width = content_chars * kDebugGlyphWidth + 24;
  if (panel_width < 200) panel_width = 200;
  panel_width = (panel_width + kGlyphSize - 1) & ~(kGlyphSize - 1);
  int maximum_panel_width = layout.logical_width - 16;
  if (maximum_panel_width > 560) maximum_panel_width = 560;
  maximum_panel_width &= ~(kGlyphSize - 1);
  if (panel_width > maximum_panel_width) panel_width = maximum_panel_width;
  int panel_x = (layout.logical_width - panel_width) / 2;
  int panel_y = avoid_point.y < output_height / 2
      ? layout.logical_height - panel_height - 8 : 8;
  if (s_debug_panel_user_position) {
    panel_x = (s_debug_panel_output_x - layout.origin_x) * 100 /
        layout.scale_percent;
    panel_y = (s_debug_panel_output_y - layout.origin_y) * 100 /
        layout.scale_percent;
  }
  int max_x = layout.logical_width - panel_width;
  int max_y = layout.logical_height - panel_height;
  if (panel_x < 0) panel_x = 0;
  if (panel_y < 0) panel_y = 0;
  if (panel_x > max_x) panel_x = max_x;
  if (panel_y > max_y) panel_y = max_y;
  DrawDialogPanel(&layout, panel_x, panel_y, panel_width, panel_height);
  DrawDebugTextN(&layout, panel_x + 12, panel_y + 9,
                 debug_title, (int)strlen(debug_title), kDebugText_Label);

  int max_chars = (panel_width - 24) / kDebugGlyphWidth;
  const char *cursor = text;
  for (int line = 0; line < lines && *cursor; line++) {
    const char *newline = strchr(cursor, '\n');
    int length = newline ? (int)(newline - cursor) : (int)strlen(cursor);
    if (length > max_chars) length = max_chars;
    DrawDebugHighlightedLine(
        &layout, panel_x + 12,
        panel_y + 21 + line * kDebugLineHeight, cursor, length);
    if (!newline) break;
    cursor = newline + 1;
  }

  /* Three short diagonal bars advertise the scale handle without replacing
   * the native bottom-right frame corner. */
  FillLogicalRect(&layout, panel_x + panel_width - 13,
                  panel_y + panel_height - 5, 9, 1,
                  ARGB(255, 92, 196, 255));
  FillLogicalRect(&layout, panel_x + panel_width - 10,
                  panel_y + panel_height - 8, 6, 1,
                  ARGB(255, 92, 196, 255));
  FillLogicalRect(&layout, panel_x + panel_width - 7,
                  panel_y + panel_height - 11, 3, 1,
                  ARGB(255, 92, 196, 255));

  s_debug_panel_rect = LogicalRect(
      &layout, panel_x, panel_y, panel_width, panel_height);
  /* The title strip moves the panel and the lower-right corner scales it.
   * Remaining report-body clicks pass through to the scene inspector so a
   * panel covering a requested sample cannot retain an old crosshair. */
  s_debug_panel_drag_rect = LogicalRect(
      &layout, panel_x, panel_y, panel_width, 20);
  s_debug_panel_resize_rect = LogicalRect(
      &layout, panel_x + panel_width - 18,
      panel_y + panel_height - 18, 18, 18);
  s_debug_panel_visible = true;
  if (s_debug_panel_user_position) {
    s_debug_panel_output_x = s_debug_panel_rect.x;
    s_debug_panel_output_y = s_debug_panel_rect.y;
  }

  SDL_SetRenderDrawBlendMode(s_renderer, old_blend_mode);
  SDL_SetRenderDrawColor(s_renderer, old_r, old_g, old_b, old_a);
}

void SettingsOverlay_HideDebugPanel(void) {
  s_debug_panel_visible = false;
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
}

bool SettingsOverlay_BeginDebugPanelDrag(int output_x, int output_y) {
  if (!s_debug_panel_visible) return false;
  if (output_x >= s_debug_panel_resize_rect.x &&
      output_x < s_debug_panel_resize_rect.x + s_debug_panel_resize_rect.w &&
      output_y >= s_debug_panel_resize_rect.y &&
      output_y < s_debug_panel_resize_rect.y + s_debug_panel_resize_rect.h) {
    s_debug_panel_resizing = true;
    s_debug_panel_dragging = false;
    s_debug_panel_user_position = true;
    s_debug_panel_output_x = s_debug_panel_rect.x;
    s_debug_panel_output_y = s_debug_panel_rect.y;
    s_debug_panel_resize_start_x = output_x;
    s_debug_panel_resize_start_y = output_y;
    s_debug_panel_resize_start_width = s_debug_panel_rect.w;
    s_debug_panel_resize_start_height = s_debug_panel_rect.h;
    s_debug_panel_resize_start_scale = s_debug_panel_render_scale_percent;
    return true;
  }
  if (output_x < s_debug_panel_drag_rect.x ||
      output_x >= s_debug_panel_drag_rect.x + s_debug_panel_drag_rect.w ||
      output_y < s_debug_panel_drag_rect.y ||
      output_y >= s_debug_panel_drag_rect.y + s_debug_panel_drag_rect.h)
    return false;
  s_debug_panel_dragging = true;
  s_debug_panel_user_position = true;
  s_debug_panel_output_x = s_debug_panel_rect.x;
  s_debug_panel_output_y = s_debug_panel_rect.y;
  s_debug_panel_drag_offset_x = output_x - s_debug_panel_rect.x;
  s_debug_panel_drag_offset_y = output_y - s_debug_panel_rect.y;
  return true;
}

void SettingsOverlay_DragDebugPanel(int output_x, int output_y) {
  if ((!s_debug_panel_dragging && !s_debug_panel_resizing) || !s_renderer)
    return;
  if (s_debug_panel_resizing) {
    int dx = output_x - s_debug_panel_resize_start_x;
    int dy = output_y - s_debug_panel_resize_start_y;
    int change_x = s_debug_panel_resize_start_width > 0
        ? dx * 100 / s_debug_panel_resize_start_width : 0;
    int change_y = s_debug_panel_resize_start_height > 0
        ? dy * 100 / s_debug_panel_resize_start_height : 0;
    int change = abs(change_x) >= abs(change_y) ? change_x : change_y;
    int scale = s_debug_panel_resize_start_scale * (100 + change) / 100;
    scale = ((scale + 2) / 5) * 5;
    if (scale < 50) scale = 50;
    if (scale > 250) scale = 250;
    s_debug_panel_scale_percent = scale;
    return;
  }
  int output_width = 0, output_height = 0;
  if (!SDL_GetRenderOutputSize(s_renderer, &output_width, &output_height) ||
      output_width <= 0 || output_height <= 0)
    return;
  int x = output_x - s_debug_panel_drag_offset_x;
  int y = output_y - s_debug_panel_drag_offset_y;
  int max_x = output_width - s_debug_panel_rect.w;
  int max_y = output_height - s_debug_panel_rect.h;
  if (max_x < 0) max_x = 0;
  if (max_y < 0) max_y = 0;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > max_x) x = max_x;
  if (y > max_y) y = max_y;
  s_debug_panel_output_x = x;
  s_debug_panel_output_y = y;
  int drag_dx = x - s_debug_panel_rect.x;
  int drag_dy = y - s_debug_panel_rect.y;
  s_debug_panel_rect.x = x;
  s_debug_panel_rect.y = y;
  s_debug_panel_drag_rect.x += drag_dx;
  s_debug_panel_drag_rect.y += drag_dy;
  s_debug_panel_resize_rect.x += drag_dx;
  s_debug_panel_resize_rect.y += drag_dy;
}

void SettingsOverlay_EndDebugPanelDrag(void) {
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
}

bool SettingsOverlay_IsDebugPanelDragging(void) {
  return s_debug_panel_dragging || s_debug_panel_resizing;
}

bool SettingsOverlay_GetDebugPanelRect(SDL_Rect *rect) {
  if (!s_debug_panel_visible || !rect) return false;
  *rect = s_debug_panel_rect;
  return true;
}
