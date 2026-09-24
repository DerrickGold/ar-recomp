#include "settings_overlay.h"

#include "localization/unicode_grapheme.h"
#include "localization/interface_text.h"
#include "settings_overlay_internal.h"
#include "settings_overlay_artwork.h"
#include "settings_overlay_palette.h"
#include "settings_overlay_localization.h"
#include "settings_overlay_layers_localization.h"
#include "settings_overlay/regional/regional_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "diorama/diorama_layer_editor.h"
#include "action/action_bg_tuner.h"
#include "host/host_clock.h"
#include "input_map.h"
#include "render/render_output.h"
#include "render/ui_text_renderer.h"
#include "settings.h"
#include "user_data_dir.h"

enum {
  kRowHeight = 13,
  kMinimumLayoutWidth = 464,
  kMinimumLayoutHeight = 208,
  kMinimumScalePercent = 25,
  kMaximumScalePercent = 800,
  kScaleStepPercent = 25,
  kMatchGameMaximumScalePercent = 400,
  /* Nav rows are as tall as a section icon; the eight sections then fill the
   * column without scrolling at any ordinary window size. */
  kNavRowHeight = 17,
  kSmallLineHeight = 9,
  kTabBarHeight = 12,
  /* Hold-to-accelerate timing. The initial delay is what separates a tap
   * (single fine step) from a hold; after it, a step fires every interval. */
  kHoldInitialDelayMs = 350,
  kHoldRepeatMs = 55,
  kCursorBlinkHalfPeriodMs = 250,
};

/* ARGB() is defined in settings_overlay_internal.h (shared with the panel). */

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
const uint32_t kSteelBlue = ARGB(255, 164, 196, 219);
static const uint32_t kSteelDim = ARGB(255, 74, 104, 130);
const uint32_t kSelectYellow = ARGB(255, 255, 230, 0);
const uint32_t kGameGold = ARGB(255, 255, 180, 65);
static const uint32_t kQualityOfLifeBlue = ARGB(255, 156, 205, 255);
const uint32_t kMutedText = ARGB(255, 120, 140, 158);
static ArUiTextRenderer s_ui_text;

static ArUiLocale InterfaceLocale(void) {
  /* A port without a ready text backend must keep its menu readable. The
   * persisted choice survives so a later successful font setup can adopt it. */
  return ArUiTextRenderer_IsReady(&s_ui_text)
      ? (ArUiLocale)g_settings.interface_language : kArUiLocale_English;
}

ArUiLocale SettingsOverlay_InterfaceLocale(void) { return InterfaceLocale(); }

static const char *Ui(const char *key) {
  return ArUiCatalog_Text(InterfaceLocale(), key, key);
}

static void FormatSectionMessage(char *out, size_t capacity, const char *key,
                                  const char *section_key) {
  ArUiTextArgument arg = {"section", Ui(section_key)};
  if (!ArUiCatalog_Format(out, capacity, Ui(key), &arg, 1) && capacity) out[0] = 0;
}

/* DebugTextStyle is defined in settings_overlay_internal.h (shared with the
 * panel, which passes these roles to DrawDebugTextN). */

/* The scene inspector is a host debugger, not an in-game dialog. Keep its
 * information hierarchy legible independently of the ROM font palette. */
static const uint32_t kDebugTextColors[kDebugTextStyle_Count] = {
  ARGB(255, 218, 229, 238), /* ordinary punctuation/text */
  ARGB(255,  92, 196, 255), /* field names */
  ARGB(255, 255, 207,  92), /* addresses and numeric values */
  ARGB(255, 105, 232, 157), /* BG/OBJ targets and source policies */
  ARGB(255, 255, 145,  76), /* missing candidates and warnings */
  ARGB(255, 119, 139, 154), /* controls and explanatory notes */
};

/* ── Sections and tabs (M1(b), followup doc) ─────────────────────────────
 * The nav column used to list one row per SettingCategory, which meant 13
 * rows of near-synonyms (Display / Diorama / Simulation / Graphics /
 * Widescreen were all "how the game looks") and two categories that alone
 * carried 45 and 52 rows. Navigation is now two levels:
 *
 *   SECTION  — what the nav column lists. Each has a 16x16
 *              game menu icon (grey when unselected, the game's colored slot
 *              palette when current).
 *   TAB      — the horizontal strip at the top of the submenu, cycled with
 *              L/R (pad) or Q/E, [/], Tab (keyboard). One tab is one
 *              SettingCategory or an explicitly owned custom row list.
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
  bool regional_rules;  /* copied per-campaign model, not settings.ini fields */
  OverlayRegionPage regional_page;
} MenuTab;

typedef struct MenuSection {
  const char *label;
  const char *navigation_label; /* Optional short caption; header keeps the full title. */
  const char *blurb;      /* shown in the description panel from the nav column */
  SettingsOverlayIcon icon;
  const MenuTab *tabs;
  int tab_count;
  /* Rows built by this file rather than enumerated from the settings registry.
   * Only the layer editor uses it: its rows depend on the room the player is
   * standing in, so they cannot be static descriptors. A custom section's tabs
   * carry an index rather than a SettingCategory. */
  bool custom_rows;
  /* Hidden entirely unless show_debug_settings is on. A whole SECTION collapses
   * this way, not just its rows -- an authoring tool for someone who knows what
   * a rake does to a parallax rate has no business in a player's menu, and
   * hiding only its rows would leave an empty section in the nav column. */
  bool debug_only;
  /* Hidden when the host has no manual input. Kept as section metadata rather
   * than a label comparison so renaming the UI cannot change behavior. */
  bool requires_manual;
} MenuSection;

#define TAB(cat, name) { .category = kSettingCat_##cat, .label = name }
#define PAGE_TAB(cat, name, key, value) { .category = kSettingCat_##cat, .label = name, .page_key = key, .page_value = value }

static const MenuTab kTabsVideo[] = {
  TAB(Display, "overlay.tab.general"),
  TAB(Graphics, "overlay.tab.effects"),
  TAB(Crt, "CRT"),
  TAB(Widescreen, "overlay.tab.widescreen"),
};
static const MenuTab kTabsDiorama[] = {
  TAB(Presentation, "overlay.tab.scene"),
  TAB(DioramaCamera, "overlay.tab.camera"),
};
static const MenuTab kTabsTown[] = {
  TAB(Simulation, "overlay.tab.scene"),
  TAB(SimCamera, "overlay.tab.camera"),
  TAB(SimLighting, "overlay.tab.light"),
  TAB(SimAtmosphere, "overlay.tab.weather"),
};
static const MenuTab kTabsAudio[] = {
  TAB(Audio, "overlay.section.audio"),
};
static const MenuTab kTabsControls[] = {
  TAB(Input, "overlay.tab.devices"),
  PAGE_TAB(InputBinds, "overlay.tab.keyboard", "input_bind_page", 0),
  PAGE_TAB(InputBinds, "overlay.tab.gamepad", "input_bind_page", 1),
};
static const MenuTab kTabsCheats[] = {
  TAB(Cheats, "overlay.section.cheats"),
};
static const MenuTab kTabsSave[] = {
  /* The backend/arming controls and the apply/import/export commands used to
   * repeat on every editor page, inflating each list. They live on their own
   * Actions tab now, so the payload pages stay short. */
  PAGE_TAB(Save, "overlay.tab.actions", "save_editor_page", kSaveEditorPage_Actions),
  PAGE_TAB(Save, "overlay.tab.progress", "save_editor_page", kSaveEditorPage_Progress),
  PAGE_TAB(Save, "overlay.tab.status", "save_editor_page", kSaveEditorPage_Status),
  PAGE_TAB(Save, "overlay.tab.magic", "save_editor_page", kSaveEditorPage_Magic),
  PAGE_TAB(Save, "overlay.tab.items", "save_editor_page", kSaveEditorPage_Items),
  PAGE_TAB(Save, "overlay.tab.scores", "save_editor_page", kSaveEditorPage_Scores),
};
static const MenuTab kTabsSystem[] = {
  TAB(Extras, "overlay.tab.tools"),
  TAB(Enhancements, "overlay.tab.game"),
  TAB(Inspector, "overlay.tab.inspector"),
};
static const MenuTab kTabsManual[] = {
  TAB(Manual, "overlay.section.manual"),
};
static const MenuTab kTabsRandomizer[] = {
  TAB(RandoSeed, "overlay.tab.seed"),
  TAB(RandoEnemies, "overlay.tab.enemies"),
  TAB(RandoItems, "overlay.tab.items"),
  TAB(RandoSim, "overlay.tab.simulation"),
};
static const MenuTab kTabsLocalization[] = {
  TAB(Localization, "overlay.tab.game_text"),
  TAB(LocalizationFont, "overlay.tab.font"),
  TAB(Interface, "overlay.tab.interface"),
};
static const MenuTab kTabsRegional[] = {
  {.label = "overlay.region.tabs.presets", .regional_rules = true, .regional_page = kOverlayRegionPage_Presets},
  {.label = "overlay.region.tabs.action", .regional_rules = true, .regional_page = kOverlayRegionPage_Action},
  {.label = "overlay.region.tabs.towns", .regional_rules = true, .regional_page = kOverlayRegionPage_Towns},
  {.label = "overlay.region.tabs.controls", .regional_rules = true, .regional_page = kOverlayRegionPage_Controls},
  {.label = "overlay.region.tabs.presentation", .regional_rules = true, .regional_page = kOverlayRegionPage_Presentation},
};

/* Most Layers tabs are LEVELS ($18), not setting categories. Their position is
 * the diorama level index. The final tab is the independent live action-BG
 * extent tuner; it deliberately does not address diorama-layers.ini. */
static const MenuTab kTabsLayers[] = {
  TAB(Presentation, "Fillmore"),
  TAB(Presentation, "Bloodpool"),
  TAB(Presentation, "Kasandora"),
  TAB(Presentation, "Aitos"),
  TAB(Presentation, "Marahna"),
  TAB(Presentation, "Northwall"),
  TAB(Presentation, "Death Heim"),
  TAB(Presentation, "overlay.tab.bg_extents"),
};
_Static_assert((int)(sizeof(kTabsLayers) / sizeof(kTabsLayers[0])) ==
                   kDioramaEditorLevelCount + 1,
               "action groups plus one live BG extent tuner tab");

#undef TAB
#undef PAGE_TAB

#define SECTION(icon_, name_, blurb_, tabs_) \
  { .icon = kOverlayIcon_##icon_, .label = (name_), .blurb = (blurb_), .tabs = (tabs_), \
    .tab_count = (int)(sizeof(tabs_) / sizeof((tabs_)[0])) }
#define MANUAL_SECTION(icon_, name_, blurb_, tabs_) \
  { .icon = kOverlayIcon_##icon_, .label = (name_), .blurb = (blurb_), .tabs = (tabs_), \
    .tab_count = (int)(sizeof(tabs_) / sizeof((tabs_)[0])), \
    .requires_manual = true }
/* Developer-only, but with ordinary registry-backed rows. Distinct from
 * CUSTOM_DEBUG_SECTION, which also owns its own row model. */
#define DEBUG_SECTION(icon_, name_, blurb_, tabs_) \
  { .icon = kOverlayIcon_##icon_, .label = (name_), .blurb = (blurb_), .tabs = (tabs_), \
    .tab_count = (int)(sizeof(tabs_) / sizeof((tabs_)[0])), \
    .debug_only = true }
/* A section whose rows this file builds, and which is developer-only. */
#define CUSTOM_DEBUG_SECTION(icon_, name_, blurb_, tabs_) \
  { .icon = kOverlayIcon_##icon_, .label = (name_), .blurb = (blurb_), .tabs = (tabs_), \
    .tab_count = (int)(sizeof(tabs_) / sizeof((tabs_)[0])), \
    .custom_rows = true, .debug_only = true }

/* Sections name their atlas icon explicitly. Restart/Exit are the
 * last two rows of System > Tools, where their descriptors already lived. Each
 * section's identity is now carried entirely by its game icon (grey when
 * unselected, the colored game slot palette when current); all chrome is the
 * shared steel-blue/yellow game scheme. */
static const MenuSection kSections[] = {
  SECTION(Video, "overlay.section.video", "overlay.section.video.help",
          kTabsVideo),
  /* Both 3D sections are named for the MODE they apply to, not the technique
     they apply. "Diorama" is the technique; a player looking for the action
     stages' visuals has no reason to guess that word, and it left the pair
     reading as unrelated features when they are the same idea per mode. Named
     this way the blurbs carry the technique instead. */
  SECTION(Action, "overlay.section.action", "overlay.section.action.help",
          kTabsDiorama),
  SECTION(Town, "overlay.section.town", "overlay.section.town.help",
          kTabsTown),
  SECTION(Audio, "overlay.section.audio", "overlay.section.audio.help",
          kTabsAudio),
  SECTION(Controls, "overlay.section.controls", "overlay.section.controls.help",
          kTabsControls),
  SECTION(Cheats, "overlay.section.cheats", "overlay.section.cheats.help",
          kTabsCheats),
  SECTION(Save, "overlay.section.save", "overlay.section.save.help",
          kTabsSave),
  /* Before System: the manual is something a PLAYER reaches for, while System
   * holds host commands, restart and exit. Inserting here renumbers everything
   * below it, and tests/settings_overlay_test.c indexes sections positionally,
   * so its enum moves with this. */
  MANUAL_SECTION(Manual, "overlay.section.manual", "overlay.section.manual.help", kTabsManual),
  SECTION(System, "overlay.section.system", "overlay.section.system.help",
          kTabsSystem),
  SECTION(Localization, "overlay.section.localization", "overlay.section.localization.help",
          kTabsLocalization),
  {.icon = kOverlayIcon_Regional, .label = "overlay.region.tab",
   .navigation_label = "overlay.region.navigation", .blurb = "overlay.region.section_help",
   .tabs = kTabsRegional, .tab_count = sizeof(kTabsRegional) / sizeof(kTabsRegional[0])},
  /* Developer-only until a randomized run has actually been played end to end.
   * Every table it rewrites is verified against the ROM, but no seed has been
   * played through, so it must not read as a finished player feature. Placed
   * with the other hidden section so revealing it cannot renumber any
   * player-visible section. */
  DEBUG_SECTION(Randomizer, "overlay.section.randomizer", "overlay.section.randomizer.help",
          kTabsRandomizer),
  /* Last deliberately: it is the developer-only section, and keeping it at the
   * end means every player section's nav position is the same whether debug
   * settings are on or off. tests/settings_overlay_test.c indexes sections
   * positionally, so an insertion anywhere above here would renumber them. */
  CUSTOM_DEBUG_SECTION(Layers, "overlay.section.layers", "overlay.section.layers.help",
          kTabsLayers),
};

#undef SECTION
#undef MANUAL_SECTION
#undef DEBUG_SECTION
#undef CUSTOM_DEBUG_SECTION

enum {
  kSectionCount = (int)(sizeof(kSections) / sizeof(kSections[0])),
  kSectionResetConfirmMs = 3000,
};
static const char kSectionResetKey[] = "reset_section_defaults";

/* MenuLayout is defined in settings_overlay_internal.h (shared with the panel). */

ArRenderDevice *s_render_device;  /* extern: debug panel shares the device */
static SDL_Window *s_window;      /* SDL input service only; never renders */
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
static int s_auto_menu_scale_percent = kPercentScale;
static int s_match_game_scale_percent = kPercentScale;
static char s_status[256];
typedef struct OverlayDecision {
  SettingsOverlayDecisionResult result;
  bool accept_selected,body_text,notice;
  char title[96], body[2048], accept[96];
} OverlayDecision;
static OverlayDecision s_decision;
/* Read-only overlay-local help; never shares host confirmation state. */
static struct {
  bool open;
  char title[256], body[3072];
  int top_line, total_lines, visible_lines;
} s_details;
/* Overlay-local advisory. Separate from host-owned save/Continue decisions:
 * dismissing it returns to the same tab; no host can consume its result. */
static struct {
  OverlayDecision dialog;
  ActRaiserRegionalRulesView view;
  const OverlayRegionRow *row;
  ArRegionalSource source;
} s_regional_confirmation;
static uint64_t s_status_until;
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
static uint64_t s_hold_start_ms;
static uint64_t s_hold_next_ms;
static SDL_Keycode s_hold_key;
static bool s_hold_dirty;
static SettingChangeResult s_hold_result;
/* A section reset changes every tab (including hidden developer rows), so it
 * takes a second confirm press within a short window. The arm is cleared as
 * soon as navigation leaves the synthetic reset row. */
static int s_reset_armed_section = -1;
static uint64_t s_reset_armed_until;
/* The keyboard key of the event currently being dispatched (0 for a pad), so a
 * hold started deep inside ApplyMenuNav knows which key release will end it. */
static SDL_Keycode s_input_key;
/* Binding capture: the row is armed and the NEXT physical input on the
 * matching device becomes its binding. Held separately from s_editing because
 * capture consumes raw events rather than text. */
static const SettingDesc *s_capture_desc;
static SettingsOverlayInspectorInfoProvider s_inspector_info_provider;

/* ── Layer editor state ─────────────────────────────────────────────────
 *
 * The rows are rebuilt from diorama_layer_editor.c every time the list is
 * enumerated, so the only state kept here is what the player has chosen: which
 * plane is expanded. The override table itself lives in diorama.c and is
 * reached through the two hooks below.
 *
 * WHY HOOKS RATHER THAN CALLING diorama.c DIRECTLY. This file's test target
 * (CMakeLists.txt:377) links five sources and not diorama.c -- deliberately,
 * since diorama.c drags in the PPU and SDL render path. Calling
 * Diorama_LayerOverrides() here would force that whole closure into the test or
 * force the test to stub it. Injecting the accessor keeps the overlay testable
 * and follows the precedent already set by s_inspector_info_provider, which
 * exists for the same reason. */
