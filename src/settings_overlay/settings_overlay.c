#include "settings_overlay/settings_overlay.h"

#include "localization/interface_text.h"
#include "settings_overlay/settings_overlay_internal.h"
#include "settings_overlay/save_slots/save_slot_menu.h"
#include "settings_overlay/settings_overlay_artwork.h"
#include "settings_overlay/layers/layer_palette.h"
#include "settings_overlay/settings_overlay_localization.h"
#include "settings_overlay/layers/layer_menu.h"
#include "settings_overlay/regional/regional_panel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "diorama/diorama_layer_editor.h"
#include "host/host_clock.h"
#include "app/input_map.h"
#include "render/render_output.h"
#include "app/settings.h"
#include "randomizer/randomizer.h"
#include "app/user_data_dir.h"

enum {
  kMatchGameMaximumScalePercent = 400,
  /* Nav rows are as tall as a section icon; the eight sections then fill the
   * column without scrolling at any ordinary window size. */
  kNavRowHeight = 17,
  kTabBarHeight = 12,
  /* Hold-to-accelerate timing. The initial delay is what separates a tap
   * (single fine step) from a hold; after it, a step fires every interval. */
  kHoldInitialDelayMs = 350,
  kHoldRepeatMs = 55,
  kCursorBlinkHalfPeriodMs = 250,
};

static const uint32_t kQualityOfLifeBlue = ARGB(255, 156, 205, 255);

static const char *Ui(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}

static void FormatSectionMessage(char *out, size_t capacity, const char *key,
                                  const char *section_key) {
  ArUiTextArgument arg = {"section", Ui(section_key)};
  if (!ArUiCatalog_Format(out, capacity, Ui(key), &arg, 1) && capacity) out[0] = 0;
}

/* ── Sections and tabs ─────────────────────────────────────────────────
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
  /* Rows built by the layer feature rather than enumerated from the settings registry.
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
#define PAGE_TAB(cat, name, key, value) \
  { .category = kSettingCat_##cat, .label = name, .page_key = key, .page_value = value }

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
    {.label = "overlay.region.tabs.presets",
     .regional_rules = true,
     .regional_page = kOverlayRegionPage_Presets},
    {.label = "overlay.region.tabs.action",
     .regional_rules = true,
     .regional_page = kOverlayRegionPage_Action},
    {.label = "overlay.region.tabs.towns",
     .regional_rules = true,
     .regional_page = kOverlayRegionPage_Towns},
    {.label = "overlay.region.tabs.controls",
     .regional_rules = true,
     .regional_page = kOverlayRegionPage_Controls},
    {.label = "overlay.region.tabs.presentation",
     .regional_rules = true,
     .regional_page = kOverlayRegionPage_Presentation},
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
/* A section whose feature owns its rows, and which is developer-only. */
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
  SECTION(Save, "overlay.section.save", "overlay.section.save.help", kTabsSave),
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
static struct {
  int section, row, top, tab;
  bool submenu;
} s_slot_parent;
static int s_top_row;
static int s_visible_rows = 9;
static int s_auto_menu_scale_percent = kPercentScale;
static int s_match_game_scale_percent = kPercentScale;
static char s_status[256];
static OverlayDecision s_decision;
/* Read-only overlay-local help; never shares host confirmation state. */
static struct {
  bool open;
  char title[256], body[16384];
  int top_line, total_lines, visible_lines;
} s_details;
void SettingsOverlay_CloseDetails(void) { s_details.open = false; }

void SettingsOverlay_ShowDetails(const SettingsOverlayDetailsText *text) {
  if (!text || !text->body[0]) return;
  snprintf(s_details.title, sizeof(s_details.title), "%s", text->title);
  snprintf(s_details.body, sizeof(s_details.body), "%s", text->body);
  s_details.top_line = s_details.total_lines = 0;
  s_details.visible_lines = 1;
  s_details.open = true;
}

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
/* Modal input has its own device history: it must not leave gameplay buttons
 * held just to choose accurate control hints. */
static InputClass s_menu_input_device;

