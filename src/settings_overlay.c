#include "settings_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_map.h"
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
  kRowHeight = 13,
  kMinimumLayoutWidth = 464,
  kMinimumLayoutHeight = 208,
  kMinimumScalePercent = 25,
  kMaximumScalePercent = 800,
  /* Nav rows are as tall as a section icon; the eight sections then fill the
   * column without scrolling at any ordinary window size. */
  kNavRowHeight = 17,
  kSmallLineHeight = 9,
  kTabBarHeight = 12,
  /* Hold-to-accelerate timing. The initial delay is what separates a tap
   * (single fine step) from a hold; after it, a step fires every interval. */
  kHoldInitialDelayMs = 350,
  kHoldRepeatMs = 55,
};

#define ARGB(a, r, g, b) \
  ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
   (uint32_t)(g) << 8 | (uint32_t)(b))

static const uint32_t kPanel = ARGB(255, 0, 0, 0);
static const uint32_t kFrameDark = ARGB(255, 45, 63, 78);
static const uint32_t kFrameLight = ARGB(255, 164, 196, 219);
static const uint32_t kHighlight = ARGB(255, 22, 57, 83);

/* Colors sampled from the game's own Sky Palace menu CGRAM (runs/.../cgram),
 * so the overlay reads as ActRaiser rather than drifting into a custom scheme:
 *  - the steel blue of the dialog frame carries all passive chrome (titles,
 *    tab strip, rules, scrollbars),
 *  - the menu's selection yellow marks wherever the cursor currently is (the
 *    highlighted section, row, and active tab), exactly like the game's
 *    yellow-bordered selected item slot,
 *  - the warm gold is the game's own highlight text color (CGRAM pal0 #6),
 *    used for the blinking cursor and the restart marker. */
static const uint32_t kSteelBlue = ARGB(255, 164, 196, 219);
static const uint32_t kSteelDim = ARGB(255, 74, 104, 130);
static const uint32_t kSelectYellow = ARGB(255, 255, 230, 0);
static const uint32_t kGameGold = ARGB(255, 255, 180, 65);
static const uint32_t kMutedText = ARGB(255, 120, 140, 158);

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
 * warning remaps of those same three pixel classes. Every non-transparent
 * color here is a color from the game's own menu CGRAM. */