static SettingsOverlayLayerTableFn s_layer_table_provider;
static SettingsOverlayLayerRoomFn s_layer_room_provider;
static SettingsOverlayLayerSaveFn s_layer_save_provider;
static SettingsOverlayLayerPaletteFn s_layer_palette_provider;
/* Which plane's parameters are expanded, or -1. Held rather than derived from
 * the cursor so the expansion does not collapse while the player steps DOWN
 * through its own parameter rows. */
static int s_layer_plane = -1;
static void ClearSectionResetArm(void) {
  s_reset_armed_section = -1;
  s_reset_armed_until = 0;
}

void SettingsOverlay_SetInspectorInfoProvider(
    SettingsOverlayInspectorInfoProvider provider) {
  s_inspector_info_provider = provider;
}

static SettingsOverlayManualHooks s_manual_hooks;

void SettingsOverlay_SetManualHooks(const SettingsOverlayManualHooks *hooks) {
  if (hooks) s_manual_hooks = *hooks;
  else memset(&s_manual_hooks, 0, sizeof s_manual_hooks);
}

/* Inert until the host injects the reader, which is what lets a target that
 * links this file without one -- the overlay's own test -- behave as though the
 * manual does not exist. */
static bool ManualIsOpen(void) {
  return s_manual_hooks.is_open && s_manual_hooks.is_open();
}

static bool ManualAvailable(void) {
  return s_manual_hooks.available && s_manual_hooks.available();
}

void SettingsOverlay_SetLayerEditorHooks(SettingsOverlayLayerTableFn table,
                                         SettingsOverlayLayerRoomFn room,
                                         SettingsOverlayLayerSaveFn save) {
  s_layer_table_provider = table;
  s_layer_room_provider = room;
  s_layer_save_provider = save;
}

void SettingsOverlay_SetLayerPaletteProvider(
    SettingsOverlayLayerPaletteFn provider) {
  s_layer_palette_provider = provider;
  if (!provider) {
    SettingsOverlayPalette_Close();
    SettingsOverlayPalette_ReleaseTexture();
  }
}

/* Layout math stays integer (it also feeds the public panel-rect API), so
 * convert only at the portable draw call. */
ArRenderRectF ToRenderRect(ArRenderRectI r) {
  return (ArRenderRectF){ (float)r.x, (float)r.y,
                          (float)r.w, (float)r.h };
}

static ArRenderColorF RenderColor(uint32_t color) {
  return (ArRenderColorF){
    .r = (float)((color >> 16) & 0xff) / 255.0f,
    .g = (float)((color >> 8) & 0xff) / 255.0f,
    .b = (float)(color & 0xff) / 255.0f,
    .a = (float)(color >> 24) / 255.0f,
  };
}

/* SDL3 SDL_StartTextInput/SDL_StopTextInput require the target window. A
 * headless or non-SDL presentation host supplies none, so these become no-ops. */
static SDL_Window *OverlayWindow(void) {
  return s_window;
}

static uint32_t ScaleColor(uint32_t color, int percent) {
  unsigned r = ((color >> 16) & 0xff) * (unsigned)percent /
      kPercentScale;
  unsigned g = ((color >> 8) & 0xff) * (unsigned)percent /
      kPercentScale;
  unsigned b = (color & 0xff) * (unsigned)percent /
      kPercentScale;
  return ARGB(255, r, g, b);
}

static int CursorBlinkOffset(void) {
  return (int)((HostClock_Milliseconds() /
                kCursorBlinkHalfPeriodMs) & 1u);
}

/* ── Section / tab / row addressing ─────────────────────────────────────
 * s_section indexes kSections. s_tab[section] remembers which tab that
 * section was last left on, so stepping away and back does not dump the
 * player at the top of a four-tab section. Rows are the visible descriptors
 * of the active tab's category, in descriptor-table order. */
/* A whole section can be hidden, which tabs and rows could already do but
 * sections could not. The layer editor is developer-only, and the Manual has no
 * useful UI without an input PDF; hiding only their rows would leave empty/dead
 * sections for a player to walk into. */
static bool SectionHidden(int section) {
  if (section < 0 || section >= kSectionCount) return true;
  const MenuSection *candidate = &kSections[section];
  if (candidate->debug_only && !g_settings.show_debug_settings) return true;
  return candidate->requires_manual && !ManualAvailable();
}

static int VisibleSectionCount(void) {
  int count = 0;
  for (int i = 0; i < kSectionCount; i++)
    if (!SectionHidden(i)) count++;
  return count < 1 ? 1 : count;
}

static const MenuSection *ActiveSection(void) {
  return &kSections[s_section];
}

/* A tab is hidden when it holds no visible rows — which happens when every row
 * it would list is developer-only and debug settings are off (the Town 3D
 * Light/Weather tabs and the System Inspector tab collapse this way). Page tabs
 * (Save pages, Controls binding pages) share always-visible rows, so they never
 * collapse and are cheap to short-circuit. */
static bool RawTabHidden(int section, int tab) {
  const MenuTab *menu_tab = &kSections[section].tabs[tab];
  /* Campaign-owned rows are independent of the global settings registry. */
  if (menu_tab->regional_rules || menu_tab->page_key) return false;
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

/* Normalize only at explicit refresh/transition points. Queries below read
 * the selected section and tab without changing navigation or settings. */
static bool NormalizeNavigation(void) {
  const int previous_section = s_section;
  if (s_section < 0)
    s_section = 0;
  if (s_section >= kSectionCount)
    s_section = kSectionCount - 1;
  if (SectionHidden(s_section)) {
    for (int i = 1; i <= kSectionCount; i++) {
      int candidate = (s_section + i) % kSectionCount;
      if (!SectionHidden(candidate)) {
        s_section = candidate;
        break;
      }
    }
  }
  const MenuSection *section = ActiveSection();
  const int previous_tab = s_tab[s_section];
  int tab = previous_tab;
  if (tab < 0) tab = 0;
  if (tab >= section->tab_count) tab = section->tab_count - 1;
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
  return s_section != previous_section || tab != previous_tab;
}

static int ActiveTabIndex(void) { return s_tab[s_section]; }

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

/* A page transition owns these in-range enum selectors. They have no change
 * callbacks or restart semantics; synchronizing them here does not save the
 * settings file. Refresh calls this before building the visible row snapshot.
 */
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

/* ── Layer editor rows ───────────────────────────────────────────────────
 *
 * Rebuilt on demand rather than cached. The list depends on the live room, which
 * changes as the player walks, and on which plane is expanded -- caching it
 * would need invalidation on both, and the build is a few dozen snprintf calls
 * on a menu that is only open while the game is paused. */
static bool ActiveSectionIsCustom(void) {
  return ActiveSection()->custom_rows;
}

static SettingsOverlayRegionalHooks s_regional_hooks;
static ActRaiserRegionalRulesView s_regional_view;
static bool s_regional_valid;
/* Unapplied preset choices belong to the menu, not the campaign. */
static int s_regional_preset_choice[2] = {-1, -1};
static const OverlayRegionRow *SelectedRegionRow(void) {
  return OverlayRegionMenu_Row(ActiveTab()->regional_page,(unsigned)s_row);
}

void SettingsOverlay_SetRegionalHooks(const SettingsOverlayRegionalHooks *hooks) {
  s_details.open = false;
  s_regional_hooks = hooks ? *hooks : (SettingsOverlayRegionalHooks){0};
  s_regional_valid = false;
  s_regional_preset_choice[0] = s_regional_preset_choice[1] = -1;
  memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
}

static bool ActiveTabIsRegional(void) { return ActiveTab()->regional_rules; }

static const char *RegionalNotice(void) {
  return Ui(!s_regional_valid ? "overlay.region.enter_campaign" :
      !s_regional_view.editable ? "overlay.region.replay_locked" :
      s_regional_view.new_game ? "overlay.region.new_game_draft" :
      s_regional_view.population_pending ? "overlay.region.population_pending" : "overlay.region.saved_with_story");
}

/* System > Game is the one registry-backed tab with semantic subsections.
 * Descriptor metadata owns the classification; this menu layer only chooses
 * presentation order and inserts non-selectable heading rows. */
static const SettingGameChangeKind kGameChangeGroupOrder[] = {
  kSettingGameChange_OriginalBugFix,
  kSettingGameChange_QualityOfLife,
};

typedef struct SettingMenuRow {
  const SettingDesc *desc;
  SettingGameChangeKind heading;
} SettingMenuRow;

enum {
  kSettingMenuGroupCount =
      sizeof(kGameChangeGroupOrder) / sizeof(kGameChangeGroupOrder[0]),
  kSettingMenuRowCapacity = kSettingsMaxDescriptors + kSettingMenuGroupCount,
};
static SettingMenuRow s_registry_rows[kSettingMenuRowCapacity];
static int s_registry_row_count;

/* Build once per refresh, in display order. Count, selection and drawing all
 * consume this same list. Each descriptor appears at most once, plus at most
 * one heading per group, so the registry's compile-time bound also covers it.
 */
static void RebuildRegistryMenuRows(void) {
  s_registry_row_count = 0;
  if (ActiveSectionIsCustom() || ActiveTabIsRegional())
    return;
  if (ActiveTab()->category != kSettingCat_Enhancements) {
    for (int i = 0; i < g_setting_desc_count; i++) {
      const SettingDesc *desc = &g_setting_descs[i];
      if (RowBelongsToActiveTab(desc))
        s_registry_rows[s_registry_row_count++] =
            (SettingMenuRow){.desc = desc};
    }
    return;
  }
  for (int group = 0; group < kSettingMenuGroupCount; group++) {
    const SettingGameChangeKind kind = kGameChangeGroupOrder[group];
    bool heading_added = false;
    for (int i = 0; i < g_setting_desc_count; i++) {
      const SettingDesc *desc = &g_setting_descs[i];
      if (!RowBelongsToActiveTab(desc) || desc->game_change_kind != kind)
        continue;
      if (!heading_added) {
        s_registry_rows[s_registry_row_count++] =
            (SettingMenuRow){.heading = kind};
        heading_added = true;
      }
      s_registry_rows[s_registry_row_count++] = (SettingMenuRow){.desc = desc};
    }
  }
  /* Keep unclassified descriptors reachable in downstream builds. The
   * settings contract test rejects this state in the shipped registry. */
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (RowBelongsToActiveTab(desc) &&
        desc->game_change_kind == kSettingGameChange_None)
      s_registry_rows[s_registry_row_count++] = (SettingMenuRow){.desc = desc};
  }
}

static int RegistryMenuRowCount(void) { return s_registry_row_count; }

static SettingMenuRow RegistryMenuRowAt(int index) {
  return index >= 0 && index < s_registry_row_count ? s_registry_rows[index]
                                                    : (SettingMenuRow){0};
}

/* Row-name suffix for SettingsOverlay_SelectedKey, so a test can navigate to
 * "bg2hi.copies" rather than counting keypresses through a list whose shape
 * changes with the active shape. These names are a TEST seam, not the manifest
 * grammar -- the file's own keys live in diorama_layer_order.c -- but they are
 * spelled the same so a failure message reads against the file. */
static const char kLayerResetRoomKey[] = "layer_reset_room";
static const char *LayerParamKey(DioramaEditorParam param) {
  switch (param) {
    case kDioramaEditorParam_Depth:     return "depth";
    case kDioramaEditorParam_Copies:    return "copies";
    case kDioramaEditorParam_Density:   return "density";
    case kDioramaEditorParam_Direction: return "dir";
    case kDioramaEditorParam_Z:         return "z";
    case kDioramaEditorParam_Alpha:     return "alpha";
    case kDioramaEditorParam_TransparentFill: return "transparent";
    case kDioramaEditorParam_Source:    return "source";
    case kDioramaEditorParam_Order:     return "order";
    case kDioramaEditorParam_None:
    default:                            return "";
  }
}

enum { kLayerMenuRowMax = kDioramaEditorRowMax };
_Static_assert(kActionBgTunerRowMax <= kLayerMenuRowMax,
               "shared Layers row buffer must fit the BG tuner");

typedef enum LayerMenuRowOwner {
  kLayerMenuRow_Diorama = 0,
  kLayerMenuRow_ActionBg,
} LayerMenuRowOwner;

typedef struct LayerMenuRow {
  LayerMenuRowOwner owner;
  char key[48];
  bool nested;
  bool selectable;
  bool separator_before;
  union {
    DioramaEditorRow diorama;
    ActionBgTunerRow action_bg;
  } source;
} LayerMenuRow;

static bool ActiveLayerTabIsBgTuner(void) {
  return ActiveSectionIsCustom() &&
      ActiveTabIndex() == kDioramaEditorLevelCount;
}

static int LayerEditorRows(DioramaEditorRow *rows, int capacity) {
  if (!ActiveSectionIsCustom()) return 0;
  DioramaEditorContext context;
  memset(&context, 0, sizeof(context));
  context.selected_plane = s_layer_plane;
  if (s_layer_room_provider)
    context.room_live = s_layer_room_provider(&context.map_group,
                                              &context.map_number,
                                              &context.section);
  const DioramaLayerOrderTable *table =
      s_layer_table_provider ? s_layer_table_provider() : NULL;
  return DioramaLayerEditor_BuildRows(
      table, &context, ActiveTabIndex(), rows, capacity);
}

static int LayerMenuRows(LayerMenuRow *out, int capacity) {
  if (!ActiveSectionIsCustom() || !out || capacity <= 0) return 0;
  int count = 0;
  if (ActiveLayerTabIsBgTuner()) {
    ActionBgTunerRow rows[kActionBgTunerRowMax];
    int n = ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
    for (int i = 0; i < n && count < capacity; i++) {
      LayerMenuRow *dst = &out[count++];
      *dst = (LayerMenuRow) { .owner = kLayerMenuRow_ActionBg };
      dst->source.action_bg = rows[i];
      snprintf(dst->key, sizeof(dst->key), "%s", rows[i].key);
      dst->nested = rows[i].nested;
      dst->selectable = rows[i].selectable;
      dst->separator_before = rows[i].separator_before;
    }
    return count;
  }

  DioramaEditorRow rows[kDioramaEditorRowMax];
  int n = LayerEditorRows(rows, kDioramaEditorRowMax);
  for (int i = 0; i < n && count < capacity; i++) {
    LayerMenuRow *dst = &out[count++];
    *dst = (LayerMenuRow) { .owner = kLayerMenuRow_Diorama };
    dst->source.diorama = rows[i];
    dst->nested = rows[i].nested;
    dst->selectable = rows[i].selectable;
    dst->separator_before =
        rows[i].kind == kDioramaEditorRow_ResetRoom;
    if (rows[i].kind == kDioramaEditorRow_ResetRoom) {
      snprintf(dst->key, sizeof(dst->key), "%s", kLayerResetRoomKey);
    } else if (rows[i].kind != kDioramaEditorRow_Header) {
      const char *token = DioramaLayerOrder_PlaneToken(rows[i].plane);
      if (token && rows[i].kind == kDioramaEditorRow_Plane)
        snprintf(dst->key, sizeof(dst->key), "%s", token);
      else if (token)
        snprintf(dst->key, sizeof(dst->key), "%s.%s", token,
                 LayerParamKey(rows[i].param));
    }
  }
  return count;
}

/* Resolve captions only for rows actually drawn, not every navigation/count
 * probe. The immutable source row remains the authority for both text and edits. */
static void LocalizeLayerRow(const LayerMenuRow *row, SettingsOverlayLayerText *text) {
  if (row->owner == kLayerMenuRow_ActionBg)
    SettingsOverlay_LocalizedActionBgRow(InterfaceLocale(), &row->source.action_bg, text);
  else
    SettingsOverlay_LocalizedDioramaRow(InterfaceLocale(), &row->source.diorama, text);
}

/* The selected row, or NULL when the cursor is on a header (which is not
 * selectable) or the section is not the editor. */
static const LayerMenuRow *SelectedLayerRow(LayerMenuRow *rows,
                                            int capacity, int *out_count) {
  int n = LayerMenuRows(rows, capacity);
  if (out_count) *out_count = n;
  if (s_row < 0 || s_row >= n) return NULL;
  return &rows[s_row];
}

static int TabSettingRowCount(void) {
  if (ActiveTabIsRegional()) return s_regional_valid ? (int)OverlayRegionMenu_Count(ActiveTab()->regional_page) : 1;
  if (ActiveSectionIsCustom()) {
    LayerMenuRow rows[kLayerMenuRowMax];
    return LayerMenuRows(rows, kLayerMenuRowMax);
  }
  return RegistryMenuRowCount();
}

/* Every populated tab ends with the same section-scoped action. Town 3D's
 * button therefore restores Scene + Camera + Light + Weather together no
 * matter which tab the player happens to be viewing.
 *
 * The layer editor is the exception: its rows are not registry descriptors, so
 * "reset this section's settings to defaults" has nothing to act on. It carries
 * its own per-room reset row instead, built into the list. */
static int TabRowCount(void) {
  if (ActiveSectionIsCustom() || ActiveTabIsRegional()) return TabSettingRowCount();
  return TabSettingRowCount() + 1;
}

/* Nav rows are the sections themselves; a section with no populated tab at
 * all would be dead, but every section here always has at least one row, so
 * the nav list is a fixed eight and never renumbers under the cursor. */
static int NavPopulatedCount(void) {
  return VisibleSectionCount();
}

/* The selected section's position among the VISIBLE ones -- what the nav column
 * draws at and what the test navigation counts in, since a hidden section
 * occupies no row. */
static int ActiveVisibleSectionPosition(void) {
  int position = 0;
  for (int i = 0; i < s_section; i++)
    if (!SectionHidden(i)) position++;
  return position;
}