InputClass SettingsOverlay_MenuInputDevice(void) {
  if (!InputMap_GamepadCount() || g_settings.input_device == kInputDevice_Keyboard)
    return kInputClass_Keyboard;
  return g_settings.input_device == kInputDevice_Gamepad ? kInputClass_Gamepad
                                                         : s_menu_input_device;
}
/* Binding capture: the row is armed and the NEXT physical input on the
 * matching device becomes its binding. Held separately from s_editing because
 * capture consumes raw events rather than text. */
static const SettingDesc *s_capture_desc;
static SettingsOverlayInspectorInfoProvider s_inspector_info_provider;

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

static bool ActiveSectionIsCustom(void) {
  return ActiveSection()->custom_rows;
}



static bool ActiveTabIsRegional(void) { return ActiveTab()->regional_rules; }


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

static int TabSettingRowCount(void) {
  if (ActiveTabIsRegional())
    return RegionalMenu_Count(ActiveTab()->regional_page);
  if (ActiveSectionIsCustom()) return LayerMenu_Count(ActiveTabIndex());
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
void SettingsOverlay_SetStatus(const char *text) { SetStatus(text); }

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
    SetStatus(result == kSettingChange_Rejected ? Ui("overlay.status.not_editable")
                                                : Ui("overlay.status.unchanged"));
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
  if(SaveSlotMenu_ConfirmEditorAction(desc))return;
  bool success=Settings_InvokeAction(desc);
  if(success && SaveSlotMenu_Active())s_status[0]=0;
  else SetStatus(Ui(success?"overlay.status.action_complete":"overlay.status.action_failed"));
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

static void ChangeSelectedValue(int direction) {
  if (ActiveTabIsRegional()) {
    EndValueHold();
    RegionalMenu_Change(ActiveTab()->regional_page, s_row, direction, false, false);
    return;
  }
  if (ActiveSectionIsCustom()) {
    LayerMenu_Change(ActiveTabIndex(), s_row, direction);
    return;
  }
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


static void ActivateSelectedRow(void) {
  if (ActiveTabIsRegional()) {
    EndValueHold();
    RegionalMenu_Change(ActiveTab()->regional_page, s_row, 1, false, true);
    return;
  }
  if (ActiveSectionIsCustom()) {
    LayerMenu_Activate(ActiveTabIndex(), s_row);
    return;
  }
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


static void ResetSelectedValue(void) {
  if (ActiveTabIsRegional()) {
    EndValueHold();
    RegionalMenu_Change(ActiveTab()->regional_page, s_row, 1, true, false);
    return;
  }
  if (ActiveSectionIsCustom()) {
    LayerMenu_Reset(ActiveTabIndex(), s_row);
    return;
  }
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
  LayerMenu_ResetNavigation();
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
  if (ActiveTabIsRegional()) return !RegionalMenu_Available();
  if (!ActiveSectionIsCustom()) {
    if (index < 0 || index >= TabSettingRowCount()) return false;
    return RegistryMenuRowAt(index).desc == NULL;
  }
  return !LayerMenu_RowSelectable(ActiveTabIndex(), index);
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
  SaveSlotMenu_Refresh();
  if (NormalizeNavigation()) {
    EndValueHold();
    StopEditing();
    ClearSectionResetArm();
    s_capture_desc = NULL;
    s_row = 0;
    s_top_row = 0;
    s_tab_scroll = 0;
    LayerMenu_ResetNavigation();
  }
  SyncActiveTabPage();
  if (ActiveTabIsRegional())
    RegionalMenu_Refresh();
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
  if(!s_submenu_open && ActiveSection()->icon==kOverlayIcon_Save && SaveSlotMenu_Available())return;
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
  LayerMenu_ResetNavigation();
  StopEditing();
  s_capture_desc = NULL;
  SettingsOverlay_Refresh();
  SkipUnselectableRow();
  EnsureSelectedRowVisible();
}

static void EnterSection(void) {
  ClearSectionResetArm();
  if(ActiveSection()->icon==kOverlayIcon_Save && SaveSlotMenu_Available()) {
    if(!SettingsOverlay_OpenSaveSlots(false))SetStatus(Ui("slots.read_failed"));
    return;
  }
  s_submenu_open = true;
  s_row = 0;
  s_top_row = 0;
  SettingsOverlay_Refresh();
  SkipUnselectableRow();
  EnsureSelectedRowVisible();
}

void SettingsOverlay_SetSaveSlotHooks(const SettingsOverlaySaveSlotHooks *hooks) {
  SaveSlotMenu_SetHooks(hooks);
}

bool SettingsOverlay_OpenSaveSlots(bool randomized) {
  if (!s_open || !SaveSlotMenu_Available() ||
      (randomized && !g_settings.show_debug_settings)) return false;
  if (!randomized && SaveSlotMenu_ReturnFromEditor()) {
    s_status[0] = 0;
    return true;
  }
  if (!SaveSlotMenu_Open(randomized)) return false;
  s_slot_parent.section = s_section;
  s_slot_parent.row = s_row;
  s_slot_parent.top = s_top_row;
  s_slot_parent.tab = s_tab[s_section];
  s_slot_parent.submenu = s_submenu_open;
  s_status[0] = 0;
  return true;
}

bool SettingsOverlay_Init(ArRenderDevice *render_device, SDL_Window *window,
                          const uint8_t *rom_data, size_t rom_size) {
  s_window = window;
  return SettingsOverlayWidgets_Init(render_device, rom_data, rom_size);
}

bool SettingsOverlay_ReloadTextures(const uint8_t *rom_data, size_t rom_size) {
  return SettingsOverlayWidgets_ReloadTextures(rom_data, rom_size);
}

void SettingsOverlay_Destroy(void) {
  SaveSlotMenu_Close();
  s_status[0] = 0;
  SettingsOverlay_SetSaveSlotHooks(NULL);
  StopEditing();
  SettingsOverlayWidgets_Destroy();
  s_window = NULL;
  s_open = false;
  memset(&s_decision, 0, sizeof(s_decision));
  memset(&s_details, 0, sizeof(s_details));
  s_submenu_open = false;
  SettingsOverlayDebugPanel_Reset();
  s_inspector_info_provider = NULL;
  SettingsOverlay_SetRegionalHooks(NULL);
}

bool SettingsOverlay_IsOpen(void) {
  return s_open;
}

void SettingsOverlay_Open(void) {
  if (s_decision.result == kOverlayDecision_Pending) return;
  s_details.open = false;
  RegionalMenu_Open();
  s_menu_input_device = InputMap_GamepadCount() &&
      (g_settings.input_device == kInputDevice_Gamepad ||
       (g_settings.input_device != kInputDevice_Keyboard && InputMap_GamepadIsActive()))
          ? kInputClass_Gamepad : kInputClass_Keyboard;
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
  SaveSlotMenu_Close();
  s_status[0] = 0;
  s_details.open = false;
  RegionalMenu_Close();
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
  if (s_open || s_decision.result != kOverlayDecision_None ||
      !SettingsOverlayWidgets_RenderDevice() ||
      !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->fonts[kText_Normal]) || !title ||
      !body || !accept || !*title || !*body || !*accept ||
      strlen(title) >= sizeof(s_decision.title) ||
      strlen(body) >= (body_text ? sizeof(s_decision.body) : 96) ||
      strlen(accept) >= sizeof(s_decision.accept))
    return false;
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
  if(SaveSlotMenu_Active() || SaveSlotMenu_DecisionActive())return SaveSlotMenu_SelectedKey();
  if (ActiveTabIsRegional()) return RegionalMenu_Key(ActiveTab()->regional_page, s_row);
  if (ActiveSectionIsCustom()) return LayerMenu_Key(ActiveTabIndex(), s_row);
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
  if(SaveSlotMenu_Active()){SaveSlotMenu_TabState(active_tab,tab_count);return true;}
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

/* Logical menu commands. Both the keyboard path and the gamepad path funnel
 * through these so the two never drift apart, and so a rebound pad drives the
 * menu with the player's own buttons. */

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
  if (RegionalMenu_HandleConfirmation(nav, repeat)) return;
  if (s_decision.result == kOverlayDecision_Pending) {
    if (repeat) return;
    if (nav == kMenuNav_Up || nav == kMenuNav_Down || nav == kMenuNav_Left ||
        nav == kMenuNav_Right) {
      if(!s_decision.notice)s_decision.accept_selected = !s_decision.accept_selected;
    } else if (nav == kMenuNav_Confirm) {
      s_decision.result =
          s_decision.accept_selected ? kOverlayDecision_Accepted : kOverlayDecision_Cancelled;
      SettingsOverlay_Close();
    } else if (nav == kMenuNav_Back || nav == kMenuNav_Close) SettingsOverlay_Close();
    return;
  }
  if (SettingsOverlayPalette_ApplyNav(nav, repeat)) return;
  switch (SaveSlotMenu_HandleNav(nav, repeat)) {
    case kSaveSlotMenuNav_Unhandled: break;
    case kSaveSlotMenuNav_Handled: return;
    case kSaveSlotMenuNav_CloseOverlay:
      SettingsOverlay_Close();
      return;
    case kSaveSlotMenuNav_OpenEditor:
      for (int section = 0; section < kSectionCount; section++)
        if (kSections[section].icon == kOverlayIcon_Save) s_section = section;
      s_submenu_open = true;
      s_row = s_top_row = 0;
      s_tab[s_section] = kSaveEditorPage_Actions;
      s_status[0] = 0;
      SettingsOverlay_Refresh();
      return;
    case kSaveSlotMenuNav_ReturnToParent:
      s_submenu_open = s_slot_parent.submenu;
      s_section = s_slot_parent.section;
      s_row = s_slot_parent.row;
      s_top_row = s_slot_parent.top;
      s_tab[s_section] = s_slot_parent.tab;
      s_status[0] = 0;
      SettingsOverlay_Refresh();
      return;
  }
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
    case kMenuNav_Reset:   if (!repeat) ResetSelectedValue(); break;
    case kMenuNav_Details:
      if (!repeat && ActiveTabIsRegional()) {
        EndValueHold();
        RegionalMenu_OpenDetails(ActiveTab()->regional_page, s_row);
      }
      break;
    case kMenuNav_Back:
      if (!repeat) {
        EndValueHold();
        ClearSectionResetArm();
        if (!SaveSlotMenu_ReturnFromEditor()) s_submenu_open = false;
        else s_status[0] = 0;
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
  s_menu_input_device = klass;

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

  s_menu_input_device = kInputClass_Gamepad;

  /* A text-entry field cannot be typed into with a pad; the two edge cases
   * that still make sense there are commit and cancel. */
  if (s_editing) {
    if (action == kInputAction_B) CommitEditing();
    else if (action == kInputAction_A) StopEditing();
    return true;
  }

  MenuNav nav;
  if (OverlayMenuInput_ActionNav(action, &nav)) ApplyMenuNav(nav, false);
  return true;
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
  s_menu_input_device = kInputClass_Keyboard;
  /* Capture is fed raw events by HostInput_HandleEvent (SettingsOverlay_HandleCaptureEvent)
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

  MenuNav nav;
  if (OverlayMenuInput_KeyNav(key, &nav)) ApplyMenuNav(nav, repeat);
  return true;
}

bool SettingsOverlay_HandleText(const char *text) {
  if (!s_open || !s_editing) return false;
  if (!text) return true;
  ArInterfaceText_Append(s_edit_buffer, sizeof(s_edit_buffer), text, strlen(text));
  return true;
}

static void DrawSectionIcon(const MenuLayout *layout, int x, int y, int size,
                            int section, bool selected, int alpha) {
  if (section < 0 || section >= kSectionCount) return;
  DrawOverlayIcon(layout, x, y, size, kSections[section].icon, selected, alpha);
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
  s_details.visible_lines = (height - 66 - kSmallLineHeight) / kSmallLineHeight;
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
    if (!ArInterfaceText_WrapLine(s_details.body + at, length - at, columns, sizeof(buffer) - 1,
                                  &slice) ||
        !slice.consumed)
      break;
    if (line >= s_details.top_line) {
      memcpy(buffer, s_details.body + at, slice.bytes);
      buffer[slice.bytes] = 0;
      DrawSmallText(layout, x + 16, y + 40 + (line - s_details.top_line) * kSmallLineHeight, buffer,
                    kSteelBlue);
    }
    at += slice.consumed;
  }
  DrawScrollBar(layout, x + width - 13, y + 40, s_details.visible_lines * kSmallLineHeight,
      s_details.total_lines, s_details.visible_lines, s_details.top_line, kSteelBlue);
  const InputClass device = SettingsOverlay_MenuInputDevice();
  char scroll[128], page[128], back[64];
  OverlayMenuInput_PairHint(scroll, sizeof(scroll), kMenuNav_Up, kMenuNav_Down, device);
  OverlayMenuInput_PairHint(page, sizeof(page), kMenuNav_Left, kMenuNav_Right, device);
  OverlayMenuInput_Hint(back, sizeof(back), kMenuNav_Back, device);
  if (!*back) OverlayMenuInput_Hint(back, sizeof(back), kMenuNav_Close, device);
  MenuHints hints = {0};
  AddMenuHint(&hints, scroll, Ui("overlay.hint.scroll"));
  AddMenuHint(&hints, page, Ui("overlay.hint.page"));
  AddMenuHint(&hints, back, Ui("overlay.hint.back"));
  DrawMenuHints(layout, x + 16, y + height - 17 - kSmallLineHeight, width - 32,
                &hints, kSteelBlue, kMutedText);
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
  /* Two fixed hint rows accommodate translated labels and physical button
   * names without resizing the panel when the selection/device changes. */
  const int bottom_height = 72 + kSmallLineHeight;
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
              Ui(kSections[section].navigation_label ? kSections[section].navigation_label
                                                     : kSections[section].label),
              label_chars, current ? kText_Normal : kText_Dim);
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
  if (visible_tabs > 1)
    snprintf(position, sizeof(position), "%d/%d", ActiveVisibleTabPosition() + 1, visible_tabs);
  const int position_x = value_right - SmallTextWidth(position);
  DrawTextN(layout, right_text_x + kIconSize + 6, right_title_y,
            SaveSlotMenu_EditorTitle() ? SaveSlotMenu_EditorTitle() : Ui(section->label),
            (position_x - right_text_x - kIconSize - 14) / kGlyphSize, kText_Normal);
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
    /* Only overflow gets arrows; the footer names the actual tab controls. */
    const int strip_x0 = right_text_x;
    const int strip_x1 = value_right;

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
      DrawSmallText(layout, inner_x1, tab_y + 2, ">", kSelectYellow);
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

  s_visible_rows = (top_y + top_height - 6 - first_row_y) / kMenuRowHeight;
  if (s_visible_rows < 1) s_visible_rows = 1;
  EnsureSelectedRowVisible();

  const SettingCategory category = ActiveTab()->category;
  int row_index = 0;
  int drawn_rows = 0;

  /* The layer editor draws its own rows and then skips the descriptor loop and
   * the synthetic section-reset row entirely: it has no descriptors, and its
   * reset is per-room and already in the list. */
  const MenuRowViewport viewport = {
    .x = right_x, .width = right_width, .first_y = first_row_y, .value_right = value_right,
    .top = s_top_row, .visible = s_visible_rows, .selected = s_row,
    .cursor_offset = CursorBlinkOffset(), .focused = s_submenu_open,
  };
  if (ActiveTabIsRegional()) {
    row_index = RegionalMenu_DrawRows(layout, ActiveTab()->regional_page, &viewport);
  } else if (custom_rows) {
    row_index = LayerMenu_DrawRows(layout, ActiveTabIndex(), &viewport);
  }

  const int registry_rows = custom_rows ? 0 : RegistryMenuRowCount();
  for (int i = 0; i < registry_rows; i++) {
    const SettingMenuRow entry = RegistryMenuRowAt(i);
    int row = row_index++;
    if (row < s_top_row || row >= s_top_row + s_visible_rows) continue;
    drawn_rows++;
    int y = first_row_y + (row - s_top_row) * kMenuRowHeight;
    if (entry.heading != kSettingGameChange_None) {
      const char *heading = SettingsOverlay_LocalizedGameChangeHeading(
          SettingsOverlay_InterfaceLocale(), entry.heading);
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
      SettingsOverlay_LocalizedValue(SettingsOverlay_InterfaceLocale(), desc, value, sizeof(value));
      if (desc->field == &g_settings.interface_language &&
          !SettingsOverlayWidgets_HasTextBackend())
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
              SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), desc), label_chars,
              style);
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
      int y = first_row_y + (row - s_top_row) * kMenuRowHeight;
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
                s_visible_rows * kMenuRowHeight, row_index, s_visible_rows,
                s_top_row, structure);

  if (row_index == 0)
    DrawSmallText(layout, right_text_x, first_row_y + 2,
                  Ui("overlay.empty_tab"), kMutedText);

  if (category == kSettingCat_Inspector) {
    int info_y = first_row_y + drawn_rows * kMenuRowHeight + 5;
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
  const bool layer_help = ActiveSectionIsCustom() && s_submenu_open &&
      LayerMenu_RowExists(ActiveTabIndex(), s_row);
  if (s_status[0]) {
    DrawSmallText(layout, description_x, header_y, Ui("overlay.tab.status"), kGameGold);
    FillLogicalRect(layout, description_x, header_y + 10,
                    bottom_width - 24, 1, structure_dim);
    DrawWrappedSmallText(layout, description_x, header_y + 14,
                         s_status, description_chars, 4, ARGB(255, 208, 220, 232));
  } else if (ActiveTabIsRegional() && s_submenu_open) {
    RegionalMenu_DrawDescription(layout, ActiveTab()->regional_page, s_row,
                                 description_x, header_y, bottom_width - 24);
  } else if (layer_help) {
    LayerMenu_DrawDescription(layout, ActiveTabIndex(), s_row,
                              description_x, header_y, bottom_width - 24);
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
                  SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), selected),
                  structure);
    /* Naming HOW a change takes effect next to the row removes the usual
     * "did that do anything?" question; the '*' row marker only says that a
     * restart is involved, not what the other kinds do. */
    char apply_key[32];
    snprintf(apply_key, sizeof(apply_key), "overlay.apply.%d", selected->apply);
    const char *apply = ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), apply_key,
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
    const char *help = SettingsOverlay_LocalizedHelp(SettingsOverlay_InterfaceLocale(), selected);
    if (Settings_IsRandomizer(selected) && Randomizer_CampaignBound())
      help = Ui("overlay.randomizer.campaign_locked");
    const char *hardware_help = Settings_HardwareUnavailableReason(selected);
    if (hardware_help) help = ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(),
        "overlay.hardware_unavailable", hardware_help);
    if (selected->field == &g_settings.interface_language &&
        !SettingsOverlayWidgets_HasTextBackend()) help = Ui("overlay.font_unavailable");
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
  const InputClass device = SettingsOverlay_MenuInputDevice();
  char select[128], change[128], tabs[128], confirm[64], back[64], reset[64], details[64];
  OverlayMenuInput_PairHint(select, sizeof(select), kMenuNav_Up, kMenuNav_Down, device);
  OverlayMenuInput_PairHint(change, sizeof(change), kMenuNav_Left, kMenuNav_Right, device);
  OverlayMenuInput_PairHint(tabs, sizeof(tabs), kMenuNav_TabPrev, kMenuNav_TabNext, device);
  OverlayMenuInput_Hint(confirm, sizeof(confirm), kMenuNav_Confirm, device);
  OverlayMenuInput_Hint(back, sizeof(back), kMenuNav_Back, device);
  OverlayMenuInput_Hint(reset, sizeof(reset), kMenuNav_Reset, device);
  OverlayMenuInput_Hint(details, sizeof(details), kMenuNav_Details, device);
  const bool back_available = *back != 0;
  if (!back_available) OverlayMenuInput_Hint(back, sizeof(back), kMenuNav_Close, device);
  MenuHints hints = {0};