static const uint32_t kTextPalettes[kTextStyle_Count][4] = {
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

/* ── Sections and tabs (M1(b), followup doc) ─────────────────────────────
 * The nav column used to list one row per SettingCategory, which meant 13
 * rows of near-synonyms (Display / Diorama / Simulation / Graphics /
 * Widescreen were all "how the game looks") and two categories that alone
 * carried 45 and 52 rows. Navigation is now two levels:
 *
 *   SECTION  — what the nav column lists. Eight of them, each with a 16x16
 *              game menu icon (grey when unselected, the game's colored slot
 *              palette when current).
 *   TAB      — the horizontal strip at the top of the submenu, cycled with
 *              L/R (pad) or Q/E, [/], Tab (keyboard). One tab is one
 *              SettingCategory, so a tab is always a panel-sized row list.
 *
 * A tab may additionally own a paging setting (`page_key`/`page_value`).
 * Save's five editor pages and Controls' keyboard/gamepad binding pages were
 * already row-filtered by such a setting; making the tab drive it turns two
 * bespoke in-list page selectors into the same mechanism as everything else,
 * and those two rows stop listing themselves (Settings_IsMenuVisible). */
typedef struct MenuTab {
  SettingCategory category;
  const char *label;
  const char *page_key;   /* NULL, or the setting this tab selects */
  long page_value;
} MenuTab;

typedef struct MenuSection {
  const char *label;
  const char *blurb;      /* shown in the description panel from the nav column */
  const MenuTab *tabs;
  int tab_count;
} MenuSection;

#define TAB(cat, name) { kSettingCat_##cat, name, NULL, 0 }
#define PAGE_TAB(cat, name, key, value) { kSettingCat_##cat, name, key, value }

static const MenuTab kTabsVideo[] = {
  TAB(Display, "General"),
  TAB(Graphics, "Effects"),
  TAB(Widescreen, "Widescreen"),
};
static const MenuTab kTabsDiorama[] = {
  TAB(Presentation, "Scene"),
  TAB(DioramaCamera, "Camera"),
};
static const MenuTab kTabsTown[] = {
  TAB(Simulation, "Scene"),
  TAB(SimCamera, "Camera"),
  TAB(SimLighting, "Light"),
  TAB(SimAtmosphere, "Weather"),
};
static const MenuTab kTabsAudio[] = {
  TAB(Audio, "Audio"),
};
static const MenuTab kTabsControls[] = {
  TAB(Input, "Devices"),
  PAGE_TAB(InputBinds, "Keyboard", "input_bind_page", 0),
  PAGE_TAB(InputBinds, "Gamepad", "input_bind_page", 1),
};
static const MenuTab kTabsCheats[] = {
  TAB(Cheats, "Cheats"),
};
static const MenuTab kTabsSave[] = {
  /* The backend/arming controls and the apply/import/export commands used to
   * repeat on every editor page, inflating each list. They live on their own
   * Actions tab now, so the payload pages stay short. */
  PAGE_TAB(Save, "Actions", "save_editor_page", kSaveEditorPage_Actions),
  PAGE_TAB(Save, "Progress", "save_editor_page", kSaveEditorPage_Progress),
  PAGE_TAB(Save, "Status", "save_editor_page", kSaveEditorPage_Status),
  PAGE_TAB(Save, "Magic", "save_editor_page", kSaveEditorPage_Magic),
  PAGE_TAB(Save, "Items", "save_editor_page", kSaveEditorPage_Items),
  PAGE_TAB(Save, "Scores", "save_editor_page", kSaveEditorPage_Scores),
};
static const MenuTab kTabsSystem[] = {
  TAB(Extras, "Tools"),
  TAB(Enhancements, "Game"),
  TAB(Inspector, "Inspector"),
};

#undef TAB
#undef PAGE_TAB

#define SECTION(name, blurb, tabs) \
  { name, blurb, tabs, (int)(sizeof(tabs) / sizeof((tabs)[0])) }

/* Icon maps below are indexed by position in this array — keep the two in the
 * same order. Restart/Exit are no longer promoted nav leaves: they are the
 * last two rows of System > Tools, where their descriptors already lived. Each
 * section's identity is now carried entirely by its game icon (grey when
 * unselected, the colored game slot palette when current); all chrome is the
 * shared steel-blue/yellow game scheme. */
static const MenuSection kSections[] = {
  SECTION("Video", "Window, aspect, shader effects and widescreen behavior.",
          kTabsVideo),
  SECTION("Diorama", "Tilt the action stages into a layered 3D diorama.",
          kTabsDiorama),
  SECTION("Town 3D", "Project the simulation town onto a 3D ground plane.",
          kTabsTown),
  SECTION("Audio", "Output device, mixing and music replacement.",
          kTabsAudio),
  SECTION("Controls", "Input device, analog tuning and every key binding.",
          kTabsControls),
  SECTION("Cheats", "Gameplay assists and raw memory pins.",
          kTabsCheats),
  SECTION("Save", "Inspect and stage edits to the battery save.",
          kTabsSave),
  SECTION("System", "Host commands, restart and exit, plus the scene inspector.",
          kTabsSystem),
};

#undef SECTION

enum { kSectionCount = (int)(sizeof(kSections) / sizeof(kSections[0])) };

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
static int s_section;
/* Per-section tab memory: leaving Town 3D on its Weather tab and coming back
 * later returns to Weather, not to Scene. */
static int s_tab[kSectionCount];
/* Horizontal scroll of the tab strip (index of the first shown VISIBLE tab)
 * for sections whose tabs are wider than the panel — e.g. Save's six pages.
 * Kept so the strip shifts to keep the active tab on screen. */
static int s_tab_scroll;
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
/* Hold-to-accelerate stepping. A numeric row's value is nudged once on the
 * Left/Right press, then SettingsOverlay_Tick (driven each frame from the main
 * thread while the menu is open) keeps stepping it as long as the direction is
 * held, growing the step the longer it is held so a large range is both fine-
 * tunable and quick to cross without ever typing. s_hold_key is the keyboard
 * key that began the hold (0 for a pad), used to match the release; each step
 * applies live but the settings.ini write is deferred to release
 * (s_hold_dirty) so a fast hold is not one disk write per frame. */
static const SettingDesc *s_hold_desc;
static int s_hold_dir;
static Uint64 s_hold_start_ms;
static Uint64 s_hold_next_ms;
static SDL_Keycode s_hold_key;
static bool s_hold_dirty;
static SettingChangeResult s_hold_result;
/* The keyboard key of the event currently being dispatched (0 for a pad), so a
 * hold started deep inside ApplyMenuNav knows which key release will end it. */
static SDL_Keycode s_input_key;
/* Binding capture: the row is armed and the NEXT physical input on the
 * matching device becomes its binding. Held separately from s_editing because
 * capture consumes raw events rather than text. */
static const SettingDesc *s_capture_desc;
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
  for (int row = 0; row < 8; row++)
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
 * snapshot (tools/dump_snapshot_chr.py + icon_picker_sheet.py). Each is a
 * 16x16 4bpp index map — the game stores these as framed item/magic/status
 * glyphs — rendered through the game's OWN menu CGRAM palettes: the grey slot
 * palette (pal 14) for an unselected section, and the colored "selected slot"
 * palette (pal 13, red/gold frame) for the current one, exactly as the game
 * lights up the item you are pointing at. Index 14 is the black outline. */
enum {
  kIconSize = 16,
  kIconAtlasWidth = kIconSize * kSectionCount,
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

static const IconIndexMap kSectionIconMaps[kSectionCount] = {
  { /* Display <- game icon #11 */
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
  { /* Diorama <- game icon #12 */
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
  { /* Simulation <- game icon #10 */
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
  { /* Audio <- game icon #53 */
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
  { /* Input <- game icon #18 */
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
  { /* Cheats <- game icon #14 */
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
  { /* Save <- game icon #16 */
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
  { /* Extras <- game icon #54 */
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
};

static SDL_Texture *s_icon_texture;

static uint32_t ScaleColor(uint32_t color, int percent) {
  unsigned r = ((color >> 16) & 0xff) * (unsigned)percent / 100;
  unsigned g = ((color >> 8) & 0xff) * (unsigned)percent / 100;
  unsigned b = (color & 0xff) * (unsigned)percent / 100;
  return ARGB(255, r, g, b);
}

static SDL_Texture *CreateIconAtlas(void) {
  uint32_t *pixels = (uint32_t *)calloc(
      (size_t)kIconAtlasWidth * kIconAtlasHeight, sizeof(uint32_t));
  if (!pixels) return NULL;

  /* Row 0 = grey (unselected), row 1 = colored (selected), each icon straight
   * through the game palette so the colors are the game's own. */
  for (int row = 0; row < 2; row++) {
    const uint32_t *palette = row == 0 ? kIconGreyPalette : kIconSelectPalette;
    for (int section = 0; section < kSectionCount; section++) {
      for (int y = 0; y < kIconSize; y++) {
        for (int x = 0; x < kIconSize; x++) {
          uint8_t index = kSectionIconMaps[section][y][x];
          uint32_t color = palette[index];
          if ((color >> 24) == 0) continue;   /* transparent index */
          pixels[(row * kIconSize + y) * kIconAtlasWidth +
                 section * kIconSize + x] = color;
        }
      }
    }
  }

  SDL_Texture *texture = SDL_CreateTexture(
      s_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
      kIconAtlasWidth, kIconAtlasHeight);
  if (texture &&
      !SDL_UpdateTexture(texture, NULL, pixels,
                         kIconAtlasWidth * (int)sizeof(uint32_t))) {
    SDL_DestroyTexture(texture);
    texture = NULL;
  }
  free(pixels);
  if (texture) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  }
  return texture;
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
    /* Lowercase is authored for real now; fold onto the capital only for the
     * handful of codepoints that still have no lowercase mask. */
    if (source_ch >= 128 || !FallbackGlyphDefined(source_ch)) {
      if (source_ch >= 'a' && source_ch <= 'z')
        source_ch = source_ch - 'a' + 'A';
      else
        source_ch = '?';
    }
    if (source_ch >= 128 || !FallbackGlyphDefined(source_ch))
      source_ch = '?';
    int cell_x = (int)(ch & 15) * kDebugGlyphWidth;
    int cell_y = (int)(ch >> 4) * kDebugGlyphHeight;
    for (int row = 0; row < 8; row++) {
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

/* ── Section / tab / row addressing ─────────────────────────────────────
 * s_section indexes kSections. s_tab[section] remembers which tab that
 * section was last left on, so stepping away and back does not dump the
 * player at the top of a four-tab section. Rows are the visible descriptors
 * of the active tab's category, in descriptor-table order. */
static const MenuSection *ActiveSection(void) {
  if (s_section < 0) s_section = 0;
  if (s_section >= kSectionCount) s_section = kSectionCount - 1;
  return &kSections[s_section];
}

/* A tab is hidden when it holds no visible rows — which happens when every row
 * it would list is developer-only and debug settings are off (the Town 3D
 * Light/Weather tabs and the System Inspector tab collapse this way). Page tabs
 * (Save pages, Controls binding pages) share always-visible rows, so they never
 * collapse and are cheap to short-circuit. */
static bool RawTabHidden(int section, int tab) {
  const MenuTab *menu_tab = &kSections[section].tabs[tab];
  if (menu_tab->page_key) return false;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->category == menu_tab->category && Settings_IsMenuVisible(desc))
      return false;
  }
  return true;
}

static int VisibleTabCount(int section) {
  int count = 0;
  for (int tab = 0; tab < kSections[section].tab_count; tab++)
    if (!RawTabHidden(section, tab)) count++;
  return count < 1 ? 1 : count;
}

static int ActiveTabIndex(void) {
  const MenuSection *section = ActiveSection();
  int tab = s_tab[s_section];
  if (tab < 0) tab = 0;
  if (tab >= section->tab_count) tab = section->tab_count - 1;
  /* Never leave the cursor parked on a collapsed tab: if debug settings were
   * turned off while it was there, slide to the next visible tab. */
  if (RawTabHidden(s_section, tab)) {
    for (int i = 1; i <= section->tab_count; i++) {
      int candidate = (tab + i) % section->tab_count;
      if (!RawTabHidden(s_section, candidate)) {
        tab = candidate;
        break;
      }
    }
  }
  s_tab[s_section] = tab;
  return tab;
}

/* Position of the active tab among the visible ones — the index the tab bar
 * and the test navigation count in, since hidden tabs are not shown. */
static int ActiveVisibleTabPosition(void) {
  int active = ActiveTabIndex();
  int position = 0;
  for (int tab = 0; tab < active; tab++)
    if (!RawTabHidden(s_section, tab)) position++;
  return position;
}

static const MenuTab *ActiveTab(void) {
  return &ActiveSection()->tabs[ActiveTabIndex()];
}

/* A paging tab owns a real setting (save_editor_page / input_bind_page) that
 * Settings_IsMenuVisible filters rows against. Push the tab's value into it
 * before anything enumerates rows, so the row list and the highlighted tab
 * can never disagree.
 *
 * Deliberately a direct store rather than Settings_SetLong: row enumeration
 * happens during SettingsOverlay_Render, which runs on the present thread
 * (present.c), and the host's change observer quiesces that very thread —
 * routing this through the mutation API would deadlock the presenter against
 * itself. Both fields are plain in-range enum selectors with no callback and
 * no restart semantics, so there is nothing for the normalizing path to do
 * here anyway. */
static void SyncActiveTabPage(void) {
  const MenuTab *tab = ActiveTab();
  if (!tab->page_key) return;
  const SettingDesc *desc = Settings_Find(tab->page_key);
  if (!desc || desc->type != kSettingType_Enum || !desc->field) return;
  if (tab->page_value < desc->minval || tab->page_value > desc->maxval) return;
  *(int *)desc->field = (int)tab->page_value;
}

static bool RowBelongsToActiveTab(const SettingDesc *desc) {
  return desc->category == ActiveTab()->category &&
         Settings_IsMenuVisible(desc);
}

static int TabRowCount(void) {
  SyncActiveTabPage();
  int count = 0;
  for (int i = 0; i < g_setting_desc_count; i++)
    if (RowBelongsToActiveTab(&g_setting_descs[i])) count++;
  return count;
}

/* Nav rows are the sections themselves; a section with no populated tab at
 * all would be dead, but every section here always has at least one row, so
 * the nav list is a fixed eight and never renumbers under the cursor. */
static int NavPopulatedCount(void) {
  return kSectionCount;
}

static void EnsureSelectedNavVisible(void) {
  int visible = s_nav_visible_rows > 0 ? s_nav_visible_rows : 1;
  if (s_section < s_nav_top_row) s_nav_top_row = s_section;
  if (s_section >= s_nav_top_row + visible)
    s_nav_top_row = s_section - visible + 1;
  int maximum_top = kSectionCount > visible ? kSectionCount - visible : 0;
  if (s_nav_top_row > maximum_top) s_nav_top_row = maximum_top;
  if (s_nav_top_row < 0) s_nav_top_row = 0;
}

static const SettingDesc *SelectedDesc(void) {
  SyncActiveTabPage();
  int row = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (!RowBelongsToActiveTab(desc)) continue;
    if (row++ == s_row) return desc;
  }
  return NULL;
}


static void SetStatus(const char *text) {
  snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
  s_status_until = SDL_GetTicks() + 2500;
}

/* Persist the current settings to disk and report the outcome. Split from the
 * apply step so a held value can be applied live every frame but written once
 * on release. */
static void PersistChange(SettingChangeResult result) {
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

static void SaveAcceptedChange(SettingChangeResult result) {
  if (result <= kSettingChange_Unchanged) {
    SetStatus(result == kSettingChange_Rejected ? "NOT EDITABLE" : "UNCHANGED");
    return;
  }
  PersistChange(result);
}

/* Flush a deferred write left by a hold, and forget the held row. Safe to call
 * unconditionally — no-op when nothing is held. */
static void EndValueHold(void) {
  if (!s_hold_desc) return;
  bool dirty = s_hold_dirty;
  SettingChangeResult result = s_hold_result;
  s_hold_desc = NULL;
  s_hold_dir = 0;
  s_hold_key = 0;
  s_hold_dirty = false;
  if (dirty) PersistChange(result);
}

/* Round to a "nice" magnitude (1, 2, or 5 times a power of ten) so an
 * accelerated step lands on tidy numbers rather than something like 83. */
static long NiceStep(long value) {
  if (value < 1) return 1;
  long magnitude = 1;
  while (magnitude * 10 <= value) magnitude *= 10;
  long lead = value / magnitude;
  long snapped = lead < 2 ? 1 : lead < 5 ? 2 : 5;
  return snapped * magnitude;
}

/* How many base steps a single held repeat should move, given how long the
 * direction has been held. Ramps from 1 (fine) to a range-proportional coarse
 * amount so a wide range crosses in ~1s of holding while a tap still nudges by
 * one. Pure function of the descriptor and elapsed time — unit-tested. */
static long HoldStepMultiplier(const SettingDesc *desc, Uint64 held_ms) {
  long base = desc->step > 0 ? desc->step : 1;
  long range = desc->maxval - desc->minval;
  if (range <= 0) return 1;
  long coarse_units = NiceStep(range / 24);
  long coarse_mult = coarse_units / base;
  if (coarse_mult < 1) coarse_mult = 1;
  if (held_ms < (Uint64)kHoldInitialDelayMs + 700) return 1;
  if (held_ms < (Uint64)kHoldInitialDelayMs + 1700) {
    long mid = coarse_mult / 4;
    return mid < 1 ? 1 : mid;
  }
  return coarse_mult;
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
      desc->type == kSettingType_Binding ||
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

static void CancelCapture(void) {
  if (!s_capture_desc) return;
  s_capture_desc = NULL;
  SetStatus("BIND CANCELLED");
}

static void BeginCapture(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || desc->type != kSettingType_Binding) return;
  InputClass klass;
  if (!InputMap_DescribeRow(desc, NULL, &klass)) return;
  s_capture_desc = desc;
  SetStatus(klass == kInputClass_Keyboard ? "PRESS A KEY - ESC CANCELS"
                                          : "PRESS A BUTTON - ESC CANCELS");
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

/* Int rows are adjusted entirely by stepping (with hold-to-accelerate); they
 * never open the text editor. Mask/Custom rows are the genuine non-numeric
 * holdouts — a hex layer mask, arbitrary PAR pins, a player name — and keep
 * text entry.
 *
 * Apply one value step of `multiplier` base-steps. Live every call; persisted
 * immediately when `persist` (a tap or single press), otherwise deferred to
 * the end of the hold via s_hold_dirty. */
static void StepNumeric(const SettingDesc *desc, int direction,
                        long multiplier, bool persist) {
  long value = 0;
  if (!Settings_GetLong(desc, &value)) return;
  /* The scale rows use 0 as a "follow the auto value" sentinel; step off that
   * resolved number so the first press moves relative to what is on screen. */
  if (value == 0 && desc->field == &g_settings.menu_scale_percent)
    value = s_auto_menu_scale_percent;
  if (value == 0 && desc->field == &g_settings.hud_scale_percent)
    value = s_match_game_scale_percent;
  long step = desc->step > 0 ? desc->step : 1;
  long next = value + (long)direction * step * multiplier;
  SettingChangeResult result = Settings_SetLong(desc, next);
  if (persist) {
    SaveAcceptedChange(result);
  } else if (result > kSettingChange_Unchanged) {
    s_hold_dirty = true;
    s_hold_result = result;
  }
}

/* Begin (or, on a re-press of the same direction, continue) a held step. The
 * idempotent guard matters for the analog stick, whose held deflection can
 * re-emit press edges — restarting would keep resetting the acceleration ramp
 * to its slowest tier. */
static void BeginValueHold(const SettingDesc *desc, int direction,
                           SDL_Keycode key) {
  if (s_hold_desc == desc && s_hold_dir == direction) return;
  EndValueHold();
  s_hold_desc = desc;
  s_hold_dir = direction;
  s_hold_key = key;
  s_hold_start_ms = SDL_GetTicks();
  s_hold_next_ms = s_hold_start_ms + kHoldInitialDelayMs;
  s_hold_dirty = false;
  s_hold_result = kSettingChange_Applied;
  StepNumeric(desc, direction, 1, false);  /* immediate fine step */
}

static void ChangeSelectedValue(int direction) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  switch (desc->type) {
    case kSettingType_Action:
      if (direction > 0) InvokeSelectedAction();
      return;
    case kSettingType_Binding:
      BeginCapture();
      return;
    case kSettingType_Mask:
    case kSettingType_Custom:
      BeginEditing();
      return;
    case kSettingType_Int:
      BeginValueHold(desc, direction, s_input_key);
      return;
    case kSettingType_Bool:
    case kSettingType_Enum: {
      long value = 0;
      if (!Settings_GetLong(desc, &value)) {
        SetStatus("EDIT IN SETTINGS.INI");
        return;
      }
      long next;
      if (desc->type == kSettingType_Bool) {
        next = !value;
      } else {
        long step = desc->step > 0 ? desc->step : 1;
        next = value + (direction < 0 ? -step : step);
        if (next < desc->minval) next = desc->maxval;
        if (next > desc->maxval) next = desc->minval;
      }
      SaveAcceptedChange(Settings_SetLong(desc, next));
      return;
    }
  }
}

static void ActivateSelectedRow(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus("UNAVAILABLE HERE");
    return;
  }
  switch (desc->type) {
    case kSettingType_Action:  InvokeSelectedAction(); break;
    case kSettingType_Binding: BeginCapture(); break;
    case kSettingType_Mask:
    case kSettingType_Custom:  BeginEditing(); break;
    /* Confirm on a numeric row is a single fine step up, not a text prompt —
     * a discrete nudge with no hold, so it saves immediately. */
    case kSettingType_Int:     StepNumeric(desc, +1, 1, true); break;
    case kSettingType_Bool:
    case kSettingType_Enum:    ChangeSelectedValue(1); break;
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

static void MoveSection(int direction) {
  EndValueHold();
  s_section = (s_section + direction + kSectionCount) % kSectionCount;
  s_row = 0;
  s_top_row = 0;
  s_tab_scroll = 0;
  SyncActiveTabPage();
  EnsureSelectedNavVisible();
}

static void EnsureSelectedRowVisible(void) {
  int count = TabRowCount();
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
  int count = TabRowCount();
  if (count <= 0) return;
  EndValueHold();
  s_row = (s_row + direction + count) % count;
  EnsureSelectedRowVisible();
}

/* Tabs wrap, like every other list in this menu. Changing tab always resets
 * the row cursor: the two lists have nothing in common, so carrying an index
 * across would land somewhere arbitrary. */
static void MoveTab(int direction) {
  const MenuSection *section = ActiveSection();
  if (VisibleTabCount(s_section) <= 1) return;
  EndValueHold();
  int candidate = ActiveTabIndex();
  for (int i = 0; i < section->tab_count; i++) {
    candidate = (candidate + direction + section->tab_count) %
                section->tab_count;
    if (!RawTabHidden(s_section, candidate)) break;
  }
  s_tab[s_section] = candidate;
  s_row = 0;
  s_top_row = 0;
  StopEditing();
  s_capture_desc = NULL;
  SyncActiveTabPage();
  EnsureSelectedRowVisible();
}

static void EnterSection(void) {
  s_submenu_open = true;
  s_row = 0;
  s_top_row = 0;
  SyncActiveTabPage();
  EnsureSelectedRowVisible();
}

bool SettingsOverlay_Init(SDL_Renderer *renderer,
                          const uint8_t *rom_data, size_t rom_size) {
  s_renderer = renderer;
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
  s_debug_font_texture = CreateDebugFontAtlas();
  if (!s_debug_font_texture) {
    DestroyFontTextures();
    return false;
  }
  /* Host-authored section icons, independent of which text font loaded. */
  s_icon_texture = CreateIconAtlas();
  if (!s_icon_texture) {
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
  SDL_DestroyTexture(s_icon_texture);
  s_icon_texture = NULL;
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
  EndValueHold();
  s_capture_desc = NULL;
  s_submenu_open = false;
  s_open = true;
  SyncActiveTabPage();
  s_status[0] = 0;
  fprintf(stderr, "[settings-menu] opened\n");
}

void SettingsOverlay_Close(void) {
  if (!s_open) return;
  StopEditing();
  EndValueHold();
  s_capture_desc = NULL;
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
  EnsureSelectedNavVisible();
  if (selected_ordinal) *selected_ordinal = s_section;
  if (top_ordinal) *top_ordinal = s_nav_top_row;
  if (visible_rows) *visible_rows = s_nav_visible_rows;
  if (total_rows) *total_rows = NavPopulatedCount();
  return true;
}

bool SettingsOverlay_GetTabState(int *active_tab, int *tab_count) {
  if (!s_open) return false;
  /* Report positions among the VISIBLE tabs — hidden (all-debug) tabs are not
   * shown and cannot be navigated to, so a caller counting tabs must not see
   * them. */
  if (active_tab) *active_tab = ActiveVisibleTabPosition();
  if (tab_count) *tab_count = VisibleTabCount(s_section);
  return true;
}

/* Shared by the live tick and the test tick so the clock is the only
 * difference. Ends the hold if the row it was moving is no longer the target
 * of a plain held direction (navigated away, started editing, went
 * unavailable), otherwise fires every due repeat with the ramped magnitude. */
static void TickHold(Uint64 now_ms) {
  if (!s_open || !s_hold_desc) return;
  if (!s_submenu_open || s_editing || s_capture_desc ||
      SelectedDesc() != s_hold_desc || !Settings_IsAvailable(s_hold_desc)) {
    EndValueHold();
    return;
  }
  int guard = 0;
  while (now_ms >= s_hold_next_ms && guard++ < 8) {
    long multiplier = HoldStepMultiplier(s_hold_desc, now_ms - s_hold_start_ms);
    StepNumeric(s_hold_desc, s_hold_dir, multiplier, false);
    s_hold_next_ms += kHoldRepeatMs;
  }
  /* If a frame hitch left the schedule far in the past, resync rather than
   * firing a long catch-up burst on the next tick. */
  if (s_hold_next_ms + kHoldRepeatMs < now_ms)
    s_hold_next_ms = now_ms + kHoldRepeatMs;
}

void SettingsOverlay_Tick(void) {
  TickHold(SDL_GetTicks());
}

long SettingsOverlay_HoldStepForTest(const struct SettingDesc *desc,
                                     uint64_t held_ms) {
  return desc ? HoldStepMultiplier((const SettingDesc *)desc, held_ms) : 0;
}

void SettingsOverlay_TickAtForTest(uint64_t now_ms) {
  TickHold((Uint64)now_ms);
}

/* Logical menu commands. Both the keyboard path and the gamepad path funnel
 * through these so the two never drift apart, and so a rebound pad drives the
 * menu with the player's own buttons. */
typedef enum {
  kMenuNav_Up,
  kMenuNav_Down,
  kMenuNav_Left,
  kMenuNav_Right,
  kMenuNav_Confirm,
  kMenuNav_Back,     /* leave the submenu, or close from the nav column */
  kMenuNav_Reset,    /* restore the selected row's default */
  kMenuNav_TabPrev,  /* previous tab of the current section */
  kMenuNav_TabNext,
  kMenuNav_Close,
} MenuNav;

static void ApplyMenuNav(MenuNav nav, bool repeat) {
  if (!s_submenu_open) {
    switch (nav) {
      case kMenuNav_Up:      MoveSection(-1); break;
      case kMenuNav_Down:    MoveSection(1); break;
      /* Left/Right have nothing to edit out here, so they preview the
       * section's tabs — the tab bar is visible from the nav column, so a
       * player can pick the tab before ever entering. */
      case kMenuNav_Left:
      case kMenuNav_TabPrev: MoveTab(-1); break;
      case kMenuNav_Right:
      case kMenuNav_TabNext: MoveTab(1); break;
      case kMenuNav_Confirm:
        if (!repeat) EnterSection();
        break;
      case kMenuNav_Back:
      case kMenuNav_Close:
        if (!repeat) SettingsOverlay_Close();
        break;
      default:
        break;
    }
    return;
  }

  switch (nav) {
    case kMenuNav_Up:      MoveRow(-1); break;
    case kMenuNav_Down:    MoveRow(1); break;
    /* Ignore OS key-repeat on value change: a numeric row's repeats come from
     * SettingsOverlay_Tick (which paces and accelerates them), and a non-
     * numeric row should change once per physical press. */
    case kMenuNav_Left:    if (!repeat) ChangeSelectedValue(-1); break;
    case kMenuNav_Right:   if (!repeat) ChangeSelectedValue(1); break;
    case kMenuNav_TabPrev: MoveTab(-1); break;
    case kMenuNav_TabNext: MoveTab(1); break;
    case kMenuNav_Confirm: ActivateSelectedRow(); break;
    case kMenuNav_Reset:   ResetSelectedValue(); break;
    case kMenuNav_Back:
      if (!repeat) {
        EndValueHold();
        s_submenu_open = false;
      }
      break;
    case kMenuNav_Close:
      if (!repeat) SettingsOverlay_Close();
      break;
  }
}

bool SettingsOverlay_IsEditing(void) {
  return s_open && s_editing;
}

bool SettingsOverlay_IsCapturing(void) {
  return s_open && s_capture_desc != NULL;
}

bool SettingsOverlay_HandleCaptureEvent(const SDL_Event *event) {
  if (!SettingsOverlay_IsCapturing() || !event) return false;

  /* Escape always aborts, whatever device the row belongs to — otherwise a
   * keyboard row would swallow Escape as its own new binding. */
  if (event->type == SDL_EVENT_KEY_DOWN &&
      event->key.scancode == SDL_SCANCODE_ESCAPE) {
    CancelCapture();
    return true;
  }

  InputClass klass;
  if (!InputMap_DescribeRow(s_capture_desc, NULL, &klass)) {
    CancelCapture();
    return true;
  }
  /* Ignore events from the other device class so, for example, a stray
   * controller nudge cannot land in a keyboard row. */
  bool keyboard_event = event->type == SDL_EVENT_KEY_DOWN ||
                        event->type == SDL_EVENT_KEY_UP ||
                        event->type == SDL_EVENT_TEXT_INPUT;
  bool pad_event = event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                   event->type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                   event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
  if (!keyboard_event && !pad_event) return false;
  if (klass == kInputClass_Keyboard && !keyboard_event) return true;
  if (klass == kInputClass_Gamepad && !pad_event) return true;

  uint32 binding = 0;
  if (!InputMap_DecodeEvent(event, klass, &binding)) return true;

  const SettingDesc *desc = s_capture_desc;
  s_capture_desc = NULL;
  SaveAcceptedChange(InputMap_ApplyBinding(desc, binding));
  return true;
}

bool SettingsOverlay_HandleGamepadEvent(const SDL_Event *event) {
  if (!s_open || !event) return false;
  if (SettingsOverlay_HandleCaptureEvent(event)) return true;

  InputAction action;
  bool pressed = false;
  if (!InputMap_ActionForEvent(event, &action, &pressed)) return true;
  /* This dispatch is pad-sourced; a held value is released by the button/stick
   * edge, not a keyboard key. */
  s_input_key = 0;
  if (!pressed) {
    /* Releasing the held direction ends the accelerate-and-flush. */
    if (s_hold_desc && s_hold_key == 0 &&
        (action == kInputAction_Left || action == kInputAction_Right))
      EndValueHold();
    return true;
  }

  /* A text-entry field cannot be typed into with a pad; the two edge cases
   * that still make sense there are commit and cancel. */
  if (s_editing) {
    if (action == kInputAction_B) CommitEditing();
    else if (action == kInputAction_A) StopEditing();
    return true;
  }

  switch (action) {
    case kInputAction_Up:     ApplyMenuNav(kMenuNav_Up, false); break;
    case kInputAction_Down:   ApplyMenuNav(kMenuNav_Down, false); break;
    case kInputAction_Left:   ApplyMenuNav(kMenuNav_Left, false); break;
    case kInputAction_Right:  ApplyMenuNav(kMenuNav_Right, false); break;
    case kInputAction_B:      ApplyMenuNav(kMenuNav_Confirm, false); break;
    case kInputAction_A:      ApplyMenuNav(kMenuNav_Back, false); break;
    case kInputAction_Y:      ApplyMenuNav(kMenuNav_Reset, false); break;
    /* Shoulders page the tab bar — the same idiom as the system menus on
     * every console this build targets, and free on a Deck. */
    case kInputAction_L:      ApplyMenuNav(kMenuNav_TabPrev, false); break;
    case kInputAction_R:      ApplyMenuNav(kMenuNav_TabNext, false); break;
    case kInputAction_Menu:
    case kInputAction_Start:  ApplyMenuNav(kMenuNav_Close, false); break;
    default: break;
  }
  return true;
}

/* True when `key` is the keyboard binding the player assigned to `action`.
 * Bindings store scancodes, so translate the keycode first (NULL modstate:
 * menu control is layout-position based, like the game input path). */
static bool MenuKeyMatchesBinding(SDL_Keycode key, InputAction action) {
  uint32 binding = g_settings.input_bind[kInputClass_Keyboard][action];
  if (INPUT_BIND_KIND(binding) != kInputBind_Key) return false;
  return SDL_GetScancodeFromKey(key, NULL) == INPUT_BIND_CODE(binding);
}

/* Maps a keycode to a menu command through the player's OWN keyboard bindings,
 * so rebinding B/A/Y/L/R or a direction moves those controls in the menu too —
 * the hint line names them by SNES button, and this is what makes that promise
 * true. It mirrors the gamepad path (SettingsOverlay_HandleGamepadEvent), with
 * one deliberate exception: Start/Select/Menu are NOT mapped here. Their
 * keyboard defaults collide with the universal keyboard conventions the menu
 * keeps (Start defaults to Return, which the menu already uses to confirm), so
 * on a keyboard the conventions win and Esc/Enter own open-close instead.
 * Returns false when the key is not one of these bound controls. */
static bool MenuNavForBoundKey(SDL_Keycode key, MenuNav *out) {
  static const struct {
    InputAction action;
    MenuNav nav;
  } kMap[] = {
    { kInputAction_Up,    kMenuNav_Up },
    { kInputAction_Down,  kMenuNav_Down },
    { kInputAction_Left,  kMenuNav_Left },
    { kInputAction_Right, kMenuNav_Right },
    { kInputAction_B,     kMenuNav_Confirm },
    { kInputAction_A,     kMenuNav_Back },
    { kInputAction_Y,     kMenuNav_Reset },
    { kInputAction_L,     kMenuNav_TabPrev },
    { kInputAction_R,     kMenuNav_TabNext },
  };
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++)
    if (MenuKeyMatchesBinding(key, kMap[i].action)) {
      *out = kMap[i].nav;
      return true;
    }
  return false;
}

bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat) {
  if (!s_open) return false;
  if (key == SDLK_F2) return false;
  if (!pressed) {
    /* Releasing the key that began a held step ends it and flushes the write. */
    if (s_hold_key && key == s_hold_key) EndValueHold();
    return true;
  }
  /* Dispatch is keyboard-sourced; a held step started now is released by this
   * same key coming up. */
  s_input_key = key;
  /* Capture is fed raw events by main.c (SettingsOverlay_HandleCaptureEvent)
   * because a scancode, not a keycode, is what gets bound. */
  if (s_capture_desc) return true;

  if (s_editing) {
    /* While typing a value, letter keys must reach the text buffer
     * (SettingsOverlay_HandleText), so this path handles ONLY the fixed edit
     * controls and maps no SNES button — Esc cancels, Enter commits,
     * Backspace deletes. A bound key like the default A-cancel would otherwise
     * eat that letter mid-value (a player name cannot contain 'x'). */
    switch (key) {
      case SDLK_ESCAPE:
        StopEditing();
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

  switch (key) {
    /* Universal keyboard controls, deliberately fixed and independent of the
     * game bindings: arrows navigate, Enter confirms, Esc/F1 close, [/]+Tab
     * cycle tabs. These alone fully operate the menu, which matters because
     * the menu is the only place to repair a broken binding — so it must stay
     * usable even if the player has unbound or mangled their SNES keys. */
    case SDLK_ESCAPE:
    case SDLK_F1:
      ApplyMenuNav(kMenuNav_Close, repeat);
      break;
    case SDLK_UP:
      ApplyMenuNav(kMenuNav_Up, repeat);
      break;
    case SDLK_DOWN:
      ApplyMenuNav(kMenuNav_Down, repeat);
      break;
    case SDLK_LEFT:
      ApplyMenuNav(kMenuNav_Left, repeat);
      break;
    case SDLK_RIGHT:
      ApplyMenuNav(kMenuNav_Right, repeat);
      break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      ApplyMenuNav(kMenuNav_Confirm, repeat);
      break;
    case SDLK_LEFTBRACKET:
      ApplyMenuNav(kMenuNav_TabPrev, repeat);
      break;
    case SDLK_RIGHTBRACKET:
    case SDLK_TAB:
      ApplyMenuNav(kMenuNav_TabNext, repeat);
      break;
    default: {
      /* Everything the hint line labels by SNES button — B confirm, A back,
       * Y reset, L/R tab, and the directions — follows the player's own
       * keyboard bindings (default Z/X/A, Q/W, arrows), so a rebind moves the
       * menu control with it. */
      MenuNav nav;
      if (MenuNavForBoundKey(key, &nav)) ApplyMenuNav(nav, repeat);
      break;
    }
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

/* ── Small font ─────────────────────────────────────────────────────────
 * The 6x8 atlas built from kFallbackFont, drawn with a free-form color via
 * SetTextureColorMod. It is monochrome (no baked outline/shadow), so unlike
 * the 8x8 ROM font it can take an arbitrary color at zero cost — which is
 * what lets the tab bar, description panel, and hint line pick up each
 * section's accent instead of everything being the same white.
 *
 * Three quarters the width of the ROM font per character, so the description
 * panel fits roughly a third more text per line at a size that still reads
 * comfortably at couch distance. */
static void DrawSmallGlyph(const MenuLayout *layout, int x, int y,
                           unsigned char ch) {
  if (ch == ' ' || !s_debug_font_texture) return;
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

static void DrawSmallTextN(const MenuLayout *layout, int x, int y,
                           const char *text, int max_chars, uint32_t color) {
  if (!text || max_chars <= 0 || !s_debug_font_texture) return;
  SDL_SetTextureColorMod(s_debug_font_texture, (Uint8)(color >> 16),
                         (Uint8)(color >> 8), (Uint8)color);
  SDL_SetTextureAlphaMod(s_debug_font_texture, (Uint8)(color >> 24));
  for (int i = 0; text[i] && i < max_chars; i++)
    DrawSmallGlyph(layout, x + i * kDebugGlyphWidth, y,
                   (unsigned char)text[i]);
}

static void DrawSmallText(const MenuLayout *layout, int x, int y,
                          const char *text, uint32_t color) {
  DrawSmallTextN(layout, x, y, text, 512, color);
}

static int SmallTextWidth(const char *text) {
  return text ? (int)strlen(text) * kDebugGlyphWidth : 0;
}

/* Icons are authored at 16x16 but drawn at whatever `size` the caller wants;
 * nearest-neighbour keeps integer multiples crisp. `selected` picks the
 * colored game palette (the highlighted slot) over the grey one, and `alpha`
 * fades an unselected, un-focused nav row so it reads as recessive. */
static void DrawSectionIcon(const MenuLayout *layout, int x, int y, int size,
                            int section, bool selected, int alpha) {
  if (!s_icon_texture || section < 0 || section >= kSectionCount) return;
  SDL_SetTextureColorMod(s_icon_texture, 255, 255, 255);
  SDL_SetTextureAlphaMod(s_icon_texture, (Uint8)alpha);
  SDL_FRect source = {
    (float)(section * kIconSize), selected ? (float)kIconSize : 0.0f,
    (float)kIconSize, (float)kIconSize,
  };
  SDL_FRect destination = ToFRect(LogicalRect(layout, x, y, size, size));
  SDL_RenderTexture(s_renderer, s_icon_texture, &source, &destination);
}

/* A slim track with a proportional thumb, drawn in the panel's inner gutter.
 * Replaces the pair of blinking ^ / v glyphs the lists used to carry: those
 * cost a full 8px text cell out of the value column and only said "there is
 * more", never how much more or where you are in it. Draws nothing when the
 * whole list already fits. */
static void DrawScrollBar(const MenuLayout *layout, int x, int y, int height,
                          int total, int visible, int top, uint32_t accent) {
  if (total <= visible || visible <= 0 || height <= 0) return;
  FillLogicalRect(layout, x, y, 3, height, ARGB(90, 60, 84, 106));
  int thumb = height * visible / total;
  if (thumb < 6) thumb = 6;
  if (thumb > height) thumb = height;
  int span = total - visible;
  int offset = span > 0 ? (height - thumb) * top / span : 0;
  FillLogicalRect(layout, x, y + offset, 3, thumb, accent);
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

/* Word-wrapped small-font paragraph. Returns the number of lines drawn so a
 * caller can place something underneath. The description panel uses this: at
 * 6px per character it fits the longest tooltips in the table without the
 * truncation the 8px menu font used to force. */
static int DrawWrappedSmallText(const MenuLayout *layout, int x, int y,
                                const char *text, int max_chars,
                                int max_lines, uint32_t color) {
  const char *cursor = text;
  if (max_chars > 127) max_chars = 127;
  int line = 0;
  for (; line < max_lines && cursor && *cursor; line++) {
    int length = (int)strlen(cursor);
    if (length > max_chars) {
      length = max_chars;
      while (length > 1 && cursor[length] != ' ') length--;
      if (length <= 1) length = max_chars;
    }
    char buffer[128];
    memcpy(buffer, cursor, (size_t)length);
    buffer[length] = 0;
    DrawSmallText(layout, x, y + line * kSmallLineHeight, buffer, color);
    cursor += length;
    while (*cursor == ' ') cursor++;
  }
  return line;
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
    DrawDebugTextN(layout, x, y + row * kSmallLineHeight, line, length,
                   kDebugText_Normal);
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
  const int left_width = 152;
  const int bottom_height = 72;
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
  DrawDialogPanel(layout, margin, bottom_y, bottom_width, bottom_height);

  const MenuSection *section = ActiveSection();
  /* The section accent tints only its 16x16 icon (baked into the atlas); all
   * chrome here is the shared game steel-blue, and the cursor/selection is the
   * game's menu yellow. */
  const uint32_t structure = kSteelBlue;
  const uint32_t structure_dim = kSteelDim;
  SyncActiveTabPage();

  /* SDL3 removed SDL_TICKS_PASSED; SDL_GetTicks is now 64-bit and never wraps
   * in practice, so a direct comparison is exact. */
  if (s_status[0] && SDL_GetTicks() >= s_status_until)
    s_status[0] = 0;

  /* ── Nav column ──────────────────────────────────────────────────────── */
  const int left_text_x = left_x + 10;
  const int left_title_y = top_y + 10;
  const int nav_first_y = top_y + 18;

  DrawSmallText(layout, left_text_x, left_title_y - 3, "SYSTEM SETTINGS",
                ARGB(255, 132, 154, 174));
  FillLogicalRect(layout, left_text_x, left_title_y + 6,
                  left_width - 20, 1, ARGB(120, 120, 150, 178));

  s_nav_visible_rows =
      (top_y + top_height - 6 - nav_first_y) / kNavRowHeight;
  if (s_nav_visible_rows < 1) s_nav_visible_rows = 1;
  EnsureSelectedNavVisible();

  for (int slot = 0; slot < kSectionCount; slot++) {
    if (slot < s_nav_top_row ||
        slot >= s_nav_top_row + s_nav_visible_rows)
      continue;
    int row_y = nav_first_y + (slot - s_nav_top_row) * kNavRowHeight;
    bool current = slot == s_section;
    /* The selected section keeps a tinted plate even after the player has
     * moved focus into the submenu, so the right-hand panel never looks
     * orphaned from the nav column. */
    if (current)
      FillLogicalRect(layout, left_x + 6, row_y - 1, left_width - 12,
                      kNavRowHeight - 2,
                      s_submenu_open ? ARGB(90, 32, 56, 78) : kHighlight);
    if (current && !s_submenu_open)
      FillLogicalRect(layout, left_x + 6, row_y - 1, 2, kNavRowHeight - 2,
                      kSelectYellow);
    /* The selected section lights up in the game's colored slot palette; the
     * rest stay grey, and a nav row that is not the current one dims slightly
     * so the cursor reads at a glance. */
    DrawSectionIcon(layout, left_text_x, row_y, kIconSize, slot,
                    current, current ? 255 : 205);
    DrawTextN(layout, left_text_x + kIconSize + 4, row_y + 4,
              kSections[slot].label, 11,
              current ? kText_Normal : kText_Dim);
  }
  DrawScrollBar(layout, left_x + left_width - 12, nav_first_y,
                s_nav_visible_rows * kNavRowHeight, kSectionCount,
                s_nav_visible_rows, s_nav_top_row, structure);

  /* ── Submenu header: section title, status, tab bar ───────────────────── */
  const int right_text_x = right_x + 12;
  const int right_title_y = top_y + 8;
  /* The value column stops short of the frame so the scrollbar has a gutter
   * of its own instead of overlapping a value. */
  const int value_right = right_x + right_width - 16;
  const int scroll_x = right_x + right_width - 13;

  /* The submenu header is always the active section, so its icon takes the
   * colored selected palette. */
  DrawSectionIcon(layout, right_text_x, right_title_y - 2, kIconSize,
                  s_section, true, 255);
  DrawTextN(layout, right_text_x + kIconSize + 6, right_title_y,
            section->label, 12, kText_Normal);
  if (s_status[0]) {
    int status_chars = (right_width - 32) / kDebugGlyphWidth;
    if (status_chars > 40) status_chars = 40;
    int status_length = CappedTextLength(s_status, status_chars);
    DrawSmallTextN(layout,
                   value_right - status_length * kDebugGlyphWidth,
                   right_title_y + 1, s_status, status_length, kGameGold);
  }

  /* A section with a single VISIBLE tab draws no strip at all — a lone
   * highlighted chip would read as a control the player can act on, and it
   * would spend a row's worth of height saying nothing. Hidden (all-debug)
   * tabs are skipped, so with debug settings off Town 3D shows Scene/Camera
   * and System shows no strip. */
  const int visible_tabs = VisibleTabCount(s_section);
  const int active_tab = ActiveTabIndex();
  const int tab_y = right_title_y + 13;
  int rule_y = right_title_y + 12;
  if (visible_tabs > 1) {
    /* Gather the visible tabs, their widths, and the active one's position so
     * a section with more tabs than fit (Save's six pages) can scroll the
     * strip to keep the active tab on screen. */
    int vis[64], vwidth[64], vcount = 0, apos = 0;
    for (int tab = 0; tab < section->tab_count; tab++) {
      if (RawTabHidden(s_section, tab) || vcount >= 64) continue;
      if (tab == active_tab) apos = vcount;
      vis[vcount] = tab;
      vwidth[vcount] = SmallTextWidth(section->tabs[tab].label) + 8;
      vcount++;
    }

    const int chevron = kDebugGlyphWidth;
    /* The strip lives between the "L" and "R" button letters. */
    const int strip_x0 = right_text_x + chevron + 5;
    const int strip_x1 = value_right - chevron - 3;

    int total = 0;
    for (int i = 0; i < vcount; i++) total += vwidth[i] + 2;
    bool overflow = total > strip_x1 - strip_x0;
    /* When scrolling, reserve a chevron on each side of the strip. */
    int inner_x0 = strip_x0 + (overflow ? chevron : 0);
    int inner_x1 = strip_x1 - (overflow ? chevron : 0);

    if (!overflow) s_tab_scroll = 0;
    if (s_tab_scroll > apos) s_tab_scroll = apos;
    if (s_tab_scroll < 0) s_tab_scroll = 0;
    /* Shift right until the active tab fits from the current scroll start. */
    while (s_tab_scroll < apos) {
      int span = 0;
      for (int i = s_tab_scroll; i <= apos; i++) span += vwidth[i] + 2;
      if (span <= inner_x1 - inner_x0) break;
      s_tab_scroll++;
    }

    DrawSmallText(layout, right_text_x, tab_y + 2, "L", kSteelDim);
    if (overflow && s_tab_scroll > 0)
      DrawSmallText(layout, strip_x0, tab_y + 2, "<", kSelectYellow);

    int tab_x = inner_x0;
    int last_shown = s_tab_scroll - 1;
    for (int i = s_tab_scroll; i < vcount; i++) {
      if (tab_x + vwidth[i] > inner_x1) break;
      bool current = vis[i] == active_tab;
      /* The active tab is the cursor's position among the tabs, so it takes
       * the same menu yellow as the selected row/section. */
      if (current) {
        FillLogicalRect(layout, tab_x, tab_y - 2, vwidth[i], 11,
                        ScaleColor(kSelectYellow, 20));
        FillLogicalRect(layout, tab_x, tab_y + 9, vwidth[i], 1, kSelectYellow);
      }
      DrawSmallText(layout, tab_x + 4, tab_y + 1, section->tabs[vis[i]].label,
                    current ? kSelectYellow : kMutedText);
      tab_x += vwidth[i] + 2;
      last_shown = i;
    }
    if (overflow && last_shown < vcount - 1)
      DrawSmallText(layout, inner_x1 + 2, tab_y + 2, ">", kSelectYellow);
    DrawSmallText(layout, value_right - chevron + 2, tab_y + 2, "R", kSteelDim);
    rule_y = tab_y + 13;
  }
  /* Accent rule under the header ties the title, tabs, and row list into one
   * section-colored block. */
  FillLogicalRect(layout, right_x + 10, rule_y, right_width - 20, 1,
                  structure_dim);

  /* ── Rows ─────────────────────────────────────────────────────────────── */
  const int first_row_y = rule_y + 6;
  const int selector_x = right_x + 12;
  const int label_x = right_x + 22;
  /* Save-state/item labels need up to 18 characters (for example
   * "Act 2 cleared" and "Strength of Angel"). Label width is computed per row
   * from the actual formatted value, so short values such as RUN do not waste
   * the rest of that reservation. */
  const int value_chars = 18;

  s_visible_rows = (top_y + top_height - 6 - first_row_y) / kRowHeight;
  if (s_visible_rows < 1) s_visible_rows = 1;
  EnsureSelectedRowVisible();

  const SettingCategory category = ActiveTab()->category;
  int row_index = 0;
  int drawn_rows = 0;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (!RowBelongsToActiveTab(desc)) continue;
    int row = row_index++;
    if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
    drawn_rows++;
    int y = first_row_y + (row - s_top_row) * kRowHeight;
    /* Commands are separated from the settings they act on. */
    if (category == kSettingCat_Save &&
        !strcmp(desc->key, "save_apply_session"))
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                      structure_dim);
    if (category == kSettingCat_Extras && !strcmp(desc->key, "restart_game"))
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                      ARGB(160, 190, 96, 76));
    bool selected = s_submenu_open && row == s_row;
    bool available = Settings_IsAvailable(desc);
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11,
                      kHighlight);
      FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
      DrawGlyph(layout, selector_x + ((SDL_GetTicks() / 250) & 1), y, '>',
                kText_Warning);
    }
    TextStyle style = available && s_submenu_open
        ? kText_Normal : kText_Dim;

    char value[512];
    if (selected && desc == s_capture_desc) {
      /* Blink so an armed row is unmistakable — on a Deck the status line at
       * the bottom of the panel is easy to miss mid-rebind. */
      snprintf(value, sizeof(value), "%s",
               (SDL_GetTicks() / 300) & 1 ? "PRESS..." : "");
    } else if (selected && s_editing) {
      snprintf(value, sizeof(value), "%s", s_edit_buffer);
    } else {
      Settings_FormatValue(desc, value, sizeof(value));
    }
    if (!value[0] && desc != s_capture_desc)
      snprintf(value, sizeof(value), "CUSTOM");
    if (desc->field == &g_settings.display_mode) {
      static const char *const short_modes[] = {
        "4:3 AUTH", "WIDE RAW", "WIDE FULL", "CUSTOM"
      };
      int mode = g_settings.display_mode;
      if (mode >= 0 &&
          mode < (int)(sizeof(short_modes) / sizeof(short_modes[0])))
        snprintf(value, sizeof(value), "%s", short_modes[mode]);
    }
    /* Vsync locks to the display, so name the display's actual refresh rate
     * rather than the bare word "Vsync". Purely display-side (the saved value
     * stays the plain enum label). Unknown Hz falls back to "Vsync". */
    if (desc->field == &g_settings.refresh_mode &&
        g_settings.refresh_mode == kRefreshMode_Vsync) {
      int hz = Settings_HostRefreshHz();
      if (hz > 0)
        snprintf(value, sizeof(value), "Vsync %dHz", hz);
    }
    int shown_value_chars = CappedTextLength(value, value_chars);
    int row_value_left = value_right - shown_value_chars * kGlyphSize;
    int restart_x = row_value_left - 12;
    int label_chars = (restart_x - label_x - 4) / kGlyphSize;
    if (label_chars < 1) label_chars = 1;
    DrawTextN(layout, label_x, y, desc->label, label_chars, style);
    /* M2 (followup doc): values render in kText_Value (cool cyan) so they
     * read distinct from labels, but only for normal/enabled rows — a
     * dim/unavailable row's style must win so it stays visibly greyed. */
    DrawTextRight(layout, value_right, y, value, value_chars,
                  style == kText_Normal ? kText_Value : style);
    if (desc->apply == kApply_Restart)
      DrawGlyph(layout, restart_x, y, '*', kText_Warning);
  }

  DrawScrollBar(layout, scroll_x, first_row_y - 2,
                s_visible_rows * kRowHeight, row_index, s_visible_rows,
                s_top_row, structure);

  if (row_index == 0)
    DrawSmallText(layout, right_text_x, first_row_y + 2,
                  "Nothing to configure on this tab.", kMutedText);

  if (category == kSettingCat_Inspector) {
    int info_y = first_row_y + drawn_rows * kRowHeight + 5;
    FillLogicalRect(layout, right_x + 12, info_y - 4, right_width - 24, 1,
                    structure_dim);
    DrawSmallText(layout, right_text_x, info_y, "LIVE SCENE", structure);
    DrawInspectorInfo(layout, right_text_x, info_y + 11,
                      (right_width - 24) / kDebugGlyphWidth, 6);
  }

  /* ── Description panel ────────────────────────────────────────────────── */
  const int description_x = margin + 12;
  const int description_chars = (bottom_width - 24) / kDebugGlyphWidth;
  const SettingDesc *selected = s_submenu_open ? SelectedDesc() : NULL;
  const int header_y = bottom_y + 8;
  if (selected) {
    DrawSmallText(layout, description_x, header_y, selected->label, structure);
    /* Naming HOW a change takes effect next to the row removes the usual
     * "did that do anything?" question; the '*' row marker only says that a
     * restart is involved, not what the other kinds do. */
    const char *apply = Settings_ApplyKindName(selected->apply);
    uint32_t apply_color = selected->apply == kApply_Restart
        ? kGameGold : kMutedText;
    if (!Settings_IsAvailable(selected)) {
      apply = "Unavailable in this mode";
      apply_color = ARGB(255, 246, 49, 49);  /* menu red */
    }
    DrawSmallTextN(layout,
                   panel_right - 12 - SmallTextWidth(apply), header_y,
                   apply, description_chars, apply_color);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         selected->tooltip, description_chars, 4,
                         ARGB(255, 208, 220, 232));
  } else {
    DrawSmallText(layout, description_x, header_y, section->label, structure);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         section->blurb, description_chars, 4,
                         ARGB(255, 208, 220, 232));
  }

  /* ── Hint line ────────────────────────────────────────────────────────── */
  static const uint32_t kKeyColor = ARGB(255, 146, 200, 244);
  static const uint32_t kHintColor = ARGB(255, 112, 132, 150);
  /* Flat key/label pairs. Key names take the bright color so the line reads
   * as controls rather than as one more grey sentence. */
  const char *hints[14];
  int hint_count = 0;
#define HINT(key, text) do { \
    hints[hint_count++] = (key); hints[hint_count++] = (text); \
  } while (0)
  if (s_capture_desc) {
    HINT("ANY KEY", "bind");
    HINT("ESC", "cancel");
  } else if (s_editing) {
    HINT("RETURN", "apply");
    HINT("A/ESC", "cancel");
  } else if (s_submenu_open) {
    HINT("UP/DOWN", "select");
    /* The verbs track what the selected row actually does: an Int row adjusts
     * (hold to accelerate — felt, not spelled out, to keep the line short),
     * a string/mask row opens a text prompt, the rest cycle. "adjust" is the
     * same width as "change", so this never widens the line. */
    const SettingDesc *row = SelectedDesc();
    bool numeric = row && row->type == kSettingType_Int;
    bool textual = row && (row->type == kSettingType_Mask ||
                           row->type == kSettingType_Custom);
    HINT("LEFT/RIGHT", numeric ? "adjust" : "change");
    if (VisibleTabCount(s_section) > 1) HINT("L/R", "tab");
    if (textual) HINT("B", "type");
    HINT("Y", "reset");
    HINT("A", "back");
  } else {
    HINT("UP/DOWN", "section");
    if (VisibleTabCount(s_section) > 1) HINT("L/R", "tab");
    HINT("B", "open");
    HINT("A", "close");
  }
#undef HINT
  int hint_x = description_x;
  const int hint_y = bottom_y + bottom_height - 13;
  for (int i = 0; i + 1 < hint_count; i += 2) {
    DrawSmallText(layout, hint_x, hint_y, hints[i], kKeyColor);
    hint_x += SmallTextWidth(hints[i]) + 5;
    DrawSmallText(layout, hint_x, hint_y, hints[i + 1], kHintColor);
    hint_x += SmallTextWidth(hints[i + 1]) + 11;
  }
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