/* Scrolls in VISIBLE positions, not raw section indices: with a hidden section
 * present the two differ, and mixing them would scroll the column to a row that
 * is not drawn. */
static void EnsureSelectedNavVisible(void) {
  int visible = s_nav_visible_rows > 0 ? s_nav_visible_rows : 1;
  const int position = ActiveVisibleSectionPosition();
  const int total = VisibleSectionCount();
  if (position < s_nav_top_row) s_nav_top_row = position;
  if (position >= s_nav_top_row + visible)
    s_nav_top_row = position - visible + 1;
  int maximum_top = total > visible ? total - visible : 0;
  if (s_nav_top_row > maximum_top) s_nav_top_row = maximum_top;
  if (s_nav_top_row < 0) s_nav_top_row = 0;
}

static const SettingDesc *SelectedDesc(void) {
  /* Custom sections have no descriptors: layer-editor row 3 is not presentation
   * descriptor 3. Keep this guard even though callers also branch earlier; a
   * missed branch must not let a custom row edit an unrelated setting. */
  if (ActiveSectionIsCustom() || ActiveTabIsRegional()) return NULL;
  return RegistryMenuRowAt(s_row).desc;
}

static bool SelectedRowIsSectionReset(void) {
  /* The layer editor has no registry rows for a section reset to act on, and
   * carries its own per-room reset instead. Without this guard the synthetic row
   * would appear one past the end of its list and reset nothing. */
  if (ActiveSectionIsCustom() || ActiveTabIsRegional()) return false;
  return s_submenu_open && s_row == TabSettingRowCount();
}


static void SetStatus(const char *text) {
  snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
  s_status_until = HostClock_Milliseconds() + 2500;
}

/* Persist the current settings to disk and report the outcome. Split from the
 * apply step so a held value can be applied live every frame but written once
 * on release. */
static void PersistChange(SettingChangeResult result) {
  char settings_file[kHostPathCapacity];
  const char *settings_path = getenv("AR_OVERLAY_TEST_SETTINGS_PATH");
  if (!settings_path || !settings_path[0])
    settings_path = UserDataFile(settings_file, sizeof settings_file,
                                 "settings.ini");
  if (!Settings_Save(settings_path)) {
    SetStatus(Ui("overlay.status.save_failed"));
    fprintf(stderr, "[settings-menu] could not save %s\n", settings_path);
    return;
  }
  if (result == kSettingChange_RestartPending)
    SetStatus(Ui("overlay.status.restart_saved"));
  else if (result == kSettingChange_AppliedStickyDisable)
    SetStatus(Ui("overlay.status.sticky_saved"));
  else
    SetStatus(Ui("overlay.status.applied_saved"));
}

static void SaveAcceptedChange(SettingChangeResult result) {
  SettingsOverlay_Refresh();
  if (result <= kSettingChange_Unchanged) {
    SetStatus(result == kSettingChange_Rejected ? Ui("overlay.status.not_editable") : Ui("overlay.status.unchanged"));
    return;
  }
  PersistChange(result);
}

/* Reset every registry category represented by the active top-level section.
 * Repeated paging tabs (Save, keyboard/gamepad bindings) deliberately collapse
 * to one category reset each. Hidden debug rows are registry rows too, so this
 * produces the shipped configuration rather than merely resetting what the
 * current menu happens to expose. */
static void ConfirmOrResetActiveSection(void) {
  uint64_t now = HostClock_Milliseconds();
  const MenuSection *section = ActiveSection();
  if (s_reset_armed_section != s_section || now > s_reset_armed_until) {
    s_reset_armed_section = s_section;
    s_reset_armed_until = now + kSectionResetConfirmMs;
    char status[sizeof(s_status)];
    FormatSectionMessage(status, sizeof(status), "overlay.confirm_reset", section->label);
    SetStatus(status);
    s_status_until = s_reset_armed_until;
    return;
  }

  ClearSectionResetArm();
  bool categories[kSettingCat_Count] = {false};
  for (int tab = 0; tab < section->tab_count; tab++)
    categories[section->tabs[tab].category] = true;

  SettingChangeResult aggregate = kSettingChange_Unchanged;
  for (int category = 0; category < kSettingCat_Count; category++) {
    if (!categories[category]) continue;
    SettingChangeResult result =
        Settings_ResetCategory((SettingCategory)category);
    if (result > aggregate) aggregate = result;
  }
  fprintf(stderr, "[settings-menu] reset %s section to built-in defaults\n",
          ArUiCatalog_Text(kArUiLocale_English, section->label, section->label));
  SaveAcceptedChange(aggregate);
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
static long HoldStepMultiplier(const SettingDesc *desc, uint64_t held_ms) {
  long base = desc->step > 0 ? desc->step : 1;
  long range = desc->maxval - desc->minval;
  if (range <= 0) return 1;
  long coarse_units = NiceStep(range / 24);
  long coarse_mult = coarse_units / base;
  if (coarse_mult < 1) coarse_mult = 1;
  if (held_ms < (uint64_t)kHoldInitialDelayMs + 700) return 1;
  if (held_ms < (uint64_t)kHoldInitialDelayMs + 1700) {
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
    SetStatus(Ui("overlay.status.not_text_editable"));
    return;
  }
  Settings_FormatValue(desc, s_edit_buffer, sizeof(s_edit_buffer));
  if (desc->apply == kApply_Save &&
      !strcmp(s_edit_buffer, "Leave as-is"))
    s_edit_buffer[0] = 0;
  s_editing = true;
  if (OverlayWindow()) SDL_StartTextInput(OverlayWindow());
  SetStatus(Ui("overlay.status.type_value"));
}

static void CancelCapture(void) {
  if (!s_capture_desc) return;
  s_capture_desc = NULL;
  SetStatus(Ui("overlay.status.bind_cancelled"));
}

static void BeginCapture(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || desc->type != kSettingType_Binding) return;
  InputClass klass;
  if (!InputMap_DescribeRow(desc, NULL, &klass)) return;
  s_capture_desc = desc;
  SetStatus(klass == kInputClass_Keyboard ? Ui("overlay.status.press_key")
                                          : Ui("overlay.status.press_button"));
}

static void CommitEditing(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!s_editing || !desc) return;
  SettingChangeResult result = Settings_SetText(desc, s_edit_buffer);
  if (result == kSettingChange_Rejected) {
    SetStatus(Ui("overlay.status.invalid_value"));
    return;
  }
  StopEditing();
  SaveAcceptedChange(result);
}

static void InvokeSelectedAction(void) {
  const SettingDesc *desc = SelectedDesc();
  if (!desc || desc->type != kSettingType_Action) return;
  SetStatus(Settings_InvokeAction(desc) ? Ui("overlay.status.action_complete") : Ui("overlay.status.action_failed"));
}

/* Int rows are adjusted entirely by stepping (with hold-to-accelerate); they
 * never open the text editor. Mask/Custom rows are the genuine non-numeric
 * holdouts — a hex layer mask, arbitrary PAR pins, a player name — and keep
 * text entry.
 *
 * Apply one value step of `multiplier` base-steps. Live every call; persisted
 * immediately when `persist` (a tap or single press), otherwise deferred to
 * the end of the hold via s_hold_dirty. Returns true when the HUD scale
 * crosses its upper cycle boundary into Match game. */
static bool StepNumeric(const SettingDesc *desc, int direction,
                        long multiplier, bool persist) {
  long value = 0;
  if (!Settings_GetLong(desc, &value)) return false;
  long stored_value = value;
  /* The scale rows use 0 as a "follow the auto value" sentinel; step off that
   * resolved number so the first press moves relative to what is on screen. */
  if (value == 0 && desc->field == &g_settings.menu_scale_percent)
    value = s_auto_menu_scale_percent;
  if (value == 0 && desc->field == &g_settings.hud_scale_percent)
    value = s_match_game_scale_percent;
  long step = desc->step > 0 ? desc->step : 1;
  long next = value + (long)direction * step * multiplier;
  bool wrapped = desc->field == &g_settings.hud_scale_percent &&
                 direction > 0 && stored_value != 0 && value >= desc->maxval;
  if (wrapped) next = 0;  /* 0 is the HUD row's "Match game" sentinel. */
  SettingChangeResult result = Settings_SetLong(desc, next);
  if (persist) {
    SaveAcceptedChange(result);
  } else if (result > kSettingChange_Unchanged) {
    s_hold_dirty = true;
    s_hold_result = result;
  }
  return wrapped;
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
  s_hold_start_ms = HostClock_Milliseconds();
  s_hold_next_ms = s_hold_start_ms + kHoldInitialDelayMs;
  s_hold_dirty = false;
  s_hold_result = kSettingChange_Applied;
  /* Stop this key hold at the cycle boundary. Otherwise its first repeat
   * would immediately step away from Match game again. */
  if (StepNumeric(desc, direction, 1, false)) EndValueHold();
}

/* ── Layer editor dispatch ───────────────────────────────────────────────
 *
 * Key handling and drawing run synchronously on the main thread, so an override
 * edit is ordered before the next frame reads it. This is what makes live A/B
 * editing safe without locking.
 *
 * Returns true when the row belonged to the editor, so the ordinary descriptor
 * paths are skipped. */
static DioramaPlaneOverride *LayerPlaneForRow(const DioramaEditorRow *row,
                                              bool create) {
  if (!row || row->plane < 0 || !s_layer_table_provider) return NULL;
  DioramaLayerOrderTable *table = s_layer_table_provider();
  if (!table) return NULL;
  DioramaRoomOverride *room = NULL;
  if (create) {
    /* The first committed edit creates its room entry. A full table returns
     * NULL and is reported rather than dropping the edit silently. */
    room = DioramaLayerOrder_FindOrAddSection(
        table, row->map_group, row->map_number, row->section);
  } else {
    /* Clear only an already-existing record; reset/preview paths must not
     * allocate one of the bounded section slots. */
    room = DioramaLayerOrder_FindMutableSection(
        table, row->map_group, row->map_number, row->section);
  }
  if (!room) return NULL;
  return &room->planes[row->plane];
}

static void LayerPruneEmptySection(const DioramaEditorRow *row) {
  if (!row || !s_layer_table_provider) return;
  DioramaLayerOrderTable *table = s_layer_table_provider();
  if (!table) return;
  const DioramaRoomOverride *room = DioramaLayerOrder_FindSection(
      table, row->map_group, row->map_number, row->section);
  if (room && !DioramaLayerOrder_RoomIsActive(room))
    DioramaLayerOrder_ResetSection(
        table, row->map_group, row->map_number, row->section);
}

static void LayerSaveEdit(void) {
  if (s_layer_save_provider && !s_layer_save_provider())
    SetStatus(Ui("overlay.status.save_failed"));
}

static void CommitLayerPalette(const DioramaEditorRow *row, bool reset, uint8_t index) {
  DioramaPlaneOverride *plane = LayerPlaneForRow(row, !reset);
  if (reset) {
    if (plane) {
      DioramaLayerEditor_ClearParam(plane, kDioramaEditorParam_TransparentFill);
      LayerPruneEmptySection(row);
      LayerSaveEdit();
      SetStatus(Ui("overlay.status.fill_cleared"));
    } else {
      SetStatus(Ui("overlay.status.inherited"));
    }
    return;
  }
  if (!plane) {
    SetStatus(Ui("overlay.status.no_room"));
    return;
  }
  plane->set_transparent_fill = true;
  plane->transparent_fill_kind = kDioramaTransparentFill_Cgram;
  plane->transparent_fill_cgram = index;
  LayerSaveEdit();
  SetStatus(Ui("overlay.status.fill_applied"));
}

static bool LayerOpenPalette(const DioramaEditorRow *row) {
  uint16_t palette[kSettingsOverlayLayerPaletteEntries];
  if (!row || row->param != kDioramaEditorParam_TransparentFill || !s_layer_palette_provider ||
      !s_layer_palette_provider(palette)) {
    SetStatus(Ui("overlay.status.palette_unavailable"));
    return false;
  }
  SettingsOverlayPalette_Open(row, palette, CommitLayerPalette);
  return true;
}

static void ReportActionBgTunerResult(ActionBgTunerResult result) {
  switch (result) {
    case kActionBgTunerResult_Changed: SetStatus(Ui("overlay.status.draft_updated")); break;
    case kActionBgTunerResult_AtLimit: SetStatus(Ui("overlay.status.at_limit")); break;
    case kActionBgTunerResult_Printed: SetStatus(Ui("overlay.status.printed")); break;
    case kActionBgTunerResult_Reset: SetStatus(Ui("overlay.status.draft_reset")); break;
    case kActionBgTunerResult_Unchanged:
    default: break;
  }
}

static bool LayerChangeSelected(int direction) {
  if (!ActiveSectionIsCustom()) return false;
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row =
      SelectedLayerRow(rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return true;   /* owned, nothing to do */
  if (row->owner == kLayerMenuRow_ActionBg) {
    if (row->source.action_bg.kind == kActionBgTunerRow_Print ||
        row->source.action_bg.kind == kActionBgTunerRow_Reset) {
      SetStatus(Ui("overlay.status.activate"));
      return true;
    }
    ReportActionBgTunerResult(
        ActionBgTuner_Change(&row->source.action_bg, direction));
    return true;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;

  if (diorama->kind == kDioramaEditorRow_ResetRoom) {
    SetStatus(Ui("overlay.status.reset_room"));
    return true;
  }

  DioramaPlaneOverride *plane = LayerPlaneForRow(diorama, true);
  if (!plane) {
    SetStatus(Ui("overlay.status.no_room"));
    return true;
  }

  if (diorama->kind == kDioramaEditorRow_Plane) {
    DioramaDepthStrategy next =
        DioramaLayerEditor_CycleStrategy(plane, direction);
    /* Expanding the plane the player just changed puts its parameters under the
     * cursor immediately, which is the next thing they want. */
    s_layer_plane = diorama->plane;
    char key[64];
    snprintf(key, sizeof(key), "overlay.layer.diorama.shape.%d", next);
    SetStatus(ArUiCatalog_Text(InterfaceLocale(), key,
                               DioramaLayerOrder_StrategyName(next)));
    LayerSaveEdit();
    return true;
  }

  /* A scoped row displays the renderer-resolved source, which may be inherited
   * from its base room. Seed a first local edit from that displayed value so
   * Right means "next source" rather than jumping from hidden Captured state. */
  if (diorama->param == kDioramaEditorParam_Source && !plane->set_source) {
    plane->source = diorama->effective_source;
    plane->set_source = true;
  }

  if (!DioramaLayerEditor_StepParam(plane, diorama->param, direction)) {
    SetStatus(Ui("overlay.status.at_limit"));
    return true;
  }
  LayerSaveEdit();
  return true;
}

static ArRegionalSource RegionalPresetChoice(const OverlayRegionRow *row) {
  const int chosen = s_regional_preset_choice[row->group == kArRegionalProfile_Presentation];
  const ArRegionalSource source = chosen >= 0 ? (ArRegionalSource)chosen :
      OverlayRegionMenu_RowSource(&s_regional_view, row, false);
  return source == kArRegionalSource_Count ? kArRegionalSource_US : source;
}

static void RegionalReportResult(ActRaiserRegionalEditResult result) {
  /* Successful cycling should show the newly selected behavior immediately,
   * not cover its explanation with the generic save reminder. */
  if (result == kActRaiserRegionalEdit_Applied || result == kActRaiserRegionalEdit_Unchanged)
    s_status[0] = 0;
  else SetStatus(SettingsOverlayRegions_EditStatus(InterfaceLocale(), result));
}

static bool RegionalChangeSelected(int direction, bool reset, bool activate) {
  if (!ActiveTabIsRegional()) return false;
  const OverlayRegionRow *row=SelectedRegionRow();
  if (!s_regional_valid || !s_regional_hooks.request || !s_regional_view.editable) {
    SetStatus(RegionalNotice());
    return true;
  }
  if(!row)return true;
  if(row->kind==kOverlayRegionRow_Difficulty) {
    if(!s_regional_hooks.difficulty)return true;
    const unsigned current=ActRaiserRegionalSettings_DifficultyChoice(&s_regional_view, false);
    const ArRegionalDifficultyChoice next=reset?kArRegionalDifficultyChoice_Original:
        current==kArRegionalDifficultyChoice_Custom ? kArRegionalDifficultyChoice_Original :
        (ArRegionalDifficultyChoice)((current+(direction<0?3u:1u))%kArRegionalDifficultyChoice_Count);
    RegionalReportResult(s_regional_hooks.difficulty(&s_regional_view,next));
    SettingsOverlay_Refresh();return true;
  }
  ArRegionalSource source = kArRegionalSource_US;
  if (row->kind == kOverlayRegionRow_Preset) {
    source = RegionalPresetChoice(row);
    if (!activate) {
      s_regional_preset_choice[row->group == kArRegionalProfile_Presentation] = reset ? kArRegionalSource_US :
          (source + (direction < 0 ? 2 : 1)) % kArRegionalSource_Count;
      s_status[0] = 0;
      return true;
    }
  } else if(!reset) {
    const ArRegionalSource current=OverlayRegionMenu_RowSource(&s_regional_view,row,false);
    source=current==kArRegionalSource_Count?(direction<0?kArRegionalSource_Europe:kArRegionalSource_US):
        (ArRegionalSource)((current+(direction<0?2:1))%kArRegionalSource_Count);
    if (row->binary) source = current == kArRegionalSource_Japan ? kArRegionalSource_US : kArRegionalSource_Japan;
  }
  const bool narrow = row->kind == kOverlayRegionRow_Setting;
  if (narrow ? (!s_regional_hooks.preview_setting || !s_regional_hooks.setting) : !s_regional_hooks.preview) {
    SetStatus(SettingsOverlayRegions_EditStatus(InterfaceLocale(), kActRaiserRegionalEdit_Invalid));
    return true;
  }
  ActRaiserRegionalEditImpact impact;
  const ActRaiserRegionalEditResult preview = narrow
      ? s_regional_hooks.preview_setting(&s_regional_view, row->setting, source, &impact)
      : s_regional_hooks.preview(&s_regional_view, row->group, source, &impact);
  if (preview != kActRaiserRegionalEdit_Applied && preview != kActRaiserRegionalEdit_Unchanged &&
      preview != kActRaiserRegionalEdit_Deferred) {
    SetStatus(SettingsOverlayRegions_EditStatus(InterfaceLocale(), preview));
    return true;
  }
  if (row->kind == kOverlayRegionRow_Preset || impact.towns != kArRegionalTownImpact_None || impact.estimated_history) {
    OverlayDecision dialog = {.result = kOverlayDecision_Pending, .body_text = true};
    snprintf(dialog.title, sizeof(dialog.title), "%s", row->kind == kOverlayRegionRow_Preset
        ? "overlay.region.preset.title" : "overlay.region.warning.title");
    snprintf(dialog.accept, sizeof(dialog.accept), "%s", impact.towns == kArRegionalTownImpact_Redevelopment
        ? "overlay.region.warning.queue" : "overlay.region.warning.apply");
    const bool formatted = row->kind == kOverlayRegionRow_Preset
        ? OverlayRegionMenu_PresetWarning(InterfaceLocale(), row, source, &impact, dialog.body, sizeof(dialog.body))
        : SettingsOverlayRegions_EditWarning(InterfaceLocale(), OverlayRegionMenu_Label(InterfaceLocale(), row),
            source, &impact, dialog.body, sizeof(dialog.body));
    if (!formatted) return true;
    s_regional_confirmation.dialog = dialog;
    s_regional_confirmation.view = s_regional_view;
    s_regional_confirmation.row = row;
    s_regional_confirmation.source = source;
    EndValueHold();
    return true;
  }
  const ActRaiserRegionalEditResult result = narrow
      ? s_regional_hooks.setting(&s_regional_view, row->setting, source)
      : s_regional_hooks.request(&s_regional_view, row->group, source);
  RegionalReportResult(result);
  SettingsOverlay_Refresh();
  return true;
}

static void ChangeSelectedValue(int direction) {
  if (RegionalChangeSelected(direction, false, false)) return;
  if (LayerChangeSelected(direction)) return;
  if (SelectedRowIsSectionReset()) {
    SetStatus(Ui("overlay.status.reset_section"));
    return;
  }
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus(Ui("overlay.status.unavailable"));
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
        SetStatus(Ui("overlay.status.edit_ini"));
        return;
      }
      long next;
      if (desc->type == kSettingType_Bool) {
        next = !value;
      } else {
        long step = desc->step > 0 ? desc->step : 1;
        /* Step over values this platform cannot select, for at most one
         * cycle; with nothing else offered the row stays put. */
        next = value;
        for (long remaining = Settings_Maximum(desc) - desc->minval + 1;
             remaining > 0; --remaining) {
          next += direction < 0 ? -step : step;
          if (next < desc->minval) next = Settings_Maximum(desc);
          if (next > Settings_Maximum(desc)) next = desc->minval;
          if (Settings_ValueAvailable(desc, next)) break;
        }
      }
      SaveAcceptedChange(Settings_SetLong(desc, next));
      return;
    }
  }
}