#define HINT(key, text) AddMenuHint(&hints, key, Ui(text))
  if (s_capture_desc) {
    HINT(Ui("overlay.key.any"), "overlay.hint.bind");
    HINT("ESC", "overlay.hint.cancel");
  } else if (s_editing) {
    HINT(device == kInputClass_Keyboard ? "Return" : confirm, "overlay.hint.apply");
    HINT(device == kInputClass_Keyboard ? "Escape" : (back_available ? back : ""),
         "overlay.hint.cancel");
  } else if (s_submenu_open) {
    /* Omitted when the only row is a notice: there is nothing to select, and
     * offering the verb would suggest otherwise. */
    if (!layer_help || LayerMenu_RowSelectable(ActiveTabIndex(), s_row))
      HINT(select, "overlay.hint.select");
    if (layer_help) {
      LayerMenu_AddHints(&hints, ActiveTabIndex(), s_row, VisibleTabCount(s_section) > 1,
                        change, confirm, tabs, reset);
    } else if (ActiveTabIsRegional()) {
      RegionalMenu_AddHints(&hints, ActiveTab()->regional_page,
                            change, confirm, tabs, reset, details);
    } else if (SelectedRowIsSectionReset()) {
      HINT(confirm, "overlay.hint.reset");
      if (VisibleTabCount(s_section) > 1) HINT(tabs, "overlay.hint.tab");
    } else {
      /* The verbs track what the selected row actually does: an Int row
       * adjusts (hold to accelerate — felt, not spelled out, to keep the line
       * short), a string/mask row opens a text prompt, the rest cycle. */
      const SettingDesc *row = SelectedDesc();
      bool numeric = row && row->type == kSettingType_Int;
      bool textual = row && (row->type == kSettingType_Mask ||
                             row->type == kSettingType_Custom);
      HINT(change, numeric ? "overlay.hint.adjust" : "overlay.hint.change");
      if (VisibleTabCount(s_section) > 1) HINT(tabs, "overlay.hint.tab");
      if (textual) HINT(confirm, "overlay.hint.type");
      HINT(reset, "overlay.hint.reset");
    }
    HINT(back, back_available ? "overlay.hint.back" : "overlay.hint.close");
  } else {
    HINT(select, "overlay.hint.section");
    if (VisibleTabCount(s_section) > 1 &&
        !(section->icon == kOverlayIcon_Save && SaveSlotMenu_Available()))
      HINT(tabs, "overlay.hint.tab");
    HINT(confirm, "overlay.hint.open");
    HINT(back, "overlay.hint.close");
  }