/* Confirm on an editor row. A plane row toggles its parameter block rather than
 * stepping the shape -- stepping is Left/Right, and a confirm that also stepped
 * would make it impossible to expand a plane without changing it. */
static bool LayerActivateSelected(void) {
  if (!ActiveSectionIsCustom()) return false;
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row =
      SelectedLayerRow(rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return true;
  if (row->owner == kLayerMenuRow_ActionBg) {
    ReportActionBgTunerResult(
        ActionBgTuner_Activate(&row->source.action_bg));
    return true;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;

  if (diorama->kind == kDioramaEditorRow_ResetRoom) {
    DioramaLayerOrderTable *table =
        s_layer_table_provider ? s_layer_table_provider() : NULL;
    if (!table) {
      SetStatus(Ui("overlay.status.no_reset_room"));
      return true;
    }
    DioramaLayerOrder_ResetPlaneOverridesSection(
        table, diorama->map_group, diorama->map_number, diorama->section);
    s_layer_plane = -1;
    SetStatus(Ui("overlay.status.planes_reset"));
    LayerSaveEdit();
    return true;
  }

  if (diorama->kind == kDioramaEditorRow_Plane) {
    s_layer_plane = (s_layer_plane == diorama->plane)
        ? -1 : diorama->plane;
    return true;
  }
  if (diorama->param == kDioramaEditorParam_TransparentFill) {
    (void)LayerOpenPalette(diorama);
    return true;
  }
  /* A parameter row: confirm is one fine step up, matching what an Int
   * descriptor row does elsewhere in this menu. */
  return LayerChangeSelected(+1);
}

static void ActivateSelectedRow(void) {
  if (RegionalChangeSelected(1, false, true)) return;
  if (LayerActivateSelected()) return;
  if (SelectedRowIsSectionReset()) {
    ConfirmOrResetActiveSection();
    return;
  }
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus(Ui("overlay.status.unavailable"));
    return;
  }
  switch (desc->type) {
    case kSettingType_Action:  InvokeSelectedAction(); break;
    case kSettingType_Binding: BeginCapture(); break;
    case kSettingType_Mask:
    case kSettingType_Custom:  BeginEditing(); break;
    /* Confirm on a numeric row is a single fine step up, not a text prompt —
     * a discrete nudge with no hold, so it saves immediately. */
    case kSettingType_Int:     (void)StepNumeric(desc, +1, 1, true); break;
    case kSettingType_Bool:
    case kSettingType_Enum:    ChangeSelectedValue(1); break;
  }
}

/* Reset (Y) on an editor row: clear exactly what the row names. A plane row
 * clears the whole plane, a parameter row only its own key -- so backing out one
 * experiment does not discard the rest of the room. */
static bool LayerResetSelected(void) {
  if (!ActiveSectionIsCustom()) return false;
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row =
      SelectedLayerRow(rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return true;
  if (row->owner == kLayerMenuRow_ActionBg) {
    ReportActionBgTunerResult(
        ActionBgTuner_ResetRow(&row->source.action_bg));
    return true;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;
  if (diorama->kind == kDioramaEditorRow_ResetRoom)
    return LayerActivateSelected();

  DioramaPlaneOverride *plane = LayerPlaneForRow(diorama, false);
  if (!plane) {
    SetStatus(Ui("overlay.status.inherited"));
    return true;
  }
  if (diorama->kind == kDioramaEditorRow_Plane) {
    DioramaLayerEditor_ClearPlane(plane);
    SetStatus(Ui("overlay.status.plane_cleared"));
  } else {
    DioramaLayerEditor_ClearParam(plane, diorama->param);
    SetStatus(Ui("overlay.status.cleared"));
  }
  LayerPruneEmptySection(diorama);
  LayerSaveEdit();
  return true;
}

static void ResetSelectedValue(void) {
  if (RegionalChangeSelected(1, true, false)) return;
  if (LayerResetSelected()) return;
  if (SelectedRowIsSectionReset()) {
    ConfirmOrResetActiveSection();
    return;
  }
  const SettingDesc *desc = SelectedDesc();
  if (!desc || !Settings_IsAvailable(desc)) {
    SetStatus(Ui("overlay.status.unavailable"));
    return;
  }
  SaveAcceptedChange(Settings_Reset(desc));
}

static void MoveSection(int direction) {
  EndValueHold();
  ClearSectionResetArm();
  /* Step over hidden sections rather than landing on one. The loop is bounded
   * by the section count and the menu always has at least one visible section,
   * so it always terminates on a real section. */
  int step = direction < 0 ? -1 : 1;
  for (int i = 0; i < kSectionCount; i++) {
    s_section = (s_section + step + kSectionCount) % kSectionCount;
    if (!SectionHidden(s_section)) break;
  }
  s_row = 0;
  s_top_row = 0;
  s_tab_scroll = 0;
  s_layer_plane = -1;
  SettingsOverlay_Refresh();
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

/* True when the row at `index` cannot take the cursor. Layer captions and the
 * System > Game subsection headings are presentation-only rows. */
static bool RowIsUnselectable(int index) {
  if (ActiveTabIsRegional()) return !s_regional_valid;
  if (!ActiveSectionIsCustom()) {
    if (index < 0 || index >= TabSettingRowCount()) return false;
    return RegistryMenuRowAt(index).desc == NULL;
  }
  LayerMenuRow rows[kLayerMenuRowMax];
  int n = LayerMenuRows(rows, kLayerMenuRowMax);
  if (index < 0 || index >= n) return false;
  return !rows[index].selectable;
}

/* Pull the cursor off an unselectable row, forwards. Called wherever the row
 * cursor is (re)seated at 0 -- entering a section or changing tab. */
static void SkipUnselectableRow(void) {
  int count = TabRowCount();
  if (count <= 0) return;
  for (int i = 0; i < count && RowIsUnselectable(s_row); i++)
    s_row = (s_row + 1) % count;
  /* A list that is ALL captions -- a level the player is not in -- leaves the
   * cursor on the caption, which is correct: there is nothing to select, and the
   * row is drawn as a notice rather than as a control. */
}

void SettingsOverlay_Refresh(void) {
  if (!s_open)
    return;
  if (NormalizeNavigation()) {
    EndValueHold();
    StopEditing();
    ClearSectionResetArm();
    s_capture_desc = NULL;
    s_row = 0;
    s_top_row = 0;
    s_tab_scroll = 0;
    s_layer_plane = -1;
  }
  SyncActiveTabPage();
  if (ActiveTabIsRegional())
    s_regional_valid = s_regional_hooks.copy && s_regional_hooks.copy(&s_regional_view);
  RebuildRegistryMenuRows();
  EnsureSelectedNavVisible();
  EnsureSelectedRowVisible();
  SkipUnselectableRow();
  EnsureSelectedRowVisible();
}

static void MoveRow(int direction) {
  int count = TabRowCount();
  if (count <= 0) return;
  EndValueHold();
  ClearSectionResetArm();
  int step = direction < 0 ? -1 : 1;
  /* Step past unselectable rows so the cursor never rests on a caption. Bounded
   * by the row count: a list that is ALL captions (a level the player is not in)
   * leaves the cursor where it was rather than spinning. */
  for (int i = 0; i < count; i++) {
    s_row = (s_row + step + count) % count;
    if (!RowIsUnselectable(s_row)) break;
  }
  EnsureSelectedRowVisible();
}

/* Tabs wrap, like every other list in this menu. Changing tab always resets
 * the row cursor: the two lists have nothing in common, so carrying an index
 * across would land somewhere arbitrary. */
static void MoveTab(int direction) {
  const MenuSection *section = ActiveSection();
  if (VisibleTabCount(s_section) <= 1) return;
  EndValueHold();
  ClearSectionResetArm();
  int candidate = ActiveTabIndex();
  for (int i = 0; i < section->tab_count; i++) {
    candidate = (candidate + direction + section->tab_count) %
                section->tab_count;
    if (!RawTabHidden(s_section, candidate)) break;
  }
  s_tab[s_section] = candidate;
  s_row = 0;
  s_top_row = 0;
  /* A new level tab means a different room, so no plane stays expanded. */
  s_layer_plane = -1;
  StopEditing();
  s_capture_desc = NULL;
  SettingsOverlay_Refresh();
  SkipUnselectableRow();
  EnsureSelectedRowVisible();
}

static void EnterSection(void) {
  ClearSectionResetArm();
  s_submenu_open = true;
  s_row = 0;
  s_top_row = 0;
  SettingsOverlay_Refresh();
  SkipUnselectableRow();
  EnsureSelectedRowVisible();
}

bool SettingsOverlay_Init(ArRenderDevice *render_device, SDL_Window *window,
                          const uint8_t *rom_data, size_t rom_size) {
  s_render_device = ArRenderDevice_IsReady(render_device)
      ? render_device : NULL;
  s_window = window;
  if (!s_render_device) return true;

  return SettingsOverlayArtwork_Init(s_render_device, rom_data, rom_size);
}

bool SettingsOverlay_ReloadTextures(const uint8_t *rom_data, size_t rom_size) {
  if (!s_render_device) return true;
  ArUiTextRenderer_ClearTextures(&s_ui_text);
  SettingsOverlayPalette_ReleaseTexture();
  return SettingsOverlayArtwork_Reload(rom_data, rom_size);
}

void SettingsOverlay_Destroy(void) {
  StopEditing();
  ArUiTextRenderer_Destroy(&s_ui_text);
  SettingsOverlayArtwork_Destroy();
  SettingsOverlayPalette_ReleaseTexture();
  s_render_device = NULL;
  s_window = NULL;
  s_open = false;
  memset(&s_decision, 0, sizeof(s_decision));
  memset(&s_details, 0, sizeof(s_details));
  s_submenu_open = false;
  SettingsOverlayDebugPanel_Reset();
  s_inspector_info_provider = NULL;
  SettingsOverlay_SetRegionalHooks(NULL);
}

bool SettingsOverlay_SetTextBackend(const ArTextBackend *backend,
                                    const ArTextBackendConfig *fonts,
                                    char *error, size_t error_capacity) {
  return ArUiTextRenderer_Init(&s_ui_text, s_render_device, backend, fonts,
                                error, error_capacity);
}

bool SettingsOverlay_IsOpen(void) {
  return s_open;
}

void SettingsOverlay_Open(void) {
  if (s_decision.result == kOverlayDecision_Pending) return;
  s_details.open = false;
  s_regional_preset_choice[0] = s_regional_preset_choice[1] = -1;
  StopEditing();
  EndValueHold();
  ClearSectionResetArm();
  s_capture_desc = NULL;
  s_submenu_open = false;
  s_open = true;
  SettingsOverlay_Refresh();
  s_status[0] = 0;
  fprintf(stderr, "[settings-menu] opened\n");
}

void SettingsOverlay_Close(void) {
  if (!s_open) return;
  s_details.open = false;
  memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
  if (s_decision.result == kOverlayDecision_Pending)
    s_decision.result = kOverlayDecision_Cancelled;
  /* The reader is nested inside this overlay, so closing the overlay closes it
   * too -- otherwise it would still believe it is open, and the next time the
   * menu came up the player would land in the manual instead of the menu. */
  if (s_manual_hooks.close) s_manual_hooks.close();
  StopEditing();
  EndValueHold();
  ClearSectionResetArm();
  s_capture_desc = NULL;
  s_submenu_open = false;
  SettingsOverlayPalette_Close();
  s_open = false;
  fprintf(stderr, "[settings-menu] closed\n");
}

static bool BeginDecision(const char *title,const char *body,const char *accept,bool body_text) {
  if (s_open || s_decision.result != kOverlayDecision_None || !s_render_device ||
      !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->fonts[kText_Normal]) ||
      !title || !body || !accept || !*title || !*body || !*accept ||
      strlen(title) >= sizeof(s_decision.title) || strlen(body) >= (body_text?sizeof(s_decision.body):96) ||
      strlen(accept) >= sizeof(s_decision.accept)) return false;
  SettingsOverlay_Open();
  strcpy(s_decision.title, title);
  strcpy(s_decision.body, body);
  strcpy(s_decision.accept, accept);
  s_decision.accept_selected = false;
  s_decision.notice=false;
  s_decision.body_text=body_text;
  s_decision.result = kOverlayDecision_Pending;
  return true;
}

bool SettingsOverlay_BeginDecision(const char *title,const char *body,const char *accept) {
  return BeginDecision(title,body,accept,false);
}
bool SettingsOverlay_BeginDecisionText(const char *title,const char *body,const char *accept) {
  return BeginDecision(title,body,accept,true);
}
bool SettingsOverlay_BeginNotice(const char *title,const char *body,const char *dismiss) {
  if(!BeginDecision(title,body,dismiss,false))return false;
  s_decision.notice=s_decision.accept_selected=true;
  return true;
}

SettingsOverlayDecisionResult SettingsOverlay_TakeDecisionResult(void) {
  const SettingsOverlayDecisionResult result = s_decision.result;
  if (result == kOverlayDecision_Accepted || result == kOverlayDecision_Cancelled)
    s_decision.result = kOverlayDecision_None;
  return result;
}

const char *SettingsOverlay_SelectedKey(void) {
  if (!s_open) return "";
  if (ActiveTabIsRegional()) return s_regional_valid && SelectedRegionRow()
      ? SelectedRegionRow()->key : "regional_no_campaign";
  /* The layer editor's rows have no descriptor key, so they report a synthesized
   * one: the plane token for a plane row ("bg2hi"), the token plus the parameter
   * for a nested row ("bg2hi.copies"), and a fixed name for the room reset. The
   * point is the same as for descriptor rows -- a test navigates to a row BY
   * NAME instead of counting keypresses, which otherwise breaks every time the
   * list's shape changes (and the shape here changes with the active shape). */
  if (ActiveSectionIsCustom()) {
    static char key[48];
    LayerMenuRow rows[kLayerMenuRowMax];
    const LayerMenuRow *row =
        SelectedLayerRow(rows, kLayerMenuRowMax, NULL);
    if (!row || !row->key[0]) return "";
    snprintf(key, sizeof(key), "%s", row->key);
    return key;
  }
  if (SelectedRowIsSectionReset()) return kSectionResetKey;
  const SettingDesc *desc = SelectedDesc();
  return desc && desc->key ? desc->key : "";
}

bool SettingsOverlay_GetNavigationState(int *selected_ordinal,
                                        int *top_ordinal,
                                        int *visible_rows,
                                        int *total_rows) {
  if (!s_open) return false;
  /* Positions among the VISIBLE sections, matching what the column draws and
   * what top_ordinal counts in.
   *
   * This differs from s_section whenever Manual is absent: System and Layers
   * close the gap left by its raw index. Reporting the raw index there would draw
   * the nav cursor on the wrong row. */
  if (selected_ordinal) *selected_ordinal = ActiveVisibleSectionPosition();
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
static void TickHold(uint64_t now_ms) {
  if (!s_open || !s_hold_desc) return;
  if (!s_submenu_open || s_editing || s_capture_desc ||
      SelectedDesc() != s_hold_desc || !Settings_IsAvailable(s_hold_desc)) {
    EndValueHold();
    return;
  }
  int guard = 0;
  while (now_ms >= s_hold_next_ms && guard++ < 8) {
    long multiplier = HoldStepMultiplier(s_hold_desc, now_ms - s_hold_start_ms);
    if (StepNumeric(s_hold_desc, s_hold_dir, multiplier, false)) {
      EndValueHold();
      return;
    }
    s_hold_next_ms += kHoldRepeatMs;
  }
  /* If a frame hitch left the schedule far in the past, resync rather than
   * firing a long catch-up burst on the next tick. */
  if (s_hold_next_ms + kHoldRepeatMs < now_ms)
    s_hold_next_ms = now_ms + kHoldRepeatMs;
}

void SettingsOverlay_Tick(void) {
  SettingsOverlay_Refresh();
  TickHold(HostClock_Milliseconds());
  SettingsOverlay_Refresh();
}

long SettingsOverlay_HoldStepForTest(const struct SettingDesc *desc,
                                     uint64_t held_ms) {
  return desc ? HoldStepMultiplier((const SettingDesc *)desc, held_ms) : 0;
}

void SettingsOverlay_TickAtForTest(uint64_t now_ms) {
  SettingsOverlay_Refresh();
  TickHold(now_ms);
  SettingsOverlay_Refresh();
}

/* Logical menu commands. Both the keyboard path and the gamepad path funnel
 * through these so the two never drift apart, and so a rebound pad drives the
 * menu with the player's own buttons. */
static void OpenRegionalDetails(void) {
  if (!ActiveTabIsRegional() || !s_submenu_open || !s_regional_valid) return;
  const OverlayRegionRow *row = SelectedRegionRow();
  if (!row) return;
  char help[2048], active[128], current[256] = "";
  SettingsOverlayRegionBadge badge;
  if (!OverlayRegionMenu_Description(InterfaceLocale(), &s_regional_view, row, help, sizeof(help))) return;
  if (!s_regional_view.new_game && OverlayRegionMenu_Value(InterfaceLocale(),
      &s_regional_view, row, true, active, sizeof(active), &badge)) {
    const ArUiTextArgument args[] = {{"region", active}};
    ArUiCatalog_Format(current, sizeof(current), Ui("overlay.region.active"), args, 1);
  }
  const int written = snprintf(s_details.body, sizeof(s_details.body), "%s%s%s\n\n%s\n\n%s",
      OverlayRegionMenu_ImpactLabel(InterfaceLocale(), &s_regional_view, row),
      *current ? "\n" : "", current, help, RegionalNotice());
  if (written < 0 || (size_t)written >= sizeof(s_details.body)) return;
  snprintf(s_details.title, sizeof(s_details.title), "%s", OverlayRegionMenu_Label(InterfaceLocale(), row));
  s_details.top_line = s_details.total_lines = 0;
  s_details.visible_lines = 1;
  s_details.open = true;
  EndValueHold();
}

static void ApplyMenuNav(MenuNav nav, bool repeat) {
  if (s_details.open) {
    const int page = s_details.visible_lines > 1 ? s_details.visible_lines - 1 : 1;
    const int last = s_details.total_lines > s_details.visible_lines
        ? s_details.total_lines - s_details.visible_lines : 0;
    if (nav == kMenuNav_Up) --s_details.top_line;
    else if (nav == kMenuNav_Down) ++s_details.top_line;
    else if (nav == kMenuNav_Left || nav == kMenuNav_TabPrev) s_details.top_line -= page;
    else if (nav == kMenuNav_Right || nav == kMenuNav_TabNext) s_details.top_line += page;
    else if (!repeat && (nav == kMenuNav_Back || nav == kMenuNav_Close ||
                        nav == kMenuNav_Confirm || nav == kMenuNav_Details)) s_details.open = false;
    if (s_details.top_line < 0) s_details.top_line = 0;
    if (s_details.top_line > last) s_details.top_line = last;
    return;
  }
  if (s_regional_confirmation.dialog.result == kOverlayDecision_Pending) {
    if (repeat) return;
    if (nav == kMenuNav_Up || nav == kMenuNav_Down || nav == kMenuNav_Left || nav == kMenuNav_Right) {
      s_regional_confirmation.dialog.accept_selected = !s_regional_confirmation.dialog.accept_selected;
    } else if (nav == kMenuNav_Confirm) {
      const bool apply = s_regional_confirmation.dialog.accept_selected;
      s_regional_confirmation.dialog.result = kOverlayDecision_None;
      if (apply && s_regional_hooks.request) {
        const OverlayRegionRow *row = s_regional_confirmation.row;
        const ActRaiserRegionalEditResult result = row->kind == kOverlayRegionRow_Setting
            ? s_regional_hooks.setting(&s_regional_confirmation.view, row->setting, s_regional_confirmation.source)
            : s_regional_hooks.request(&s_regional_confirmation.view, row->group, s_regional_confirmation.source);
        RegionalReportResult(result);
      }
      SettingsOverlay_Refresh();
    } else if (nav == kMenuNav_Back || nav == kMenuNav_Close) {
      memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
    }
    return;
  }
  if (s_decision.result == kOverlayDecision_Pending) {
    if (repeat) return;
    if (nav == kMenuNav_Up || nav == kMenuNav_Down || nav == kMenuNav_Left || nav == kMenuNav_Right) {
      if(!s_decision.notice)s_decision.accept_selected = !s_decision.accept_selected;
    }
    else if (nav == kMenuNav_Confirm) {
      s_decision.result = s_decision.accept_selected ? kOverlayDecision_Accepted : kOverlayDecision_Cancelled;
      SettingsOverlay_Close();
    } else if (nav == kMenuNav_Back || nav == kMenuNav_Close) SettingsOverlay_Close();
    return;
  }
  if (SettingsOverlayPalette_ApplyNav(nav, repeat)) return;
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
    case kMenuNav_Details: if (!repeat) OpenRegionalDetails(); break;
    case kMenuNav_Back:
      if (!repeat) {
        EndValueHold();
        ClearSectionResetArm();
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
  SettingsOverlay_Refresh();
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
  SettingsOverlay_Refresh();
  /* Before capture, so a pad press while reading pages the manual instead of
   * being interpreted as menu navigation underneath it. */
  if (ManualIsOpen() && s_manual_hooks.handle_pad &&
      s_manual_hooks.handle_pad(event))
    return true;
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
    case kInputAction_X:      ApplyMenuNav(kMenuNav_Details, false); break;
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
  return (int)SDL_GetScancodeFromKey(key, NULL) ==
      (int)INPUT_BIND_CODE(binding);
}

/* Maps a keycode to a menu command through the player's OWN keyboard bindings,
 * so rebinding B/A/X/Y/L/R or a direction moves those controls in the menu too —
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
    { kInputAction_X,     kMenuNav_Details },
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
  SettingsOverlay_Refresh();
  /* The reader is modal while it is up, so it gets first refusal on every key.
   * It declines exactly the ones that must never be captured -- F1, which closes
   * the menu outright -- so there is always a key that exits, whatever state the
   * reader has got itself into. */
  if (ManualIsOpen() && s_manual_hooks.handle_key &&
      s_manual_hooks.handle_key(key, pressed, repeat))
    return true;
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
        SetStatus(Ui("overlay.status.edit_cancelled"));
        break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (!repeat) CommitEditing();
        break;
      case SDLK_BACKSPACE: {
        ArInterfaceText_EraseLast(s_edit_buffer, sizeof(s_edit_buffer));
        break;
      }
      default:
        break;
    }
    return true;
  }

  /* Universal keyboard controls, deliberately fixed and independent of the
   * game bindings: arrows navigate, Enter confirms, Esc/F1 close, [/]+Tab
   * cycle tabs. These alone fully operate the menu, which matters because the
   * menu is the only place to repair a broken binding — so it must stay usable
   * even if the player has unbound or mangled their SNES keys. Matched by
   * SCANCODE (physical position) not keycode, so the arrow/Enter/Esc positions
   * navigate the menu the same way on a non-US layout (AZERTY etc.). */
  SDL_Scancode sc = SDL_GetScancodeFromKey(key, NULL);
  switch (sc) {
    case SDL_SCANCODE_F3:
      ApplyMenuNav(kMenuNav_Details, repeat);
      break;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_F1:
      ApplyMenuNav(kMenuNav_Close, repeat);
      break;
    case SDL_SCANCODE_UP:
      ApplyMenuNav(kMenuNav_Up, repeat);
      break;
    case SDL_SCANCODE_DOWN:
      ApplyMenuNav(kMenuNav_Down, repeat);
      break;
    case SDL_SCANCODE_LEFT:
      ApplyMenuNav(kMenuNav_Left, repeat);
      break;
    case SDL_SCANCODE_RIGHT:
      ApplyMenuNav(kMenuNav_Right, repeat);
      break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      ApplyMenuNav(kMenuNav_Confirm, repeat);
      break;
    case SDL_SCANCODE_LEFTBRACKET:
      ApplyMenuNav(kMenuNav_TabPrev, repeat);
      break;
    case SDL_SCANCODE_RIGHTBRACKET:
    case SDL_SCANCODE_TAB:
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
  ArInterfaceText_Append(s_edit_buffer, sizeof(s_edit_buffer), text, strlen(text));
  return true;
}

static int ScalePosition(int position, int scale_percent) {
  return (position * scale_percent + kPercentScale / 2) /
      kPercentScale;
}

ArRenderRectI LogicalRect(const MenuLayout *layout,
                          int x, int y, int width, int height) {
  int x0 = layout->origin_x + ScalePosition(x, layout->scale_percent);
  int y0 = layout->origin_y + ScalePosition(y, layout->scale_percent);
  int x1 = layout->origin_x +
           ScalePosition(x + width, layout->scale_percent);
  int y1 = layout->origin_y +
           ScalePosition(y + height, layout->scale_percent);
  return (ArRenderRectI){ x0, y0, x1 - x0, y1 - y0 };
}

static bool FillPixelRectChecked(int x, int y, int width, int height,
                                 uint32_t color) {
  if (width <= 0 || height <= 0) return true;
  const ArRenderRectF rect = {
    (float)x, (float)y, (float)width, (float)height,
  };
  return ArRenderDevice_DrawSolidRect(
      s_render_device, &rect, RenderColor(color), kArRenderBlendMode_Alpha);
}

static void FillPixelRect(int x, int y, int width, int height,
                          uint32_t color) {
  if (width <= 0 || height <= 0) return;
  const ArRenderRectF rect = {
    (float)x, (float)y, (float)width, (float)height,
  };
  (void)ArRenderDevice_DrawSolidRect(
      s_render_device, &rect, RenderColor(color), kArRenderBlendMode_Alpha);
}

void FillLogicalRect(const MenuLayout *layout,
                            int x, int y, int width, int height,
                            uint32_t color) {
  ArRenderRectI rect = LogicalRect(layout, x, y, width, height);
  FillPixelRect(rect.x, rect.y, rect.w, rect.h, color);
}

static bool DrawDialogTileChecked(const MenuLayout *layout, int atlas_column,
                                  int atlas_row, int x, int y) {
  ArRenderRectI source = {
    atlas_column * kGlyphSize,
    atlas_row * kGlyphSize,
    kGlyphSize,
    kGlyphSize,
  };
  const ArRenderRectF destination =
      ToRenderRect(LogicalRect(layout, x, y, kGlyphSize, kGlyphSize));
  const ArRenderRectF source_f = ToRenderRect(source);
  return ArRenderDevice_DrawTexture(
      s_render_device, SettingsOverlayArtwork_Get()->dialog_frame, &source_f, &destination);
}

static bool DrawDialogPanelChecked(const MenuLayout *layout,
                                   int x, int y, int width, int height) {
  if (width < 16 || height < 16) return false;
  if (!ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->dialog_frame)) {
    const ArRenderRectI outer = LogicalRect(layout, x, y, width, height);
    const ArRenderRectI middle = LogicalRect(
        layout, x + 2, y + 2, width - 4, height - 4);
    const ArRenderRectI inner = LogicalRect(
        layout, x + 4, y + 4, width - 8, height - 8);
    return FillPixelRectChecked(
               outer.x, outer.y, outer.w, outer.h, kFrameDark) &&
        FillPixelRectChecked(
               middle.x, middle.y, middle.w, middle.h, kFrameLight) &&
        FillPixelRectChecked(
               inner.x, inner.y, inner.w, inner.h, kPanel);
  }

  const ArRenderRectI inner = LogicalRect(
      layout, x + kGlyphSize, y + kGlyphSize,
      width - kGlyphSize * 2, height - kGlyphSize * 2);
  if (!FillPixelRectChecked(
          inner.x, inner.y, inner.w, inner.h, kPanel))
    return false;
  for (int tile_x = x + kGlyphSize;
       tile_x < x + width - kGlyphSize; tile_x += kGlyphSize) {
    if (!DrawDialogTileChecked(layout, 1, 0, tile_x, y) ||
        !DrawDialogTileChecked(layout, 1, 2, tile_x,
                               y + height - kGlyphSize))
      return false;
  }
  for (int tile_y = y + kGlyphSize;
       tile_y < y + height - kGlyphSize; tile_y += kGlyphSize) {
    if (!DrawDialogTileChecked(layout, 0, 1, x, tile_y) ||
        !DrawDialogTileChecked(layout, 2, 1,
                               x + width - kGlyphSize, tile_y))
      return false;
  }
  return DrawDialogTileChecked(layout, 0, 0, x, y) &&
      DrawDialogTileChecked(
          layout, 2, 0, x + width - kGlyphSize, y) &&
      DrawDialogTileChecked(
          layout, 0, 2, x, y + height - kGlyphSize) &&
      DrawDialogTileChecked(
          layout, 2, 2, x + width - kGlyphSize,
          y + height - kGlyphSize);
}

void DrawDialogPanel(const MenuLayout *layout,
                     int x, int y, int width, int height) {
  (void)DrawDialogPanelChecked(layout, x, y, width, height);
}

bool SettingsOverlay_DrawGameFrame(ArRenderRectI rect, int scale) {
  if (!s_render_device || scale <= 0) return false;
  const int tile_size = kGlyphSize * scale;
  if (rect.w <= 0 || rect.h <= 0 ||
      rect.w % tile_size != 0 || rect.h % tile_size != 0)
    return false;
  const MenuLayout layout = {
    .output_width = rect.w,
    .output_height = rect.h,
    .scale_percent = scale * kPercentScale,
    .logical_width = rect.w / scale,
    .logical_height = rect.h / scale,
    .origin_x = rect.x,
    .origin_y = rect.y,
  };
  return DrawDialogPanelChecked(
      &layout, 0, 0, layout.logical_width, layout.logical_height);
}

static void DrawGlyph(const MenuLayout *layout, int x, int y,
                      unsigned char ch, TextStyle style) {
  if (ch == ' ') return;
  if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) ch = '?';
  if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) return;
  const ArRenderTexture texture = SettingsOverlayArtwork_Get()->fonts[style];
  if (!ArRenderTexture_IsValid(texture)) return;
  const ArRenderRectF source = {
    (float)((ch & 15) * kGlyphSize),
    (float)((ch >> 4) * kGlyphSize),
    (float)kGlyphSize,
    (float)kGlyphSize,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(
      layout, x, y, kGlyphSize, kGlyphSize));
  (void)ArRenderDevice_DrawTexture(
      s_render_device, texture, &source, &destination);
}

/* ── Character cells ───────────────────────────────────────────────────────
 *
 * Package names come from whoever authored the pack, in whatever script, so
 * the overlay is handed UTF-8. The compatibility font atlas is ASCII-only,
 * but the counting must be right with either renderer: one cell
 * per extended grapheme cluster, not per byte. A name with an accent then
 * occupies the cells a reader would count, right alignment lands where it
 * should, and truncation can never cut a character in half.
 *
 * The fallback path draws a cluster that is a single ASCII byte; anything else draws
 * the replacement, so a precomposed and a decomposed accent look the same
 * rather than one silently losing its mark. Nothing is transliterated. The
 * package ID in the same row is ASCII by construction and stays readable
 * while a name has no glyphs. The host-injected font path instead shapes the
 * complete bounded UTF-8 run, preserving accents and contextual joining. */
static size_t OverlayNextCell(const char *text, size_t bytes, size_t offset,
                              unsigned char *glyph) {
  uint32_t first = 0;
  size_t next = 0;
  if (!ArUnicodeGrapheme_Next(text, bytes, offset, &first, &next) ||
      next <= offset) {
    /* Malformed bytes cost one cell each and never stop the row: a package
     * name with one bad byte must still be readable and selectable, not
     * vanish. Deciding a cluster boundary reads the following scalar, so a
     * bad byte fails its predecessor too -- recovering a byte at a time is
     * what keeps that local. */
    *glyph = (unsigned char)'?';
    return offset + 1u;
  }
  *glyph = next - offset == 1u && first >= 0x20u && first < 0x80u
      ? (unsigned char)first : (unsigned char)'?';
  return next;
}