#undef HINT
  DrawMenuHints(layout, description_x, bottom_y + bottom_height - 13 - kSmallLineHeight,
                bottom_width - 24, &hints, kKeyColor, kHintColor);
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
  if(!s_submenu_open && section->icon==kOverlayIcon_Save && SaveSlotMenu_Available()) {
    DrawSectionIcon(layout,chrome.right_text_x,chrome.top_y+8,kIconSize,s_section,true,255);
    DrawTextN(layout,chrome.right_text_x+kIconSize+6,chrome.top_y+10,
        Ui("slots.title"),(chrome.right_width-56)/kGlyphSize,kText_Normal);
    DrawTextN(layout,chrome.right_text_x,chrome.top_y+36,
        Ui("slots.open"),(chrome.right_width-32)/kGlyphSize,kText_Value);
    DrawWrappedSmallText(layout,chrome.right_text_x,chrome.top_y+56,
        Ui("slots.overview"),(chrome.right_width-32)/kDebugGlyphWidth,
        (chrome.top_height-68)/kSmallLineHeight,kSteelBlue);
  } else {
    const int rule_y = DrawMenuHeader(layout, &chrome, section);
    DrawMenuRows(layout, &chrome, section, rule_y, custom_rows);
  }
  DrawMenuFooter(layout, &chrome, section);
}


void SettingsOverlay_Render(ArRenderRectI game_viewport) {
  SettingsOverlay_Refresh();
  if (!s_open || !SettingsOverlayWidgets_RenderDevice() ||
      !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->fonts[kText_Normal])) return;
  int output_width = 0;
  int output_height = 0;
  if (!ArRenderOutput_UseFull(
          SettingsOverlayWidgets_RenderDevice(), &output_width, &output_height))
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
  if (RegionalMenu_DrawConfirmation(&layout)) return;
  if (s_details.open) {
    DrawDetails(&layout);
    return;
  }
  if(SaveSlotMenu_Active() || SaveSlotMenu_DecisionActive()){SaveSlotMenu_Draw(&layout);return;}
  DrawMenu(&layout);
  SettingsOverlayPalette_Draw(&layout);
}