/* Cells `text` occupies, at most `maximum`. */
static int OverlayCellCount(const char *text, int maximum) {
  if (!text || maximum <= 0) return 0;
  const size_t bytes = strlen(text);
  int cells = 0;
  for (size_t offset = 0; offset < bytes && cells < maximum;) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    offset = next;
    ++cells;
  }
  return cells;
}

/* Keep English's authentic atlases unchanged. Any non-ASCII run (including a
 * package name) goes through the independently owned interface font stack.
 * Grapheme boundaries retain the existing cell-budget/truncation contract;
 * shaping is whole-run, never a collection of isolated Unicode code points. */
static bool MakeUnicodeTextRun(const MenuLayout *layout, int x, int y,
                             const char *text, int max_chars, int cell_width,
                             ArUiTextAlignment alignment,
                             int style, uint32_t tint, ArUiTextRun *out_run) {
  if (!text || max_chars <= 0 || !ArUiTextRenderer_IsReady(&s_ui_text)) return false;
  const size_t bytes = strlen(text);
  /* ASCII remains the common path. Don't repeat the grapheme walk for it. */
  bool candidate = InterfaceLocale() != kArUiLocale_English;
  for (size_t i = 0; i < bytes; ++i)
    if ((unsigned char)text[i] >= 0x80) { candidate = true; break; }
  if (!candidate) return false;
  size_t end = 0;
  int cells = 0;
  bool non_ascii = InterfaceLocale() != kArUiLocale_English;
  while (end < bytes && cells < max_chars) {
    unsigned char glyph;
    const size_t next = OverlayNextCell(text, bytes, end, &glyph);
    if (next > kArInterfaceTextMaximumBytes) return false;
    for (size_t i = end; i < next; ++i)
      if ((unsigned char)text[i] >= 0x80) non_ascii = true;
    end = next;
    ++cells;
  }
  if (!non_ascii) return false;
  ArUiTextRun run = {
    .struct_size = sizeof(run), .abi_version = AR_UI_TEXT_RUN_ABI_VERSION,
    .utf8 = text, .utf8_bytes = end,
    .bounds = LogicalRect(layout, x, y, cells * cell_width, kGlyphSize),
    .alignment = alignment, .tint = RenderColor(tint),
    .language_bcp47 = ArUiCatalog_LocaleTag(InterfaceLocale()),
  };
  if (style >= 0 && style < kTextStyle_Count) {
    run.style_id = kArTextStyle_RetailPaletteBands;
    run.band_rgb = kTextPalettes[style][2] & UINT32_C(0xffffff);
    run.body_rgb = kTextPalettes[style][3] & UINT32_C(0xffffff);
    run.shadow_rgb = kTextPalettes[style][1] & UINT32_C(0xffffff);
    run.shadow_enabled = true;
  }
  *out_run = run;
  return true;
}

static bool DrawUnicodeText(const MenuLayout *layout, int x, int y,
                             const char *text, int max_chars, int cell_width,
                             ArUiTextAlignment alignment,
                             int style, uint32_t tint) {
  ArUiTextRun run;
  return MakeUnicodeTextRun(layout, x, y, text, max_chars, cell_width,
                             alignment, style, tint, &run) &&
      ArUiTextRenderer_Draw(&s_ui_text, &run);
}

static void DrawTextN(const MenuLayout *layout, int x, int y,
                      const char *text, int max_chars, TextStyle style) {
  if (!text || max_chars <= 0) return;
  if (DrawUnicodeText(layout, x, y, text, max_chars, kGlyphSize,
                        kArUiTextAlignment_Left, style,
                        UINT32_C(0xffffffff))) return;
  const size_t bytes = strlen(text);
  int cell = 0;
  for (size_t offset = 0; offset < bytes && cell < max_chars; ++cell) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    DrawGlyph(layout, x + cell * kGlyphSize, y, glyph, style);
    offset = next;
  }
}

/* ── The game font at output-pixel coordinates ─────────────────────────────
 *
 * For a nested fullscreen mode -- the manual reader -- which is not laid out on
 * the menu's logical grid and so cannot go through DrawTextN's MenuLayout. Same
 * atlas, same glyphs, same colors; only the coordinate space differs. */

int SettingsOverlay_GameTextWidth(const char *text, int scale) {
  if (!text || scale <= 0) return 0;
  if (scale <= kMaximumScalePercent / kPercentScale) {
    const MenuLayout layout = {.scale_percent = scale * kPercentScale};
    ArUiTextRun run;
    int width;
    if (MakeUnicodeTextRun(&layout, 0, 0, text, INT32_MAX, kGlyphSize,
          kArUiTextAlignment_Left, kText_Normal,
          UINT32_C(0xffffffff), &run) &&
        ArUiTextRenderer_Measure(&s_ui_text, &run, &width, NULL)) return width;
  }
  return OverlayCellCount(text, INT32_MAX) * kGlyphSize * scale;
}

void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha,
                                  const char *text) {
  enum {
    kFontAtlasCellsPerAxis = 16,
    kGameTextGlyphBatchCapacity = 64,
    kVerticesPerGlyph = 4,
    kIndicesPerGlyph = 6,
  };
  if (!text || scale <= 0 || alpha == 0) return;
  if (scale <= kMaximumScalePercent / kPercentScale) {
    const MenuLayout layout = {.scale_percent = scale * kPercentScale,
                               .origin_x = x, .origin_y = y};
    if (DrawUnicodeText(&layout, 0, 0, text, INT32_MAX,
          kGlyphSize, kArUiTextAlignment_Left, kText_Normal,
          ARGB(alpha, 255, 255, 255))) return;
  }
  const ArRenderTexture texture = SettingsOverlayArtwork_Get()->fonts[kText_Normal];
  if (!s_render_device || !ArRenderTexture_IsValid(texture)) return;

  static int32_t indices[
      kGameTextGlyphBatchCapacity * kIndicesPerGlyph];
  static bool indices_initialized;
  if (!indices_initialized) {
    for (int glyph = 0; glyph < kGameTextGlyphBatchCapacity; glyph++) {
      const int vertex = glyph * kVerticesPerGlyph;
      const int at = glyph * kIndicesPerGlyph;
      indices[at + 0] = vertex + 0;
      indices[at + 1] = vertex + 1;
      indices[at + 2] = vertex + 2;
      indices[at + 3] = vertex + 0;
      indices[at + 4] = vertex + 2;
      indices[at + 5] = vertex + 3;
    }
    indices_initialized = true;
  }

  ArRenderVertex2D vertices[
      kGameTextGlyphBatchCapacity * kVerticesPerGlyph];
  int glyph_count = 0;
  const float uv_cell = 1.0f / (float)kFontAtlasCellsPerAxis;
  const float glyph_pixels = (float)(kGlyphSize * scale);
  const ArRenderColorF white = {
    1.0f, 1.0f, 1.0f, (float)alpha / 255.0f,
  };
  const ArRenderDrawState draw_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_Alpha,
  };
  const size_t text_bytes = strlen(text);
  int cell = -1;
  for (size_t offset = 0; offset < text_bytes;) {
    unsigned char ch = 0;
    const size_t next = OverlayNextCell(text, text_bytes, offset, &ch);
    if (!next) break;
    offset = next;
    ++cell;
    if (ch == ' ') continue;
    if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) ch = '?';
    if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) continue;

    if (glyph_count == kGameTextGlyphBatchCapacity) {
      (void)ArRenderDevice_DrawGeometryWithState(
          s_render_device, texture, vertices,
          glyph_count * kVerticesPerGlyph, indices,
          glyph_count * kIndicesPerGlyph, &draw_state);
      glyph_count = 0;
    }

    const float x0 = (float)(x + cell * kGlyphSize * scale);
    const float y0 = (float)y;
    const float x1 = x0 + glyph_pixels;
    const float y1 = y0 + glyph_pixels;
    const float u0 = (float)(ch & 15) * uv_cell;
    const float v0 = (float)(ch >> 4) * uv_cell;
    const float u1 = u0 + uv_cell;
    const float v1 = v0 + uv_cell;
    ArRenderVertex2D *quad = &vertices[glyph_count * kVerticesPerGlyph];
    quad[0] = (ArRenderVertex2D){{x0, y0}, white, {u0, v0}};
    quad[1] = (ArRenderVertex2D){{x1, y0}, white, {u1, v0}};
    quad[2] = (ArRenderVertex2D){{x1, y1}, white, {u1, v1}};
    quad[3] = (ArRenderVertex2D){{x0, y1}, white, {u0, v1}};
    glyph_count++;
  }
  if (glyph_count > 0)
    (void)ArRenderDevice_DrawGeometryWithState(
        s_render_device, texture, vertices,
        glyph_count * kVerticesPerGlyph, indices,
        glyph_count * kIndicesPerGlyph, &draw_state);
}

static int CappedTextLength(const char *text, int max_chars) {
  return OverlayCellCount(text, max_chars);
}

static void DrawTextRight(const MenuLayout *layout, int right, int y,
                          const char *text, int max_chars, TextStyle style) {
  int length = CappedTextLength(text, max_chars);
  if (DrawUnicodeText(layout, right - length * kGlyphSize, y, text, length,
                        kGlyphSize, kArUiTextAlignment_Right, style,
                        UINT32_C(0xffffffff))) return;
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
                           unsigned char ch, uint32_t color) {
  if (ch == ' ' || !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font)) return;
  const ArRenderRectF source = {
    (float)((ch & 15) * kDebugGlyphWidth),
    (float)((ch >> 4) * kDebugGlyphHeight),
    (float)kDebugGlyphWidth,
    (float)kDebugGlyphHeight,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(
      layout, x, y, kDebugGlyphWidth, kDebugGlyphHeight));
  (void)ArRenderDevice_DrawTextureTinted(
      s_render_device, SettingsOverlayArtwork_Get()->debug_font, &source, &destination,
      RenderColor(color));
}

void DrawSmallTextN(const MenuLayout *layout, int x, int y,
                           const char *text, int max_chars, uint32_t color) {
  if (!text || max_chars <= 0 ||
      !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font)) return;
  if (DrawUnicodeText(layout, x, y, text, max_chars, kDebugGlyphWidth,
                        kArUiTextAlignment_Left, -1, color)) return;
  const size_t bytes = strlen(text);
  int cell = 0;
  for (size_t offset = 0; offset < bytes && cell < max_chars; ++cell) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    DrawSmallGlyph(layout, x + cell * kDebugGlyphWidth, y, glyph, color);
    offset = next;
  }
}

void DrawSmallText(const MenuLayout *layout, int x, int y,
                          const char *text, uint32_t color) {
  DrawSmallTextN(layout, x, y, text, 512, color);
}

static int SmallTextWidth(const char *text) {
  return OverlayCellCount(text, INT32_MAX) * kDebugGlyphWidth;
}

/* Icons are authored at 16x16 but drawn at whatever `size` the caller wants;
 * nearest-neighbour keeps integer multiples crisp. `selected` picks the
 * colored game palette (the highlighted slot) over the grey one, and `alpha`
 * fades an unselected, un-focused nav row so it reads as recessive. */
static void DrawSectionIcon(const MenuLayout *layout, int x, int y, int size,
                            int section, bool selected, int alpha) {
  if (!ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->icons) ||
      section < 0 || section >= kSectionCount) return;
  const ArRenderRectF source = {
    (float)(kSections[section].icon * kIconSize), selected ? (float)kIconSize : 0.0f,
    (float)kIconSize, (float)kIconSize,
  };
  const ArRenderRectF destination = ToRenderRect(
      LogicalRect(layout, x, y, size, size));
  (void)ArRenderDevice_DrawTextureTinted(
      s_render_device, SettingsOverlayArtwork_Get()->icons, &source, &destination,
      (ArRenderColorF){1.0f, 1.0f, 1.0f, (float)alpha / 255.0f});
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
  if (ch == ' ' || !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font)) return;
  if (ch >= 128 || !SettingsOverlayArtwork_HasDebugGlyph(ch)) {
    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
    else ch = '?';
  }
  const ArRenderRectF source = {
    (float)((ch & 15) * kDebugGlyphWidth),
    (float)((ch >> 4) * kDebugGlyphHeight),
    (float)kDebugGlyphWidth,
    (float)kDebugGlyphHeight,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(
      layout, x, y, kDebugGlyphWidth, kDebugGlyphHeight));
  (void)ArRenderDevice_DrawTextureTinted(
      s_render_device, SettingsOverlayArtwork_Get()->debug_font, &source, &destination,
      RenderColor(kDebugTextColors[style]));
}

void DrawDebugTextN(const MenuLayout *layout, int x, int y,
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

void DrawDebugHighlightedLine(const MenuLayout *layout,
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
  if (max_chars <= 0 || !cursor) return 0;
  size_t remaining = strlen(cursor);
  int line = 0;
  for (; line < max_lines && remaining; line++) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(cursor, remaining, (size_t)max_chars,
                                   sizeof(buffer) - 1, &slice)) break;
    memcpy(buffer, cursor, slice.bytes);
    buffer[slice.bytes] = 0;
    DrawSmallText(layout, x, y + line * kSmallLineHeight, buffer, color);
    cursor += slice.consumed;
    remaining -= slice.consumed;
  }
  return line;
}

/* Compact preview never grows the footer. An ellipsis points to Details. */
static void DrawSmallTextPreview(const MenuLayout *layout, int x, int y,
    const char *text, int columns, int lines, uint32_t color) {
  if (!text || columns < 4) return;
  if (columns > 127) columns = 127;
  size_t remaining = strlen(text);
  for (int line = 0; line < lines && remaining; ++line) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(text, remaining, columns, sizeof(buffer) - 4, &slice) || !slice.consumed) break;
    const bool truncated = line == lines - 1 && slice.consumed < remaining;
    if (truncated && !ArInterfaceText_WrapLine(text, remaining, columns - 3, sizeof(buffer) - 4, &slice)) break;
    memcpy(buffer, text, slice.bytes);
    strcpy(buffer + slice.bytes, truncated ? "..." : "");
    DrawSmallText(layout, x, y + line * kSmallLineHeight, buffer, color);
    text += slice.consumed;
    remaining -= slice.consumed;
  }
}

static void DrawDetails(const MenuLayout *layout) {
  const int x = 8, y = 8;
  const int width = (layout->logical_width - 16) / 8 * 8;
  const int height = (layout->logical_height - 16) / 8 * 8;
  int columns = (width - 40) / kDebugGlyphWidth;
  if (columns > 127) columns = 127;
  if (columns < 1) return;
  DrawDialogPanel(layout, x, y, width, height);
  DrawWrappedSmallText(layout, x + 16, y + 12, s_details.title, columns, 2, kGameGold);
  FillLogicalRect(layout, x + 16, y + 33, width - 32, 1, kSteelDim);
  s_details.visible_lines = (height - 66) / kSmallLineHeight;
  if (s_details.visible_lines < 1) s_details.visible_lines = 1;
  s_details.total_lines = 0;
  const size_t length = strlen(s_details.body);
  size_t at = 0;
  while (at < length) {
    ArInterfaceTextLine slice;
    if (!ArInterfaceText_WrapLine(s_details.body + at, length - at, columns,
                                  kArInterfaceTextMaximumBytes, &slice) || !slice.consumed) break;
    at += slice.consumed;
    ++s_details.total_lines;
  }
  const int last = s_details.total_lines > s_details.visible_lines
      ? s_details.total_lines - s_details.visible_lines : 0;
  if (s_details.top_line > last) s_details.top_line = last;
  at = 0;
  for (int line = 0; at < length && line < s_details.top_line + s_details.visible_lines; ++line) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(s_details.body + at, length - at, columns, sizeof(buffer) - 1, &slice) || !slice.consumed) break;
    if (line >= s_details.top_line) {
      memcpy(buffer, s_details.body + at, slice.bytes);
      buffer[slice.bytes] = 0;
      DrawSmallText(layout, x + 16, y + 40 + (line - s_details.top_line) * kSmallLineHeight, buffer, kSteelBlue);
    }
    at += slice.consumed;
  }
  DrawScrollBar(layout, x + width - 13, y + 40, s_details.visible_lines * kSmallLineHeight,
      s_details.total_lines, s_details.visible_lines, s_details.top_line, kSteelBlue);
  DrawSmallTextPreview(layout, x + 16, y + height - 17, Ui("overlay.details.controls"),
      (width - 32) / kDebugGlyphWidth, 1, kMutedText);
}

static void DrawDecision(const MenuLayout *layout, const OverlayDecision *decision) {
  const int width = (layout->logical_width < 496 ? layout->logical_width - 32 : 464) / 8 * 8;
  const int height = (layout->logical_height < 288 ? layout->logical_height - 32 : 256) / 8 * 8;
  const int x = (layout->logical_width - width) / 2;
  const int y = (layout->logical_height - height) / 2;
  FillLogicalRect(layout, 0, 0, layout->logical_width, layout->logical_height, ARGB(180, 0, 0, 0));
  DrawDialogPanel(layout, x, y, width, height);
  DrawWrappedSmallText(layout, x + 16, y + 12, Ui(decision->title),
                       (width - 32) / kDebugGlyphWidth, 2, kGameGold);
  DrawWrappedSmallText(layout, x + 16, y + 36, decision->body_text?decision->body:Ui(decision->body),
                       (width - 32) / kDebugGlyphWidth,
                       (height - 100) / kSmallLineHeight, kSteelBlue);
  const int choices_y = y + height - 48;
  for (unsigned n = 0; n < (decision->notice?1u:2u); ++n) {
    const bool selected = decision->accept_selected == (n == 0);
    if (selected) FillLogicalRect(layout, x + 8, choices_y + n * 16 - 2, width - 16, 14, kPanel);
    DrawSmallText(layout, x + 16, choices_y + n * 16, selected ? ">" : " ", kSelectYellow);
    DrawSmallText(layout, x + 30, choices_y + n * 16,
                   Ui(n ? "overlay.decision.cancel" : decision->accept), selected ? kSelectYellow : kSteelBlue);
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
    DrawDebugTextN(layout, x, y + row * kSmallLineHeight, line, length,
                   kDebugText_Normal);
    line = end ? end + 1 : NULL;
  }
}

int SnappedFitScale(int output_width, int output_height) {
  int fit_x = output_width * kPercentScale / kMinimumLayoutWidth;
  int fit_y = output_height * kPercentScale / kMinimumLayoutHeight;
  int fit = fit_x < fit_y ? fit_x : fit_y;
  fit = (fit / kScaleStepPercent) * kScaleStepPercent;
  if (fit < kMinimumScalePercent) fit = kMinimumScalePercent;
  if (fit > kMaximumScalePercent) fit = kMaximumScalePercent;
  return fit;
}

MenuLayout BuildLayoutAtScale(int output_width, int output_height,
                                     int scale) {
  int logical_width = output_width * kPercentScale / scale;
  int logical_height = output_height * kPercentScale / scale;
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

MenuLayout BuildLayout(int output_width, int output_height) {
  int fit_scale = SnappedFitScale(output_width, output_height);
  s_auto_menu_scale_percent = fit_scale;
  /* A pinned percentage is in source-pixels-per-OUTPUT-pixel terms, and the
   * output is physical pixels under SDL_WINDOW_HIGH_PIXEL_DENSITY — scale it by
   * the display's pixel density so a saved 200% looks the same size on a
   * Retina panel as it did before the flag existed. The auto fit_scale is
   * already derived from the output size, so it must NOT be scaled. */
  int scale = g_settings.menu_scale_percent > 0
      ? Settings_ScalePercentToOutput(g_settings.menu_scale_percent)
      : fit_scale;
  if (scale > fit_scale) scale = fit_scale;
  if (scale < kMinimumScalePercent) scale = kMinimumScalePercent;
  return BuildLayoutAtScale(output_width, output_height, scale);
}

static int SnapPanelEdge(int origin, int edge) {
  return origin + ((edge - origin) / kGlyphSize) * kGlyphSize;
}

/* Fixed panel geometry for one DrawMenu pass, computed once and handed to each
 * section renderer. Section chrome draws in the game steel-blue (structure /
 * structure_dim = kSteelBlue / kSteelDim); the cursor/selection uses menu
 * yellow. Extracted from the former single 560-line DrawMenu; the section
 * bodies are unchanged. */
typedef struct MenuChrome {
  int margin;
  int panel_right;
  int top_y, top_height;
  int bottom_y, bottom_height;
  int left_x, left_width;
  int right_x, right_width;
  int bottom_width;
  int right_text_x;
  int value_right, scroll_x;
} MenuChrome;

static MenuChrome ComputeMenuChrome(const MenuLayout *layout) {
  const int margin = 8;
  const int gap = 8;
  const int left_width = 152;
  /* Stable across sections; longer explanations use the details reader. */
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
  MenuChrome c = {
      .margin = margin,
      .panel_right = panel_right,
      .top_y = top_y,
      .top_height = top_height,
      .bottom_y = bottom_y,
      .bottom_height = bottom_height,
      .left_x = left_x,
      .left_width = left_width,
      .right_x = right_x,
      .right_width = right_width,
      .bottom_width = bottom_width,
      .right_text_x = right_x + 12,
      .value_right = right_x + right_width - 16,
      .scroll_x = right_x + right_width - 13,
  };
  return c;
}

static void DrawMenuNavColumn(const MenuLayout *layout, const MenuChrome *c) {
  const int left_x = c->left_x;
  const int left_width = c->left_width;
  const int top_y = c->top_y;
  const int top_height = c->top_height;
  const uint32_t structure = kSteelBlue;

  /* ── Nav column ──────────────────────────────────────────────────────── */
  const int left_text_x = left_x + 10;
  const int left_title_y = top_y + 10;
  const int nav_first_y = top_y + 18;

  DrawSmallText(layout, left_text_x, left_title_y - 3, Ui("overlay.title"),
                ARGB(255, 132, 154, 174));
  FillLogicalRect(layout, left_text_x, left_title_y + 6,
                  left_width - 20, 1, ARGB(120, 120, 150, 178));

  s_nav_visible_rows =
      (top_y + top_height - 6 - nav_first_y) / kNavRowHeight;
  if (s_nav_visible_rows < 1) s_nav_visible_rows = 1;
  EnsureSelectedNavVisible();

  /* `section` walks every entry; `slot` counts only the drawn ones, so a hidden
   * section leaves no gap in the column. The icon atlas is still indexed by the
   * raw section, since it is built from the same table. */
  int slot = -1;
  for (int section = 0; section < kSectionCount; section++) {
    if (SectionHidden(section)) continue;
    slot++;
    if (slot < s_nav_top_row ||
        slot >= s_nav_top_row + s_nav_visible_rows)
      continue;
    int row_y = nav_first_y + (slot - s_nav_top_row) * kNavRowHeight;
    bool current = section == s_section;
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
    DrawSectionIcon(layout, left_text_x, row_y, kIconSize, section,
                    current, current ? 255 : 205);
    const int label_x = left_text_x + kIconSize + 4;
    const int label_chars = (left_x + left_width - 16 - label_x) / kGlyphSize;
    DrawTextN(layout, label_x, row_y + 4,
              Ui(kSections[section].navigation_label ? kSections[section].navigation_label : kSections[section].label), label_chars,
              current ? kText_Normal : kText_Dim);
  }
  DrawScrollBar(layout, left_x + left_width - 12, nav_first_y,
                s_nav_visible_rows * kNavRowHeight, VisibleSectionCount(),
                s_nav_visible_rows, s_nav_top_row, structure);

}

static int DrawMenuHeader(const MenuLayout *layout, const MenuChrome *c,
                          const MenuSection *section) {
  const int right_x = c->right_x;
  const int right_width = c->right_width;
  const int top_y = c->top_y;
  const int right_text_x = c->right_text_x;
  const int value_right = c->value_right;
  const uint32_t structure_dim = kSteelDim;

  /* ── Submenu header: section title, status, tab bar ───────────────────── */
  const int right_title_y = top_y + 8;
  /* The value column stops short of the frame so the scrollbar has a gutter
   * of its own instead of overlapping a value. */

  /* The submenu header is always the active section, so its icon takes the
   * colored selected palette. */
  DrawSectionIcon(layout, right_text_x, right_title_y - 2, kIconSize,
                  s_section, true, 255);
  const int visible_tabs = VisibleTabCount(s_section);
  char position[24] = "";
  if (visible_tabs > 1) snprintf(position, sizeof(position), "%d/%d", ActiveVisibleTabPosition() + 1, visible_tabs);
  const int position_x = value_right - SmallTextWidth(position);
  DrawTextN(layout, right_text_x + kIconSize + 6, right_title_y,
            Ui(section->label), (position_x - right_text_x - kIconSize - 14) / kGlyphSize, kText_Normal);
  DrawSmallText(layout, position_x, right_title_y + 1, position, kMutedText);
  /* Translated feedback belongs in the full-width description panel below,
   * not beside the title where longer reset/save messages collide with it. */

  /* A section with a single VISIBLE tab draws no strip at all — a lone
   * highlighted chip would read as a control the player can act on, and it
   * would spend a row's worth of height saying nothing. Hidden (all-debug)
   * tabs are skipped, so with debug settings off Town 3D shows Scene/Camera
   * and System shows no strip. */
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
      vwidth[vcount] = SmallTextWidth(Ui(section->tabs[tab].label)) + 8;
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
    bool partial_tab = false;
    for (int i = s_tab_scroll; i < vcount; i++) {
      const int available = inner_x1 - tab_x;
      if (available < vwidth[i] && available < 8 + 4 * kDebugGlyphWidth) break;
      const int shown = vwidth[i] < available ? vwidth[i] : available;
      const bool partial = shown < vwidth[i];
      bool current = vis[i] == active_tab;
      /* The active tab is the cursor's position among the tabs, so it takes
       * the same menu yellow as the selected row/section. */
      if (current) {
        FillLogicalRect(layout, tab_x, tab_y - 2, shown, 11,
                        ScaleColor(kSelectYellow, 20));
        FillLogicalRect(layout, tab_x, tab_y + 9, shown, 1, kSelectYellow);
      }
      const int chars = (shown - 8) / kDebugGlyphWidth;
      DrawSmallTextN(layout, tab_x + 4, tab_y + 1, Ui(section->tabs[vis[i]].label),
                    chars - (partial ? 3 : 0), current ? kSelectYellow : kMutedText);
      if (partial) DrawSmallText(layout, tab_x + 4 + (chars - 3) * kDebugGlyphWidth,
                                 tab_y + 1, "...", current ? kSelectYellow : kMutedText);
      tab_x += vwidth[i] + 2;
      last_shown = i;
      partial_tab = partial;
      if (partial) break;
    }
    if (overflow && (last_shown < vcount - 1 || partial_tab))
      DrawSmallText(layout, inner_x1 + 2, tab_y + 2, ">", kSelectYellow);
    DrawSmallText(layout, value_right - chevron + 2, tab_y + 2, "R", kSteelDim);
    rule_y = tab_y + 13;
  }
  /* Accent rule under the header ties the title, tabs, and row list into one
   * section-colored block. */
  FillLogicalRect(layout, right_x + 10, rule_y, right_width - 20, 1,
                  structure_dim);


  return rule_y;
}

static void DrawMenuRows(const MenuLayout *layout, const MenuChrome *c,
                         const MenuSection *section, int rule_y,
                         bool custom_rows) {
  const int right_x = c->right_x;
  const int right_width = c->right_width;
  const int top_y = c->top_y;
  const int top_height = c->top_height;
  const int right_text_x = c->right_text_x;
  const int value_right = c->value_right;
  const int scroll_x = c->scroll_x;
  const uint32_t structure = kSteelBlue;
  const uint32_t structure_dim = kSteelDim;

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

  /* The layer editor draws its own rows and then skips the descriptor loop and
   * the synthetic section-reset row entirely: it has no descriptors, and its
   * reset is per-room and already in the list. */
  if (ActiveTabIsRegional()) {
    const int count = TabSettingRowCount();
    for (int row = 0; row < count; ++row) {
      ++row_index;
      if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
      ++drawn_rows;
      const int y = first_row_y + (row - s_top_row) * kRowHeight;
      if (!s_regional_valid) {
        DrawSmallTextN(layout, label_x, y, Ui("overlay.region.enter_campaign"),
                       (value_right - label_x) / kDebugGlyphWidth, kMutedText);
        continue;
      }
      const OverlayRegionRow *entry=OverlayRegionMenu_Row(ActiveTab()->regional_page,(unsigned)row);
      SettingsOverlayRegionBadge badge;
      char value[128];
      if (!OverlayRegionMenu_Value(InterfaceLocale(),&s_regional_view,entry,false,value,sizeof(value),&badge)) continue;
      if (entry->kind == kOverlayRegionRow_Preset) {
        const ArRegionalSource source = RegionalPresetChoice(entry);
        badge = source == kArRegionalSource_US ? kOverlayRegionBadge_US :
            source == kArRegionalSource_Japan ? kOverlayRegionBadge_Japan : kOverlayRegionBadge_Europe;
        snprintf(value, sizeof(value), "%s >", SettingsOverlayRegions_BadgeCode(InterfaceLocale(), badge));
      }
      const bool selected = s_submenu_open && row == s_row;
      if (selected) {
        FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11, kHighlight);
        FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
        DrawGlyph(layout, selector_x + CursorBlinkOffset(), y, '>', kText_Warning);
      }
      const TextStyle style = s_submenu_open && s_regional_view.editable ? kText_Normal : kText_Dim;
      const int shown = CappedTextLength(value, value_chars);
      const int badge_x = value_right - shown * kGlyphSize - (entry->kind == kOverlayRegionRow_Difficulty ? 0 : 16);
      DrawTextN(layout, label_x, y, OverlayRegionMenu_Label(InterfaceLocale(), entry),
                (badge_x - label_x - 4) / kGlyphSize, style);
      DrawTextRight(layout, value_right, y, value, value_chars, style == kText_Normal ? kText_Value : style);
      const ArRenderTexture texture = SettingsOverlayArtwork_Get()->region_badges;
      if (entry->kind!=kOverlayRegionRow_Difficulty && ArRenderTexture_IsValid(texture)) {
        const ArRenderRectF source = {(float)(badge * kRegionBadgeWidth), 0,
                                      kRegionBadgeWidth, kRegionBadgeHeight};
        const ArRenderRectF destination = ToRenderRect(LogicalRect(layout, badge_x, y, 12, 8));
        (void)ArRenderDevice_DrawTextureTinted(s_render_device, texture, &source, &destination,
            (ArRenderColorF){1, 1, 1, style == kText_Normal ? 1.0f : 0.5f});
      }
    }
  } else if (custom_rows) {
    LayerMenuRow rows[kLayerMenuRowMax];
    int n = LayerMenuRows(rows, kLayerMenuRowMax);
    for (int i = 0; i < n; i++) {
      int row = row_index++;
      if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
      drawn_rows++;
      int y = first_row_y + (row - s_top_row) * kRowHeight;
      const LayerMenuRow *entry = &rows[i];
      SettingsOverlayLayerText text;
      LocalizeLayerRow(entry, &text);
      /* An unselectable row is never drawn as selected, even when the cursor sits
       * on it -- which happens on a tab whose every row is a notice, since there
       * is nothing for SkipUnselectableRow to move to. Highlighting it with the
       * blinking cursor would invite a keypress that does nothing. */
      const bool selected =
          s_submenu_open && row == s_row && entry->selectable;

      /* A rule above the reset row, matching how the Save and Extras tabs fence
       * their destructive commands off from the settings above them. */
      if (entry->separator_before)
        FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                        ARGB(160, 190, 96, 76));
      if (selected) {
        FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11,
                        kHighlight);
        FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
        DrawGlyph(layout, selector_x + CursorBlinkOffset(), y, '>',
                  kText_Warning);
      }

      /* A caption is structure, not a control, so it takes the panel's structure
       * color and no value styling. A nested parameter indents under its plane
       * and dims, so the eye reads the grouping without a box. */
      TextStyle style = s_submenu_open ? kText_Normal : kText_Dim;
      int row_label_x = label_x + (entry->nested ? 3 * kGlyphSize : 0);
      if (!entry->selectable) {
        int shown = CappedTextLength(text.value, value_chars);
        int value_x = value_right - shown * kDebugGlyphWidth;
        DrawSmallTextN(layout, row_label_x, y + 1, text.label,
                        (value_x - row_label_x - 8) / kDebugGlyphWidth, structure);
        if (shown)
          DrawSmallTextN(layout, value_x, y + 1, text.value, shown, kGameGold);
        continue;
      }

      int shown = CappedTextLength(text.value, value_chars);
      int label_chars = (value_right - shown * kGlyphSize - 12 -
                         row_label_x - 4) / kGlyphSize;
      if (label_chars < 1) label_chars = 1;
      DrawTextN(layout, row_label_x, y, text.label, label_chars,
                entry->nested && !selected ? kText_Dim : style);
      bool reset_row =
          (entry->owner == kLayerMenuRow_Diorama &&
           entry->source.diorama.kind == kDioramaEditorRow_ResetRoom) ||
          (entry->owner == kLayerMenuRow_ActionBg &&
           entry->source.action_bg.kind == kActionBgTunerRow_Reset);
      DrawTextRight(layout, value_right, y, text.value, value_chars,
                    reset_row
                        ? (s_submenu_open ? kText_Warning : kText_Dim)
                        : (style == kText_Normal ? kText_Value : style));
    }
  }

  const int registry_rows = custom_rows ? 0 : RegistryMenuRowCount();
  for (int i = 0; i < registry_rows; i++) {
    const SettingMenuRow entry = RegistryMenuRowAt(i);
    int row = row_index++;
    if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
    drawn_rows++;
    int y = first_row_y + (row - s_top_row) * kRowHeight;
    if (entry.heading != kSettingGameChange_None) {
      const char *heading = SettingsOverlay_LocalizedGameChangeHeading(
          InterfaceLocale(), entry.heading);
      uint32_t color = s_submenu_open
          ? (entry.heading == kSettingGameChange_OriginalBugFix
              ? kGameGold : kQualityOfLifeBlue)
          : kMutedText;
      DrawSmallText(layout, label_x, y + 1, heading, color);
      const int rule_x = label_x + SmallTextWidth(heading) + 7;
      if (rule_x < value_right)
        FillLogicalRect(layout, rule_x, y + 5, value_right - rule_x, 1,
                        s_submenu_open ? ScaleColor(color, 55) : kSteelDim);
      continue;
    }
    const SettingDesc *desc = entry.desc;
    if (!desc) continue;
    /* Commands are separated from the settings they act on. */
    if (category == kSettingCat_Save &&
        desc->action == kSettingAction_SaveApplySession)
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                      structure_dim);
    if (category == kSettingCat_Extras && desc->action == kSettingAction_Restart)
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                      ARGB(160, 190, 96, 76));
    bool selected = s_submenu_open && row == s_row;
    bool available = Settings_IsAvailable(desc);
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11,
                      kHighlight);
      FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
      DrawGlyph(layout, selector_x + CursorBlinkOffset(), y, '>',
                kText_Warning);
    }
    TextStyle style = available && s_submenu_open
        ? kText_Normal : kText_Dim;

    char value[512];
    if (selected && desc == s_capture_desc) {
      /* Blink so an armed row is unmistakable — on a Deck the status line at
       * the bottom of the panel is easy to miss mid-rebind. */
      snprintf(value, sizeof(value), "%s",
               (HostClock_Milliseconds() / 300) & 1 ? Ui("overlay.value.press") : "");
    } else if (selected && s_editing) {
      snprintf(value, sizeof(value), "%s", s_edit_buffer);
    } else {
      SettingsOverlay_LocalizedValue(InterfaceLocale(), desc, value, sizeof(value));
      if (desc->field == &g_settings.interface_language &&
          !ArUiTextRenderer_IsReady(&s_ui_text))
        Settings_FormatValue(desc, value, sizeof(value));
    }
    if (!value[0] && desc != s_capture_desc)
      snprintf(value, sizeof(value), "%s", Ui("overlay.value.custom"));
    int shown_value_chars = CappedTextLength(value, value_chars);
    int row_value_left = value_right - shown_value_chars * kGlyphSize;
    int restart_x = row_value_left - 12;
    int label_chars = (restart_x - label_x - 4) / kGlyphSize;
    if (label_chars < 1) label_chars = 1;
    DrawTextN(layout, label_x, y,
              SettingsOverlay_LocalizedLabel(InterfaceLocale(), desc), label_chars, style);
    /* M2 (followup doc): values render in kText_Value (cool cyan) so they
     * read distinct from labels, but only for normal/enabled rows — a
     * dim/unavailable row's style must win so it stays visibly greyed. */
    DrawTextRight(layout, value_right, y, value, value_chars,
                  style == kText_Normal ? kText_Value : style);
    if (desc->apply == kApply_Restart)
      DrawGlyph(layout, restart_x, y, '*', kText_Warning);
  }

  /* Synthetic section action: it is deliberately outside the descriptor
   * registry because one button spans several registry categories/tabs and
   * must never be written as a setting of its own. Skipped for the layer
   * editor, whose reset is per-room and part of its own list. */
  if (!custom_rows) {
    int row = row_index++;
    if (row >= s_top_row && row < s_top_row + s_visible_rows) {
      drawn_rows++;
      int y = first_row_y + (row - s_top_row) * kRowHeight;
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1,
                      ARGB(160, 190, 96, 76));
      bool selected = s_submenu_open && row == s_row;
      if (selected) {
        FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11,
                        kHighlight);
        FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
        DrawGlyph(layout, selector_x + CursorBlinkOffset(), y, '>',
                  kText_Warning);
      }
      char label[256];
      FormatSectionMessage(label, sizeof(label), "overlay.reset_section", section->label);
      int restart_x = value_right - 5 * kGlyphSize - 12;
      int label_chars = (restart_x - label_x - 4) / kGlyphSize;
      if (label_chars < 1) label_chars = 1;
      DrawTextN(layout, label_x, y, label, label_chars,
                s_submenu_open ? kText_Normal : kText_Dim);
      DrawTextRight(layout, value_right, y, Ui("common.reset"), 8,
                    s_submenu_open ? kText_Warning : kText_Dim);
    }
  }

  DrawScrollBar(layout, scroll_x, first_row_y - 2,
                s_visible_rows * kRowHeight, row_index, s_visible_rows,
                s_top_row, structure);

  if (row_index == 0)
    DrawSmallText(layout, right_text_x, first_row_y + 2,
                  Ui("overlay.empty_tab"), kMutedText);

  if (category == kSettingCat_Inspector) {
    int info_y = first_row_y + drawn_rows * kRowHeight + 5;
    FillLogicalRect(layout, right_x + 12, info_y - 4, right_width - 24, 1,
                    structure_dim);
    DrawSmallText(layout, right_text_x, info_y, Ui("overlay.scene.live"), structure);
    DrawInspectorInfo(layout, right_text_x, info_y + 11,
                      (right_width - 24) / kDebugGlyphWidth, 6);
  }

}

static void DrawMenuFooter(const MenuLayout *layout, const MenuChrome *c,
                           const MenuSection *section) {
  const int margin = c->margin;
  const int panel_right = c->panel_right;
  const int bottom_y = c->bottom_y;
  const int bottom_height = c->bottom_height;
  const int bottom_width = c->bottom_width;
  const uint32_t structure = kSteelBlue;
  const uint32_t structure_dim = kSteelDim;

  /* ── Description panel ────────────────────────────────────────────────── */
  const int description_x = margin + 12;
  const int description_chars = (bottom_width - 24) / kDebugGlyphWidth;
  const bool reset_selected =
      s_submenu_open && SelectedRowIsSectionReset();
  const SettingDesc *selected = s_submenu_open ? SelectedDesc() : NULL;
  const int header_y = bottom_y + 8;
  /* The editor's rows are not descriptors, so they carry their own header and
   * help. Handled before the descriptor branches, which would otherwise fall
   * through to the section blurb and say nothing about the selected row. */
  LayerMenuRow help_rows[kLayerMenuRowMax];
  const LayerMenuRow *help_row =
      (ActiveSectionIsCustom() && s_submenu_open)
          ? SelectedLayerRow(help_rows, kLayerMenuRowMax, NULL) : NULL;
  if (s_status[0]) {
    DrawSmallText(layout, description_x, header_y, Ui("overlay.tab.status"), kGameGold);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         s_status, description_chars, 4, ARGB(255, 208, 220, 232));
  } else if (ActiveTabIsRegional() && s_submenu_open) {
    const OverlayRegionRow *entry=SelectedRegionRow();
    const char *label = s_regional_valid
        ? OverlayRegionMenu_Label(InterfaceLocale(), entry) : Ui("overlay.region.tab");
    char help[2048];
    char current[256] = "";
    SettingsOverlayRegionBadge effective;
    char active[128];
    if (s_regional_valid && !s_regional_view.new_game && entry &&
        OverlayRegionMenu_Value(InterfaceLocale(),&s_regional_view,entry,true,active,sizeof(active),&effective)) {
      const ArUiTextArgument args[] = {{"region", active}};
      ArUiCatalog_Format(current, sizeof(current), Ui("overlay.region.active"), args, 1);
    }
    const int current_x = panel_right - 12 - SmallTextWidth(current);
    DrawSmallTextN(layout, description_x, header_y, label,
        (current_x - description_x - 8) / kDebugGlyphWidth, structure);
    DrawSmallText(layout, current_x, header_y, current, kMutedText);
    FillLogicalRect(layout, description_x, header_y + 10, bottom_width - 24, 1, structure_dim);
    if (!s_regional_valid || !OverlayRegionMenu_Description(InterfaceLocale(),
        &s_regional_view, entry, help, sizeof(help)))
      snprintf(help, sizeof(help), "%s", RegionalNotice());
    DrawSmallTextN(layout, description_x, header_y + 14,
        s_regional_valid ? OverlayRegionMenu_ImpactLabel(InterfaceLocale(), &s_regional_view, entry) : "",
        description_chars, kGameGold);
    DrawSmallTextPreview(layout, description_x, header_y + 14 + kSmallLineHeight, help, description_chars,
        3,
        ARGB(255, 208, 220, 232));
  } else if (help_row) {
    SettingsOverlayLayerText text;
    LocalizeLayerRow(help_row, &text);
    const DioramaEditorRow *diorama = help_row->owner == kLayerMenuRow_Diorama
        ? &help_row->source.diorama : NULL;
    char label[2 * kOverlayLayerCaptionBytes + 8];
    if (diorama && diorama->kind == kDioramaEditorRow_Plane)
      snprintf(label, sizeof(label), "%s -- %s", text.label, text.value);
    else
      snprintf(label, sizeof(label), "%s", text.label);
    /* The right-hand slug says WHEN a change takes effect, which for these is
     * always "the next frame" -- that immediacy is the point of the tool. */
    const char *kApplyNow = Ui("overlay.apply.0");
    int apply_x = panel_right - 12 - SmallTextWidth(kApplyNow);
    DrawSmallTextN(layout, description_x, header_y, label,
                   (apply_x - description_x - 8) / kDebugGlyphWidth, structure);
    DrawSmallTextN(layout, panel_right - 12 - SmallTextWidth(kApplyNow),
                   header_y, kApplyNow, description_chars, kMutedText);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         text.help,
                         description_chars, 4, ARGB(255, 208, 220, 232));
  } else if (reset_selected) {
    char label[256], help[1024];
    FormatSectionMessage(label, sizeof(label), "overlay.reset_section", section->label);
    FormatSectionMessage(help, sizeof(help), "overlay.reset_section.help", section->label);
    DrawSmallText(layout, description_x, header_y, label, structure);
    DrawSmallTextN(layout,
                   panel_right - 12 - SmallTextWidth(Ui("overlay.apply.4")), header_y,
                   Ui("overlay.apply.4"), description_chars, kGameGold);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         help, description_chars, 4,
                         ARGB(255, 208, 220, 232));
  } else if (selected) {
    DrawSmallText(layout, description_x, header_y,
                   SettingsOverlay_LocalizedLabel(InterfaceLocale(), selected), structure);
    /* Naming HOW a change takes effect next to the row removes the usual
     * "did that do anything?" question; the '*' row marker only says that a
     * restart is involved, not what the other kinds do. */
    char apply_key[32];
    snprintf(apply_key, sizeof(apply_key), "overlay.apply.%d", selected->apply);
    const char *apply = ArUiCatalog_Text(InterfaceLocale(), apply_key,
                                         Settings_ApplyKindName(selected->apply));
    uint32_t apply_color = selected->apply == kApply_Restart
        ? kGameGold : kMutedText;
    if (!Settings_IsAvailable(selected)) {
      apply = Ui("overlay.unavailable");
      apply_color = ARGB(255, 246, 49, 49);  /* menu red */
    }
    DrawSmallTextN(layout,
                   panel_right - 12 - SmallTextWidth(apply), header_y,
                   apply, description_chars, apply_color);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    const char *help = SettingsOverlay_LocalizedHelp(InterfaceLocale(), selected);
    const char *hardware_help = Settings_HardwareUnavailableReason(selected);
    if (hardware_help) help = ArUiCatalog_Text(InterfaceLocale(),
        "overlay.hardware_unavailable", hardware_help);
    if (selected->field == &g_settings.interface_language &&
        !ArUiTextRenderer_IsReady(&s_ui_text)) help = Ui("overlay.font_unavailable");
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         help, description_chars, 4,
                         ARGB(255, 208, 220, 232));
  } else {
    DrawSmallText(layout, description_x, header_y, Ui(section->label), structure);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         Ui(section->blurb), description_chars, 4,
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
    hints[hint_count++] = (key); hints[hint_count++] = Ui(text); \
  } while (0)
  if (s_capture_desc) {
    HINT(Ui("overlay.key.any"), "overlay.hint.bind");
    HINT("ESC", "overlay.hint.cancel");
  } else if (s_editing) {
    HINT("RETURN", "overlay.hint.apply");
    HINT("A/ESC", "overlay.hint.cancel");
  } else if (s_submenu_open) {
    /* Omitted when the only row is a notice: there is nothing to select, and
     * offering the verb would suggest otherwise. */
    if (!help_row || help_row->selectable) HINT("UP/DOWN", "overlay.hint.select");
    if (help_row) {
      /* The editor's verbs differ enough to be worth spelling out: Left/Right
       * cycles the SHAPE on a plane row but steps a number on a parameter row,
       * and B expands rather than edits. */
      if (help_row->owner == kLayerMenuRow_ActionBg) {
        switch (help_row->source.action_bg.kind) {
          case kActionBgTunerRow_Layer:
            HINT("B", "overlay.hint.settings");
            HINT("Y", "overlay.hint.clear_layer");
            break;
          case kActionBgTunerRow_BandHeader:
            HINT("B", "overlay.hint.settings");
            HINT("Y", "overlay.hint.canonical_bands");
            break;
          case kActionBgTunerRow_Print:
            HINT("B", "overlay.hint.print");
            break;
          case kActionBgTunerRow_Reset:
            HINT("B", "overlay.hint.reset_draft");
            break;
          case kActionBgTunerRow_Header:
            break;
          default:
            HINT("LEFT/RIGHT", "overlay.hint.adjust");
            HINT("Y", "overlay.hint.canonical");
            break;
        }
      } else {
        switch (help_row->source.diorama.kind) {
          case kDioramaEditorRow_Plane:
            HINT("LEFT/RIGHT", "overlay.hint.shape");
            HINT("B", "overlay.hint.settings");
            HINT("Y", "overlay.hint.clear_plane");
            break;
          case kDioramaEditorRow_ResetRoom:
            HINT("B", "overlay.hint.reset_room");
            break;
          case kDioramaEditorRow_Header:
            break;
          default:
            HINT("LEFT/RIGHT", "overlay.hint.adjust");
            HINT("Y", "overlay.hint.clear");
            break;
        }
      }
      if (VisibleTabCount(s_section) > 1)
        HINT("L/R", help_row->owner == kLayerMenuRow_ActionBg
                        ? "overlay.hint.tab" : "overlay.hint.level");
    } else if (ActiveTabIsRegional() && ActiveTab()->regional_page == kOverlayRegionPage_Presets) {
      HINT("LEFT/RIGHT", "overlay.hint.select");
      HINT("B", "overlay.region.hint.review");
      HINT("L/R", "overlay.hint.tab");
    } else if (ActiveTabIsRegional()) {
      HINT("LEFT/RIGHT", "overlay.hint.change");
      HINT("L/R", "overlay.hint.tab");
    } else if (SelectedRowIsSectionReset()) {
      HINT("B", "overlay.hint.reset");
      if (VisibleTabCount(s_section) > 1) HINT("L/R", "overlay.hint.tab");
    } else {
      /* The verbs track what the selected row actually does: an Int row
       * adjusts (hold to accelerate — felt, not spelled out, to keep the line
       * short), a string/mask row opens a text prompt, the rest cycle. */
      const SettingDesc *row = SelectedDesc();
      bool numeric = row && row->type == kSettingType_Int;
      bool textual = row && (row->type == kSettingType_Mask ||
                             row->type == kSettingType_Custom);
      HINT("LEFT/RIGHT", numeric ? "overlay.hint.adjust" : "overlay.hint.change");
      if (VisibleTabCount(s_section) > 1) HINT("L/R", "overlay.hint.tab");
      if (textual) HINT("B", "overlay.hint.type");
      HINT("Y", "overlay.hint.reset");
    }
    if (ActiveTabIsRegional() && s_regional_valid) HINT("X/F3", "overlay.hint.details");
    HINT("A", "overlay.hint.back");
  } else {
    HINT("UP/DOWN", "overlay.hint.section");
    if (VisibleTabCount(s_section) > 1) HINT("L/R", "overlay.hint.tab");
    HINT("B", "overlay.hint.open");
    HINT("A", "overlay.hint.close");
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

static void DrawMenu(const MenuLayout *layout) {
  const MenuChrome chrome = ComputeMenuChrome(layout);

  /* Three framed panels: nav column (left), submenu (top-right), and the
   * description/hint strip (bottom). */
  DrawDialogPanel(layout, chrome.left_x, chrome.top_y, chrome.left_width,
                  chrome.top_height);
  DrawDialogPanel(layout, chrome.right_x, chrome.top_y, chrome.right_width,
                  chrome.top_height);
  DrawDialogPanel(layout, chrome.margin, chrome.bottom_y, chrome.bottom_width,
                  chrome.bottom_height);

  const MenuSection *section = ActiveSection();

  /* The host clock is monotonic and 64-bit, so a direct comparison is exact
   * over any realistic session. */
  if (s_status[0] && HostClock_Milliseconds() >= s_status_until)
    s_status[0] = 0;

  const bool custom_rows = ActiveSectionIsCustom() || ActiveTabIsRegional();

  DrawMenuNavColumn(layout, &chrome);
  const int rule_y = DrawMenuHeader(layout, &chrome, section);
  DrawMenuRows(layout, &chrome, section, rule_y, custom_rows);
  DrawMenuFooter(layout, &chrome, section);
}

void SettingsOverlay_Render(ArRenderRectI game_viewport) {
  SettingsOverlay_Refresh();
  if (!s_open || !s_render_device ||
      !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->fonts[kText_Normal])) return;
  int output_width = 0;
  int output_height = 0;
  if (!ArRenderOutput_UseFull(
          s_render_device, &output_width, &output_height))
    return;

  /* THE MANUAL SUPERSEDES THE MENU, and draws instead of it rather than over it.
   * The reader is a mode this overlay is in, not a peer of it -- s_open stays
   * true throughout, so everything that asks whether the menu has the game
   * suspended keeps getting one answer. Full output, not game_viewport: a page
   * of text wants the window, not the letterboxed 4:3 area the game sits in. */
  if (ManualIsOpen() && s_manual_hooks.render) {
    s_manual_hooks.render(
        (ArRenderRectI){ 0, 0, output_width, output_height });
    return;
  }

  s_match_game_scale_percent =
      ((game_viewport.h * kPercentScale +
        kActRaiserAuthenticHeight / 2) /
           kActRaiserAuthenticHeight + kScaleStepPercent / 2) /
      kScaleStepPercent * kScaleStepPercent;
  if (s_match_game_scale_percent < kMinimumScalePercent)
    s_match_game_scale_percent = kMinimumScalePercent;
  if (s_match_game_scale_percent > kMatchGameMaximumScalePercent)
    s_match_game_scale_percent = kMatchGameMaximumScalePercent;

  MenuLayout layout = BuildLayout(output_width, output_height);
  if (s_decision.result == kOverlayDecision_Pending) {
    DrawDecision(&layout, &s_decision);
    return;
  }
  if (s_regional_confirmation.dialog.result == kOverlayDecision_Pending) {
    DrawDecision(&layout, &s_regional_confirmation.dialog);
    return;
  }
  if (s_details.open) {
    DrawDetails(&layout);
    return;
  }
  DrawMenu(&layout);
  SettingsOverlayPalette_Draw(&layout);
}
