#define _POSIX_C_SOURCE 200809L

#include "diorama_layer_editor.h"
#include "action/action_bg_tuner.h"
#include "input_map.h"
#include "host/host_display_status.h"
#include "render_capabilities.h"
#include "settings.h"
#include "randomizer.h"
#include "settings_overlay.h"
#include "sim/sim_town_terrain.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

bool g_ws_active;
int g_ws_extra;
int g_ws_display_extra;
uint8 g_ram[0x20000];
/* kSettingCat_Graphics's GpuShadersActive() availability gate reads this
 * (main.c's real runtime state); this harness has no renderer, so it's
 * never actually true here. */
bool g_gpu_shaders_active;
/* W4-2: present.c owns the real value (latched when a renderer rejects the rim
 * mask blend mode); stubbed true here so the row's availability is exercised. */
bool Present_SimRimMaskSupported(void) { return true; }
bool Present_EffectRendererSupported(void) { return true; }
/* Host-side diorama geometry rebind; no renderer in this harness. */
void Diorama_OnModeChanged(void) {}
static int s_failures;
static int s_action_calls;
static const SettingDesc *s_action_desc;
static int s_inspector_info_calls;
static bool s_fake_manual_available = true;

static bool FakeManualAvailable(void) {
  return s_fake_manual_available;
}

static const SettingsOverlayManualHooks kFakeManualHooks = {
  .available = FakeManualAvailable,
};

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static bool ActionObserved(const SettingDesc *desc) {
  s_action_calls++;
  s_action_desc = desc;
  return true;
}

static void InspectorInfo(char *buffer, size_t buffer_size) {
  s_inspector_info_calls++;
  snprintf(buffer, buffer_size,
           "SCENE Fillmore sim  $18/$19 $00/$01\n"
           "GF $1234  HOST 5678  PAUSE MENU\n"
           "CAM $0080,$0040  MAP 512X512\n"
           "PPU MODE 1  MAIN $17 SUB $00\n"
           "MUSIC Fillmore  SONG $01 AUTH");
}

/* The overlay's nav column lists SECTIONS and each section has a tab bar, so
 * these walk to a named destination instead of counting keypresses — the old
 * "press Down four times" style broke every time a row landed above the one
 * under test. */
enum {
  kSection_Video = 0,
  kSection_Diorama,
  kSection_Town3D,
  kSection_Audio,
  kSection_Controls,
  kSection_Cheats,
  kSection_Save,
  /* The in-game manual: a player-facing section, so it sits ahead of System's
   * host commands. */
  kSection_Manual,
  kSection_System,
  /* Developer-only until a seeded run has been played end to end, so it sits
   * with Layers rather than among the player sections. */
  kSection_Randomizer,
  /* Developer-only, so it is present in the nav column only while
   * show_debug_settings is on. Last on purpose: every section above keeps the
   * same ordinal whether it is shown or hidden. */
  kSection_Layers,
};
/* Sections a player sees, and the total with developer tools revealed. Named so
 * the assertions below say which one they mean rather than repeating an enum
 * arithmetic expression that reads the same for both. */
enum {
  /* The first hidden section marks the end of the player-visible run; both
   * sections past it are debug-gated. */
  kPlayerSectionCount = kSection_Randomizer,
  kDebugSectionCount = kSection_Layers + 1,
  kPlayerSectionCountWithoutManual = kPlayerSectionCount - 1,
  kSystemVisibleOrdinalWithoutManual = kSection_System - 1,
};

/* Call from the nav column (not inside a submenu). */
static void NavToSection(int target) {
  for (int guard = 0; guard < 24; guard++) {
    int selected = -1;
    CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
    if (selected == target) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"section not reachable");
}

/* A build with no staged PDF must not advertise a dead Manual destination.
 * Exercise this through the same availability hook used by main.c, including
 * both directions around the hole so section movement cannot land on it. */
static void CheckManualSectionAvailability(void) {
  g_settings.show_debug_settings = false;
  s_fake_manual_available = true;

  int selected = -1;
  int total = -1;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(total == kPlayerSectionCount);

  NavToSection(kSection_Save);
  s_fake_manual_available = false;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected == kSection_Save);
  CHECK(total == kPlayerSectionCountWithoutManual);

  /* DOWN skips the hidden raw Manual section and lands on System. Its visible
   * ordinal closes the gap, then another DOWN wraps to Video. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSystemVisibleOrdinalWithoutManual);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSection_Video);

  /* UP must likewise skip the missing section. Restoring availability inserts
   * Manual back ahead of System and makes it reachable again. */
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSystemVisibleOrdinalWithoutManual);
  s_fake_manual_available = true;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected == kSection_System);
  CHECK(total == kPlayerSectionCount);
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, NULL));
  CHECK(selected == kSection_Manual);

  /* Restore the fixture state expected by the exhaustive menu checks below. */
  g_settings.show_debug_settings = true;
  NavToSection(kSection_Video);
}

/* Tabs are remembered per section, so step forward (wrapping) until the
 * wanted one is active rather than assuming we start at zero. */
static void NavToTab(int target) {
  for (int guard = 0; guard < 12; guard++) {
    int active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, NULL));
    if (active == target) return;
    /* ']' is the layout-independent next-tab key; the primary L/R keys follow
     * the player's own SNES bindings and are exercised separately below. */
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
  }
  CHECK(!"tab not reachable");
}

static void RowToKey(const char *key) {
  for (int guard = 0; guard < 80; guard++) {
    if (!strcmp(SettingsOverlay_SelectedKey(), key)) return;
    CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  }
  CHECK(!"row not reachable");
}

static uint8_t *ReadOptionalRom(size_t *size_out) {
  const char *path = getenv("AR_OVERLAY_TEST_ROM");
  *size_out = 0;
  if (!path || !path[0]) return NULL;
  FILE *file = fopen(path, "rb");
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *data = (uint8_t *)malloc((size_t)size);
  if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size_out = (size_t)size;
  return data;
}

/* The gamepad menu gate remains connection-based. Keyboard arbitration is
 * activity-based in Auto: an idle connected pad does not lock out a keyboard,
 * but a native pad event wins over a simultaneous Steam-generated key. */
static bool MenuGamepadOwns(int input_device, int gamepad_count) {
  return input_device != kInputDevice_Keyboard && gamepad_count > 0;
}
static void CheckMenuDeviceGateTruthTable(void) {
  const struct {
    int input_device;
    int gamepad_count;
    bool pad_input_active;
    bool gamepad_enabled;
    bool keyboard_active;
  } rows[] = {
      /* input_device,       pads, active, gamepad, keyboard */
      { kInputDevice_Auto,     0, false, false, true  },
      { kInputDevice_Auto,     1, false, true,  true  },
      { kInputDevice_Auto,     1, true,  true,  false },
      { kInputDevice_Keyboard, 0, false, false, true  },
      { kInputDevice_Keyboard, 2, true,  false, true  },
      { kInputDevice_Gamepad,  1, false, true,  false },
      { kInputDevice_Gamepad,  0, false, false, true  },
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    bool pad = MenuGamepadOwns(rows[i].input_device, rows[i].gamepad_count);
    bool kbd = InputMap_ShouldAcceptKeyboard(
        (InputDeviceMode)rows[i].input_device, rows[i].gamepad_count > 0,
        rows[i].pad_input_active);
    CHECK(pad == rows[i].gamepad_enabled);
    CHECK(kbd == rows[i].keyboard_active);
    /* At least one device is always active — never a total lockout. */
    CHECK(pad || kbd);
  }

  const uint32 keyboard = (1u << kInputAction_Up) |
                          (1u << kInputAction_B);
  const uint32 gamepad = (1u << kInputAction_Right);
  CHECK(InputMap_ArbitrateState(kInputDevice_Auto, true, true,
                                keyboard, gamepad) == gamepad);
  CHECK(InputMap_ArbitrateState(kInputDevice_Auto, true, false,
                                keyboard, gamepad) == keyboard);
  CHECK(InputMap_ArbitrateState(kInputDevice_Gamepad, false, false,
                                keyboard, gamepad) == keyboard);
}

/* ── Layer editor harness ────────────────────────────────────────────────
 *
 * The overlay reaches the override table through injected hooks precisely so
 * this test can supply its own without linking diorama.c (which would drag in
 * the PPU and the SDL render path). These fakes are the whole reason the
 * indirection exists. */
static DioramaLayerOrderTable s_fake_layer_table;
static bool s_fake_room_live;
static uint8_t s_fake_group = 0x01;   /* Fillmore */
static uint8_t s_fake_map = 0x02;     /* act 2, the reported room */
static uint8_t s_fake_section = kDioramaLayerSection_Room;
static int s_fake_saves;
static uint16_t s_fake_cgram[kSettingsOverlayLayerPaletteEntries];

static DioramaLayerOrderTable *FakeLayerTable(void) {
  return &s_fake_layer_table;
}

static bool FakeLayerRoom(uint8_t *group, uint8_t *map, uint8_t *section) {
  if (!s_fake_room_live) return false;
  if (group) *group = s_fake_group;
  if (map) *map = s_fake_map;
  if (section) *section = s_fake_section;
  return true;
}

static bool FakeLayerSave(void) {
  s_fake_saves++;
  return true;
}

static bool FakeLayerPalette(
    uint16_t out_cgram[kSettingsOverlayLayerPaletteEntries]) {
  memcpy(out_cgram, s_fake_cgram, sizeof(s_fake_cgram));
  return true;
}

/* Drive the Layers section the way a player would: keys only, no direct calls
 * into the row model. What is asserted is the WIRING -- that a keypress reaches
 * the override table, that the cursor never rests on a caption, that the section
 * disappears without debug settings, and that an edit persists. The row model
 * itself is covered by tests/diorama_layer_editor_test.c. */
static void CheckLayerEditorSection(void) {
  SettingsOverlay_SetLayerEditorHooks(FakeLayerTable, FakeLayerRoom,
                                      FakeLayerSave);
  SettingsOverlay_SetLayerPaletteProvider(FakeLayerPalette);
  memset(&s_fake_layer_table, 0, sizeof(s_fake_layer_table));
  for (int i = 0; i < kSettingsOverlayLayerPaletteEntries; i++)
    s_fake_cgram[i] = (uint16_t)i;
  s_fake_room_live = true;
  s_fake_section = kDioramaLayerSection_Room;
  s_fake_saves = 0;

  /* THE GATE the feature was asked for: developer-only means the section is not
   * in the nav column at all for a player, not merely that its rows are. */
  g_settings.show_debug_settings = false;
  int total = -1;
  CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, &total));
  CHECK(total == kPlayerSectionCount);

  /* Stepping DOWN from the last visible section must WRAP to the first, not walk
   * onto the hidden one. Asserted from the section above it, since a wrap that
   * landed on Layers would report an out-of-range position rather than 0.
   *
   * Without this the only evidence would be positional, and Layers is last -- so
   * its raw index and its visible position coincide and a MoveSection that
   * happily lands on a hidden section looks identical to one that skips it.
   *
   * Driven from the last PLAYER-visible section, which is the property under
   * test -- not that section's name. */
  NavToSection(kSection_System);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  int wrapped = -1;
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, &total));
  CHECK(wrapped == 0);
  CHECK(total == kPlayerSectionCount);
  /* And UP from the first must reach System, not the hidden section past it. */
  CHECK(SettingsOverlay_HandleKey(SDLK_UP, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_System);

  /* The randomizer is gated at BOTH levels: its section is debug_only so the
   * nav column omits it (asserted by the count above), and its rows are
   * debug-only so nothing can surface them from another tab. Checked with the
   * flag still off, which is the state a player ships with. */
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_enable")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_statue_drops")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("rando_lair_types")));

  g_settings.show_debug_settings = true;
  CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, &total));
  CHECK(total == kDebugSectionCount);
  /* ...and revealed by the same flag, so the gate is a switch and not a wall. */
  CHECK(Settings_IsMenuVisible(Settings_Find("rando_enable")));
  /* With debug on, DOWN from System reaches the first hidden section and one
   * more DOWN reaches the last -- which is what pins the reported ordinal to
   * the VISIBLE numbering that the nav column draws in. */
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_Randomizer);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_GetNavigationState(&wrapped, NULL, NULL, NULL));
  CHECK(wrapped == kSection_Layers);

  /* Give the randomizer a ROM image to own before touching its rows: its gated
   * rows require a snapshot, and a synthetic buffer exercises Init/Apply's
   * bounds handling on a degenerate image at the same time. */
  {
    static uint8_t fake_rom[0x100000];
    CHECK(Randomizer_Init(fake_rom, (uint32_t)sizeof fake_rom));
  }
  /* The randomizer section: four tabs, and every row the player edits must be
   * reachable by name. Its rows are gated on the master being on and on the ROM
   * snapshot existing, so this drives the master first -- otherwise the gated
   * rows are legitimately absent and "reachable" would prove nothing. Seed sits
   * on tab 0 beside the master; the per-area rows are on their own tabs. */
  NavToSection(kSection_Randomizer);
  {
    int tabs = 0;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 4);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* into the rows */
  NavToTab(0);
  RowToKey("rando_enable");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* master on */
  CHECK(g_settings.rando_enable);
  NavToTab(0);
  RowToKey("rando_seed");
  RowToKey("rando_reroll");
  NavToTab(1);
  RowToKey("rando_enemy_hp");
  RowToKey("rando_enemy_types");
  NavToTab(2);
  RowToKey("rando_statue_drops");
  RowToKey("rando_statue_spots");
  NavToTab(3);
  RowToKey("rando_lair_spots");
  RowToKey("rando_lair_types");
  /* Turning the master back off must retract the gated rows, which is what
   * stops the menu offering edits that would do nothing. */
  NavToTab(0);
  RowToKey("rando_enable");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(!g_settings.rando_enable);
  CHECK(!Settings_IsAvailable(Settings_Find("rando_seed")));
  /* Back out to the nav column so the sections below start where they expect. */
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  NavToSection(kSection_Layers);
  /* Tab 0 is Fillmore, which is where the fake room is. */
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* open submenu */

  /* The cursor must never rest on the room caption, which is row 0 and is not
   * selectable -- so opening the submenu has already stepped past it onto a real
   * row. Asserted by name rather than by index. */
  CHECK(strcmp(SettingsOverlay_SelectedKey(), "") != 0);

  /* The list must contain NO row the editor does not own. Two ways it could:
   * the synthetic "Reset <section> defaults" row (which has no registry
   * categories to act on here), and a settings descriptor matched by row index
   * because SelectedDesc walked the registry. Walking the whole list and
   * requiring every row to report an editor-shaped key catches both -- an
   * off-by-one row past the end reports "" and a descriptor reports its own key,
   * neither of which is a plane token. */
  {
    int rows = 0;
    CHECK(SettingsOverlay_GetNavigationState(NULL, NULL, NULL, NULL));
    /* Step through more rows than any tab has, checking each landing. */
    for (int i = 0; i < 48; i++) {
      const char *key = SettingsOverlay_SelectedKey();
      CHECK(strcmp(key, "") != 0);
      CHECK(strcmp(key, "reset_section_defaults") != 0);
      /* Every editor key is a plane token, a "token.param", or the room reset. */
      const bool known = !strcmp(key, "layer_reset_room") ||
                         DioramaLayerOrder_PlaneFromToken(key) >= 0 ||
                         strchr(key, '.') != NULL;
      CHECK(known);
      if (Settings_Find(key)) CHECK(!"editor row matched a settings descriptor");
      rows++;
      CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
    }
    CHECK(rows == 48);
  }

  /* Cycle the water plane's shape. Navigating by name, because the row list's
   * shape changes with the active shape and a keypress count would break. */
  RowToKey("bg2hi");
  const int saves_before = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_fake_saves > saves_before);   /* the edit was persisted */
  const DioramaRoomOverride *room =
      DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(room != NULL);
  CHECK(DioramaLayerOrder_RoomIsActive(room));
  /* One Right from FLAT is RAKE, and cycling a plane expands it -- so its depth
   * row now exists and is reachable. Both are contracts, not incidentals: the
   * expansion is what puts the parameters under the cursor after a change. */
  const DioramaPlaneOverride *water = &room->planes[kDioramaPlane_Bg2Hi];
  CHECK(DioramaLayerEditor_StrategyOfPlane(water) == kDioramaDepth_Rake);
  RowToKey("bg2hi.depth");
  /* Stepping the depth row moves the rake itself, not some other key. */
  const float rake_before = water->rake;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(water->rake > rake_before);

  /* The menu's reset verb is SNES Y, whose kDefaults keyboard binding is A --
   * not SDLK_Y. Clearing the depth removes the SHAPE, since
   * a rake of zero is not a shape, so the plane reads FLAT again. */
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  CHECK(DioramaLayerEditor_StrategyOfPlane(water) == kDioramaDepth_Flat);

  /* Author two planes, then confirm the room reset clears both at once. */
  RowToKey("bg2hi");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  RowToKey("bg1");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(DioramaLayerOrder_RoomIsActive(room));

  RowToKey("layer_reset_room");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* confirm */

  /* A room with no overrides left must be INACTIVE, so Resolve returns the
   * built-in table and the unedited-game guarantee holds. */
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(!room || !DioramaLayerOrder_RoomIsActive(room));

  /* Base BG1/BG2 expose a live 16x16 CGRAM picker. The selection stores the
   * index (not the sampled RGB), so palette animation remains live. Starting at
   * $00, Right, Right, Down lands on $12. */
  RowToKey("bg2");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("bg2.transparent");
  const int saves_before_palette = s_fake_saves;
  /* Opening and cancelling the picker is read-only: it must not consume one
   * of the bounded room override slots before a colour is confirmed. */
  CHECK(!DioramaLayerOrder_Find(
      &s_fake_layer_table, s_fake_group, s_fake_map));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(!DioramaLayerOrder_Find(
      &s_fake_layer_table, s_fake_group, s_fake_map));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  const DioramaPlaneOverride *filled =
      room ? &room->planes[kPpuOverlaySource_Bg2] : NULL;
  CHECK(filled && filled->set_transparent_fill);
  CHECK(filled && filled->transparent_fill_kind ==
                      kDioramaTransparentFill_Cgram);
  CHECK(filled && filled->transparent_fill_cgram == 0x12);
  CHECK(s_fake_saves > saves_before_palette);
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(room == NULL);  /* clearing the final key recycles the bounded slot */

  /* The production room hook also carries a camera-local section. Edits while
   * that section is live must land in its refining record, never the base room. */
  s_fake_section = kDioramaLayerSection_AitosWaterfall;
  RowToKey("backdrop");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* expand */
  RowToKey("backdrop.source");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  const DioramaRoomOverride *scoped = DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section);
  CHECK(scoped != NULL);
  CHECK(scoped && scoped->planes[kDioramaPlane_Backdrop].set_source);
  CHECK(scoped && scoped->planes[kDioramaPlane_Backdrop].source ==
                      DioramaLayerOrder_ActionBgSource(0x01, 0x01, 1));
  room = DioramaLayerOrder_Find(&s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(!room || !DioramaLayerOrder_RoomIsActive(room));

  /* Left in a scoped section authors OFF, rather than merely clearing the
   * local key and exposing an inherited base-room fill again. */
  DioramaRoomOverride *base = DioramaLayerOrder_FindOrAdd(
      &s_fake_layer_table, s_fake_group, s_fake_map);
  CHECK(base != NULL);
  if (base) {
    base->planes[kPpuOverlaySource_Bg2].set_transparent_fill = true;
    base->planes[kPpuOverlaySource_Bg2].transparent_fill_kind =
        kDioramaTransparentFill_Black;
  }
  RowToKey("bg2");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("bg2.transparent");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  scoped = DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section);
  CHECK(scoped &&
        scoped->planes[kPpuOverlaySource_Bg2].set_transparent_fill);
  CHECK(scoped &&
        scoped->planes[kPpuOverlaySource_Bg2].transparent_fill_kind ==
            kDioramaTransparentFill_None);
  DioramaTransparentFill effective_fill = kDioramaTransparentFill_Black;
  uint8_t effective_cgram = 0xff;
  CHECK(DioramaLayerOrder_ResolveTransparentFill(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section,
      kPpuOverlaySource_Bg2, &effective_fill, &effective_cgram));
  CHECK(effective_fill == kDioramaTransparentFill_None);
  RowToKey("layer_reset_room");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!DioramaLayerOrder_FindSection(
      &s_fake_layer_table, s_fake_group, s_fake_map, s_fake_section));
  DioramaLayerOrder_ResetRoom(
      &s_fake_layer_table, s_fake_group, s_fake_map);
  s_fake_section = kDioramaLayerSection_Room;

  /* The final Layers tab is a separate, session-only action-BG tuner. It uses
   * the live canonical plan even when Diorama itself is not the provider, and
   * must never write diorama-layers.ini through the hooks above. */
  {
    ActionBgPlan canonical;
    ActionBgPlan_InitNative(&canonical);
    canonical.layer[0].role = kActionBgLayerRole_Playfield;
    canonical.layer[0].source = kActionBgSource_WorldMap;
    canonical.layer[0].world_width = 4096;
    canonical.layer[0].world_height = 512;
    canonical.layer[0].default_edge = kActionBgEdge_LiveWorld;
    canonical.layer[1].role = kActionBgLayerRole_Backdrop;
    canonical.layer[1].source = kActionBgSource_AuthenticViewport;
    canonical.layer[1].world_width = 256;
    canonical.layer[1].world_height = 256;
    canonical.layer[1].default_edge = kActionBgEdge_Mirror;
    canonical.layer[1].vertical_extent = (ActionBgVerticalExtent) {
      .mode = kActionBgExtent_Fixed, .top = 8, .bottom = 12,
    };
    ActionBgTuner_ResetSession();
    CHECK(ActionBgTuner_ObservePlan(
        1, 1, &canonical, (ActionBgTunerLimits){120, 120, 32, 32}));
    const int saves_before_bg_tuner = s_fake_saves;
    NavToTab(kDioramaEditorLevelCount);
    RowToKey("bg_tuner.apply");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(ActionBgTuner_DraftEnabled());
    RowToKey("bg2");
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    RowToKey("bg2.ignore_side_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    ActionBgPlan unbounded = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&unbounded));
    CHECK(unbounded.layer[1].horizontal_extent.mode ==
          kActionBgExtent_Available);
    RowToKey("bg2.ignore_vertical_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    unbounded = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&unbounded));
    CHECK(unbounded.layer[1].vertical_extent.mode ==
          kActionBgExtent_Available);
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    RowToKey("bg2.ignore_side_bounds");
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    RowToKey("bg2.horizontal");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    RowToKey("bg2.left");
    CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
    ActionBgPlan applied = canonical;
    CHECK(ActionBgTuner_ApplyDraft(&applied));
    CHECK(applied.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
    CHECK(applied.layer[1].horizontal_extent.left == 116);
    RowToKey("bg_tuner.guides");
    CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
    CHECK(ActionBgTuner_GuidesEnabled());
    CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
    CHECK(!ActionBgTuner_GuidesEnabled());
    RowToKey("bg_tuner.reset");
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    CHECK(!ActionBgTuner_DraftEnabled());
    CHECK(s_fake_saves == saves_before_bg_tuner);
  }

  /* A level the player is not in explains itself rather than editing something.
   * Bloodpool is tab 1; the fake room is Fillmore. */
  NavToTab(1);
  const int saves_at_foreign = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(s_fake_saves == saves_at_foreign);   /* nothing authored */

  /* And with no room live at all, no tab edits anything. */
  NavToTab(0);
  s_fake_room_live = false;
  const int saves_offline = s_fake_saves;
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_fake_saves == saves_offline);
  s_fake_room_live = true;

  /* Turning debug settings off while standing IN the editor must move focus out
   * rather than leave the cursor on a hidden section. */
  g_settings.show_debug_settings = false;
  int selected = -1;
  CHECK(SettingsOverlay_GetNavigationState(&selected, NULL, NULL, &total));
  CHECK(selected != kSection_Layers);
  CHECK(total == kPlayerSectionCount);
  g_settings.show_debug_settings = true;

  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));   /* leave submenu */
  /* Leave no hooks behind: later blocks drive other sections. */
  SettingsOverlay_SetLayerEditorHooks(NULL, NULL, NULL);
  SettingsOverlay_SetLayerPaletteProvider(NULL);
  ActionBgTuner_ResetSession();
}

int main(void) {
  char settings_path[160];
  char settings_temporary[164];
  snprintf(settings_path, sizeof(settings_path),
           "/tmp/actraiser-overlay-settings-%ld.ini", (long)getpid());
  snprintf(settings_temporary, sizeof(settings_temporary), "%s.tmp",
           settings_path);
  setenv("AR_OVERLAY_TEST_SETTINGS_PATH", settings_path, 1);
  remove(settings_path);
  remove(settings_temporary);
  setenv("SDL_VIDEODRIVER", "dummy", 1);
  setenv("SDL_AUDIODRIVER", "dummy", 1);

  g_ws_active = true;
  g_ws_extra = g_ws_display_extra = 43;
  Settings_ClearConfigLayer();
  Settings_Init();
  CheckMenuDeviceGateTruthTable();
  Settings_SetActionObserver(ActionObserved);
  /* Most of this test drives every tab and row, including the developer-only
   * ones (the town Light/Weather dials, the inspector). Turn debug settings on
   * so they are all present; a dedicated block below toggles it back off and
   * checks that they collapse. */
  g_settings.show_debug_settings = true;

  int surface_width = 640;
  int surface_height = 480;
  const char *preview_size = getenv("AR_OVERLAY_TEST_SIZE");
  if (preview_size)
    (void)sscanf(preview_size, "%dx%d", &surface_width, &surface_height);
  if (surface_width <= 0) surface_width = 640;
  if (surface_height <= 0) surface_height = 480;

  CHECK(SDL_Init(SDL_INIT_VIDEO));
  SDL_Surface *surface = SDL_CreateSurface(
      surface_width, surface_height, SDL_PIXELFORMAT_ARGB8888);
  CHECK(surface != NULL);
  SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
  CHECK(renderer != NULL);
  size_t rom_size = 0;
  uint8_t *rom_data = ReadOptionalRom(&rom_size);
  CHECK(SettingsOverlay_Init(renderer, rom_data, rom_size));
  SettingsOverlay_SetManualHooks(&kFakeManualHooks);
  /* Device-reset recovery: rebuild every atlas in place. All rendering below
   * runs against the REBUILT textures, so a broken reload shows up in the
   * preview captures too. */
  CHECK(SettingsOverlay_ReloadTextures(rom_data, rom_size));
  SettingsOverlay_SetInspectorInfoProvider(InspectorInfo);
  free(rom_data);

  /* Output-space text is shared by the manual and the FPS counter. Keep a
   * real software-renderer regression around the batched glyph path so a bad
   * index/UV layout cannot turn the performance overlay into an invisible
   * one while the ordinary per-glyph menu continues to pass. */
  if (renderer && surface) {
    CHECK(SDL_SetRenderLogicalPresentation(
        renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED));
    CHECK(SDL_SetRenderViewport(renderer, NULL));
    CHECK(SDL_SetRenderClipRect(renderer, NULL));
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    CHECK(SDL_RenderClear(renderer));
    SettingsOverlay_DrawGameText(8, 8, 2, 255, "FPS 123.4");
    CHECK(SDL_RenderPresent(renderer));
    int changed_pixels = 0;
    const int text_width = SettingsOverlay_GameTextWidth("FPS 123.4", 2);
    for (int y = 8; y < 8 + 2 * kSettingsOverlayGlyphSize; y++)
      for (int x = 8; x < 8 + text_width; x++) {
        Uint8 r = 0, g = 0, b = 0, a = 0;
        CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
        if (r != 32 || g != 24 || b != 16) changed_pixels++;
      }
    CHECK(changed_pixels > 0);
  }

  /* Headless SDL reports no refresh rate; let a preview inject one so the
   * "Vsync NHz" row can be eyeballed. */
  const char *refresh_hz = getenv("AR_OVERLAY_TEST_REFRESH_HZ");
  if (refresh_hz && refresh_hz[0]) {
    HostDisplayStatus_SetNominalRefreshHz(atoi(refresh_hz));
    HostDisplayStatus_SetVsyncActive(true);
  }

  SettingsOverlay_Open();
  CHECK(SettingsOverlay_IsOpen());
  CheckManualSectionAvailability();
  if (renderer) {
    /* Fullscreen 4:3 leaves SDL's game presentation pillarboxed on a wide
     * output. A direct overlay call must temporarily discard that coordinate
     * space and game-local clip, draw into the bars, then restore both. */
    CHECK(SDL_SetRenderLogicalPresentation(
        renderer, 1024, 768, SDL_LOGICAL_PRESENTATION_LETTERBOX));
    SDL_Rect game_clip = { 0, 0, 1024, 768 };
    CHECK(SDL_SetRenderClipRect(renderer, &game_clip));
    int expected_logical_width = -1, expected_logical_height = -1;
    SDL_RendererLogicalPresentation expected_logical_mode =
        SDL_LOGICAL_PRESENTATION_DISABLED;
    CHECK(SDL_GetRenderLogicalPresentation(
        renderer, &expected_logical_width, &expected_logical_height,
        &expected_logical_mode));
    const bool expected_viewport_set = SDL_RenderViewportSet(renderer);
    SDL_Rect expected_viewport = {0};
    CHECK(SDL_GetRenderViewport(renderer, &expected_viewport));
    const bool expected_clip_enabled = SDL_RenderClipEnabled(renderer);
    SDL_Rect expected_clip = {0};
    CHECK(SDL_GetRenderClipRect(renderer, &expected_clip));
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    int logical_width = -1, logical_height = -1;
    SDL_RendererLogicalPresentation logical_mode =
        SDL_LOGICAL_PRESENTATION_LETTERBOX;
    CHECK(SDL_GetRenderLogicalPresentation(
        renderer, &logical_width, &logical_height, &logical_mode));
    CHECK(logical_width == expected_logical_width &&
          logical_height == expected_logical_height);
    CHECK(logical_mode == expected_logical_mode);
    CHECK(SDL_RenderViewportSet(renderer) == expected_viewport_set);
    SDL_Rect overlay_viewport = { -1, -1, -1, -1 };
    CHECK(SDL_GetRenderViewport(renderer, &overlay_viewport));
    CHECK(SDL_RectsEqual(&overlay_viewport, &expected_viewport));
    CHECK(SDL_RenderClipEnabled(renderer) == expected_clip_enabled);
    SDL_Rect overlay_clip = {0};
    CHECK(SDL_GetRenderClipRect(renderer, &overlay_clip));
    CHECK(SDL_RectsEqual(&overlay_clip, &expected_clip));
    SDL_RenderPresent(renderer);
    /* On a wide target, surviving pixels in the left pillar prove this was a
     * real full-output draw rather than state bookkeeping alone. */
    const int content_width = surface_height * 4 / 3;
    const int pillar_width = (surface_width - content_width) / 2;
    if (pillar_width > 0 && surface) {
      int changed = 0;
      for (int y = 0; y < surface_height && changed == 0; y++) {
        for (int x = 0; x < pillar_width; x++) {
          Uint8 r = 0, g = 0, b = 0, a = 0;
          CHECK(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
          if (r != 32 || g != 24 || b != 16) {
            changed++;
            break;
          }
        }
      }
      CHECK(changed > 0);
    }
    const char *preview = getenv("AR_OVERLAY_TEST_BMP");
    if (preview && preview[0]) CHECK(SDL_SaveBMP(surface, preview));
    /* Scroll to the Refresh rate row and capture it to eyeball the Hz label. */
    const char *rpreview = getenv("AR_OVERLAY_TEST_REFRESH_BMP");
    if (rpreview && rpreview[0]) {
      NavToSection(kSection_Video);
      NavToTab(0);
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      RowToKey("refresh_mode");
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, rpreview));
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
  }

  /* Tab cycling follows the player's OWN SNES L/R keyboard bindings, not
   * hardcoded keys. With the defaults (L=Q, R=W) both directions must work
   * from the nav column — the R half of this regressed once because the
   * overlay guessed Q/E instead of consulting the bindings. */
  NavToSection(kSection_Video);
  {
    int start = -1, count = -1;
    CHECK(SettingsOverlay_GetTabState(&start, &count));
    CHECK(count >= 2);
    CHECK(SettingsOverlay_HandleKey(SDLK_W, true, false));  /* SNES R */
    int after_r = -1;
    CHECK(SettingsOverlay_GetTabState(&after_r, NULL));
    CHECK(after_r == (start + 1) % count);
    CHECK(SettingsOverlay_HandleKey(SDLK_Q, true, false));  /* SNES L */
    int after_l = -1;
    CHECK(SettingsOverlay_GetTabState(&after_l, NULL));
    CHECK(after_l == start);
  }

  /* The overlay opens on primary navigation. B enters the selected section;
   * only then do Up/Down select rows and Left/Right edit values. */
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "menu_scale_percent"));
  /* menu_scale is an Int row: the press applies the step live, and releasing
   * the key flushes the deferred settings.ini write (numeric rows no longer
   * open a text editor). */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  int auto_x = surface_width * 100 / 464;
  int auto_y = surface_height * 100 / 208;
  int auto_scale = auto_x < auto_y ? auto_x : auto_y;
  auto_scale = auto_scale / 25 * 25;
  if (auto_scale < 25) auto_scale = 25;
  if (auto_scale > 800) auto_scale = 800;
  int expected_scale = auto_scale < 800 ? auto_scale + 25 : 800;
  CHECK(g_settings.menu_scale_percent == expected_scale);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release flushes */
  FILE *saved = fopen(settings_path, "rb");
  CHECK(saved != NULL);
  if (saved) fclose(saved);

  /* Aspect rows share the Video section's General tab. */
  RowToKey("extended_aspect");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_169);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.extended_aspect == kScreenAspect_1610);

  /* Widescreen is now a TAB of Video rather than its own nav entry, and
   * switching tabs must swap the row list without leaving the section.
   * Index 3: Video's tabs are General, Effects, CRT, Widescreen — this index
   * moves whenever a tab is inserted before it. */
  NavToTab(3);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "ws_action"));
  NavToTab(0);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "display_mode"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Drive the Town 3D section through the same key path a user does. This
   * guards both the master toggle and the A/B stage selectors against
   * becoming display-only rows. */
  NavToSection(kSection_Town3D);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_mode"));
  CHECK(!g_settings.sim3d_mode);
  CHECK(!g_settings.sim3d_world_navigation);
  CHECK(g_settings.sim3d_world_navigation_lighting);
  CHECK(!g_settings.sim3d_world_navigation_clouds);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_mode);
  RowToKey("sim3d_voxel_preset");
  CHECK(g_settings.sim3d_voxel_preset ==
        kSimBackgroundVoxelPreset_Balanced);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_voxel_preset ==
        kSimBackgroundVoxelPreset_Custom);
#if AR_SIM3D_TERRAIN_ELEVATION
  RowToKey("sim3d_landscape_height_pct");
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct -
            kSimTownTerrainLandscapeHeightStepPct);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_landscape_height_pct ==
        kSimTownTerrainLandscapeHeightDefaultPct);
#endif
  RowToKey("sim3d_voxel_detail");
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_High);
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_Ultra);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_detail == kSimBackgroundVoxelDetail_High);
  RowToKey("sim3d_voxel_lod");
  CHECK(g_settings.sim3d_voxel_lod == kSimBackgroundVoxelLod_Adaptive);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_lod == kSimBackgroundVoxelLod_Fixed);
  RowToKey("sim3d_voxel_shading");
  CHECK(g_settings.sim3d_voxel_shading ==
        kSimBackgroundVoxelShading_MaterialAware);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.sim3d_voxel_shading ==
        kSimBackgroundVoxelShading_AmbientOcclusion);
  RowToKey("sim3d_voxel_style");
  CHECK(g_settings.sim3d_voxel_style == kSimBackgroundVoxelStyle_Varied);
  RowToKey("sim3d_voxel_facing");
  CHECK(g_settings.sim3d_voxel_facing ==
        kSimBackgroundVoxelFacing_PerModel);
  RowToKey("sim3d_voxel_render_scale");
  CHECK(g_settings.sim3d_voxel_render_scale ==
        kSimBackgroundVoxelRenderScale_PixelClean);
  /* Walk to a stage toggle by key rather than counting rows: the stage list
   * grows every time a render stage lands. Toggling one from the menu must
   * also change what the renderer is asked for, since the fold is the only
   * thing standing between these rows and the frame payload. */
  RowToKey("sim3d_shadows");
  CHECK(g_settings.sim3d_shadows);
  CHECK(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!g_settings.sim3d_shadows);
  CHECK(!(Settings_Sim3DRequestedFeatures() & kSimFeature_Shadows));
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.sim3d_shadows);
  /* The camera/light/weather splits are separate tabs of the same section. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_camera_mode"));
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_shadow_opacity_pct"));
  NavToTab(3);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "sim3d_underlay_haze_pct"));

  /* Every tab ends with one section-scoped reset button. Town 3D spans four
   * registry categories, including hidden developer dials, and the button
   * must restore all four without touching Audio (or any other section).
   * The first confirm only arms it; the second performs and persists it. */
  const SettingDesc *sim_tilt = Settings_Find("sim3d_tilt_x_mrad");
  const SettingDesc *sim_shadow = Settings_Find("sim3d_shadow_opacity_pct");
  const SettingDesc *sim_corner = Settings_Find("sim3d_cull_corner_px");
  const SettingDesc *volume = Settings_Find("audio_master_volume");
  CHECK(Settings_SetLong(sim_tilt, sim_tilt->defval - sim_tilt->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(sim_shadow, sim_shadow->defval - sim_shadow->step) ==
        kSettingChange_Applied);
  CHECK(Settings_SetLong(sim_corner, 0) == kSettingChange_Applied);
  CHECK(Settings_SetLong(volume, 75) == kSettingChange_Applied);
  CHECK(g_settings.sim3d_mode);  /* made non-default by the Scene test above */
  RowToKey("reset_section_defaults");
  const char *reset_preview = getenv("AR_OVERLAY_RESET_TEST_BMP");
  if (renderer && reset_preview && reset_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, reset_preview));
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* arm */
  CHECK(g_settings.sim3d_mode);
  CHECK(g_settings.sim3d_tilt_x_mrad != sim_tilt->defval);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));  /* confirm */
  CHECK(!g_settings.sim3d_mode);
  CHECK(g_settings.sim3d_tilt_x_mrad == sim_tilt->defval);
  CHECK(g_settings.sim3d_shadow_opacity_pct == sim_shadow->defval);
  CHECK(g_settings.sim3d_cull_corner_px == sim_corner->defval);
  CHECK(g_settings.audio_master_volume == 75);
  CHECK(Settings_Reset(volume) == kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Audio frequency is a bounded preset selector, not an arbitrary integer
   * editor. Audio starts with Enable audio, then Audio frequency. The
   * default is Auto (device-native, Hz==0 sentinel); stepping LEFT pins the
   * explicit 48 kHz preset. */
  NavToSection(kSection_Audio);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("audio_frequency");
  CHECK(g_settings.audio_frequency == kAudioFrequency_Auto);
  CHECK(Settings_AudioFrequencyHz() == 0);
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));
  CHECK(g_settings.audio_frequency == kAudioFrequency_48000);
  CHECK(Settings_AudioFrequencyHz() == 48000);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Hold-to-accelerate lives on Camera sensitivity: a wide 10..400 Int with a
   * base step of 1, so the ramp is actually visible (unlike a 0..100 row whose
   * coarse step would equal its base). A tap steps by one and — proven by the
   * value moving at all — never opens a text editor (BeginEditing would leave
   * the value untouched). Holding then releasing flushes one deferred save. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  const SettingDesc *sens = Settings_Find("input_cam_sensitivity");
  CHECK(sens && sens->type == kSettingType_Int && sens->step == 1);
  int sens_default = g_settings.input_cam_sensitivity;
  CHECK(Settings_SetLong(sens, 150) >= kSettingChange_Applied);  /* != default */
  RowToKey("input_cam_sensitivity");
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, true, false));   /* tap down */
  CHECK(g_settings.input_cam_sensitivity == 149);  /* stepped, not text-edited */
  CHECK(!SettingsOverlay_IsEditing());            /* numeric never opens a field */
  CHECK(SettingsOverlay_HandleKey(SDLK_LEFT, false, false));  /* release */
  /* Confirm (B) on a numeric row is a single fine step up, still no editor. */
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(g_settings.input_cam_sensitivity == 150);
  CHECK(!SettingsOverlay_IsEditing());

  /* The pure ramp: a fresh hold moves one base step, a long hold moves more. */
  CHECK(SettingsOverlay_HoldStepForTest(sens, 0) == 1);
  CHECK(SettingsOverlay_HoldStepForTest(sens, 4000) > 1);

  /* Drive the tick with an injected clock so acceleration is deterministic:
   * press up, let the initial delay pass, then repeats well past the ramp knee
   * move the value by far more than a tap — and it clamps to the range. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* press up */
  int after_press = g_settings.input_cam_sensitivity;
  CHECK(after_press == 151);  /* one base step up from 150 */
  uint64_t base = SDL_GetTicks();
  for (int i = 1; i <= 40; i++)
    SettingsOverlay_TickAtForTest(base + (uint64_t)i * 60);
  CHECK(g_settings.input_cam_sensitivity > after_press + 5);  /* accelerated */
  CHECK(g_settings.input_cam_sensitivity <= 400);  /* normalized to range */
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, false, false));  /* release */
  CHECK(Settings_SetLong(sens, sens_default) >= kSettingChange_Applied);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The genuine string holdouts still open a text field: pins are arbitrary
   * PAR codes. Confirm B enters editing, and Escape leaves it without a value
   * change — this is the one path numeric rows deliberately no longer use. */
  NavToSection(kSection_Cheats);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("pins");
  CHECK(Settings_Find("pins")->type == kSettingType_Custom);
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));   /* B opens the field */
  CHECK(SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsEditing());
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Controls: the Devices tab holds device/analog rows, and the Keyboard and
   * Gamepad tabs are the two binding pages — the tab now drives
   * input_bind_page, which no longer lists itself as a row. */
  NavToSection(kSection_Controls);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "input_device"));
  CHECK(!Settings_IsMenuVisible(Settings_Find("input_bind_page")));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_key_up"));

  const char *controls_preview = getenv("AR_OVERLAY_CONTROLS_TEST_BMP");
  if (renderer && controls_preview && controls_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, controls_preview));
  }

  /* Drive one full rebind the way a player would: select the row, arm
   * capture, and feed the raw SDL event — the capture path takes scancodes,
   * not keycodes, so it bypasses SettingsOverlay_HandleKey entirely. */
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  /* A gamepad event must not land in a keyboard row. */
  SDL_Event pad = {0};
  pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  pad.gbutton.button = SDL_GAMEPAD_BUTTON_NORTH;
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(SettingsOverlay_IsCapturing());

  SDL_Event key = {0};
  key.type = SDL_EVENT_KEY_DOWN;
  key.key.scancode = SDL_SCANCODE_I;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));

  /* Escape aborts an armed row and leaves the old binding intact. */
  CHECK(SettingsOverlay_HandleKey(SDLK_RETURN, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  key.key.scancode = SDL_SCANCODE_ESCAPE;
  CHECK(SettingsOverlay_HandleCaptureEvent(&key));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_I, false));
  /* Y restores the row default. */
  CHECK(SettingsOverlay_HandleKey(SDLK_A, true, false));
  CHECK(g_settings.input_bind[kInputClass_Keyboard][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_Key, SDL_SCANCODE_UP, false));

  /* Stepping to the Gamepad tab swaps the listed rows to the gamepad set and
   * writes the page setting through, so a reopened menu agrees with it. */
  NavToTab(2);
  CHECK(g_settings.input_bind_page == kInputClass_Gamepad);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "bind_pad_up"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(SettingsOverlay_IsCapturing());
  CHECK(SettingsOverlay_HandleCaptureEvent(&pad));
  CHECK(!SettingsOverlay_IsCapturing());
  CHECK(g_settings.input_bind[kInputClass_Gamepad][kInputAction_Up] ==
        INPUT_BIND_MAKE(kInputBind_PadButton, SDL_GAMEPAD_BUTTON_NORTH,
                        false));
  NavToTab(1);
  CHECK(g_settings.input_bind_page == kInputClass_Keyboard);
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* The Save section's tabs are the Actions page plus five editor pages. The
   * backend/arming controls and apply/export commands now live only on the
   * Actions tab instead of repeating on every page. */
  NavToSection(kSection_Save);
  NavToTab(kSaveEditorPage_Actions);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "save_backend"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_backend == 1);
  CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.save_edit_armed);
  CHECK(Settings_SetText(Settings_Find("save_prog_fillmore"),
                         "act2-cleared") == kSettingChange_Applied);
  const char *save_preview = getenv("AR_OVERLAY_SAVE_TEST_BMP");
  if (renderer && save_preview && save_preview[0]) {
    SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    SDL_RenderPresent(renderer);
    CHECK(SDL_SaveBMP(surface, save_preview));
  }

  /* Selecting a page tab writes the page setting through, and the row list
   * follows it. */
  NavToTab(kSaveEditorPage_Items);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Items);
  RowToKey("save_item_slot_1");
  NavToTab(kSaveEditorPage_Progress);
  CHECK(g_settings.save_editor_page == kSaveEditorPage_Progress);
  /* The export commands live on the Actions tab now; verify the observer path
   * still dispatches the final conversion action from there. */
  NavToTab(kSaveEditorPage_Actions);
  RowToKey("save_export_ini");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("save_export_ini"));
  s_action_calls = 0;
  s_action_desc = NULL;
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* System > Tools carries the debug-settings switch and the host commands
   * (pause, restart, exit); Restart and Exit are the last two rows of this
   * tab, no longer permanent nav-column slots. */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "show_debug_settings"));
  RowToKey("toggle_pause");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 1);
  CHECK(s_action_desc == Settings_Find("toggle_pause"));
  RowToKey("restart_game");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 2);
  CHECK(s_action_desc == Settings_Find("restart_game"));
  RowToKey("exit_desktop");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 3);
  CHECK(s_action_desc == Settings_Find("exit_desktop"));

  /* System > Game holds the QoL gameplay enhancements moved off Tools. */
  NavToTab(1);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "fix_bridge_limit"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.fix_bridge_limit);

  /* Inspector is the section's third tab: its first row makes the enabled
   * state explicit, its second dispatches the complete scene-asset dump, and
   * the remainder is supplied by the read-only live-info provider. */
  NavToTab(2);
  CHECK(!strcmp(SettingsOverlay_SelectedKey(), "scene_inspector"));
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));
  CHECK(g_settings.scene_inspector);
  if (renderer) {
    int calls_before = s_inspector_info_calls;
    SettingsOverlay_Render(
        (SDL_Rect){0, 0, surface_width, surface_height});
    CHECK(s_inspector_info_calls == calls_before + 1);
  }
  RowToKey("dump_scene_assets");
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  CHECK(s_action_calls == 4);
  CHECK(s_action_desc == Settings_Find("dump_scene_assets"));
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));

  /* Debug-settings gate: with the switch on (set at startup) the town dials,
   * their A/B toggles, and the inspector are all visible. */
  const SettingDesc *debug_row = Settings_Find("show_debug_settings");
  CHECK(debug_row && !Settings_IsDebugOnly(debug_row));  /* never hides itself */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  NavToSection(kSection_Town3D);
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 4);  /* Scene, Camera, Light, Weather */
  }

  /* Turn it off through the menu the way a player would (System > Tools). */
  NavToSection(kSection_System);
  NavToTab(0);
  CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
  RowToKey("show_debug_settings");
  CHECK(SettingsOverlay_HandleKey(SDLK_RIGHT, true, false));  /* on -> off */
  CHECK(!g_settings.show_debug_settings);

  /* The dials, internal A/B toggles, and inspector collapse out... */
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_tilt_x_mrad")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_diagnostic_layers")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("sim3d_separated_composite")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("diorama_layer_bg1")));
  CHECK(!Settings_IsMenuVisible(Settings_Find("scene_inspector")));
  /* ...while master toggles, major on/off effects, and camera mode stay. */
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_mode")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_world_navigation")));
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_world_navigation_lighting")));
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_world_navigation_clouds")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_shadows")));
  CHECK(Settings_IsMenuVisible(Settings_Find("sim3d_camera_mode")));
#if AR_SIM3D_TERRAIN_ELEVATION
  CHECK(Settings_IsMenuVisible(
      Settings_Find("sim3d_landscape_height_pct")));
#else
  CHECK(!Settings_IsMenuVisible(
      Settings_Find("sim3d_landscape_height_pct")));
#endif
  CHECK(Settings_IsMenuVisible(Settings_Find("diorama_skybox")));
  /* System drops the all-debug Inspector tab, leaving Tools and Game. */
  {
    int tabs = -1;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(tabs == 2);
  }
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));  /* back to nav column */
  /* Town 3D collapses to Scene + Camera; cycling never lands on a hidden tab. */
  NavToSection(kSection_Town3D);
  {
    int tabs = -1, active = -1;
    CHECK(SettingsOverlay_GetTabState(&active, &tabs));
    CHECK(tabs == 2);
    for (int i = 0; i < 6; i++) {
      CHECK(SettingsOverlay_HandleKey(SDLK_RIGHTBRACKET, true, false));
      CHECK(SettingsOverlay_GetTabState(&active, NULL));
      CHECK(active >= 0 && active < 2);
    }
  }
  /* Restore for the section-sweep contact sheet below. */
  g_settings.show_debug_settings = true;

  /* Every section stays reachable and its nav row stays inside the scroll
   * window, whatever the panel can fit. With AR_OVERLAY_PREVIEW_DIR set this
   * doubles as a contact sheet: one BMP per (section, tab), which is the only
   * practical way to eyeball a layout change across the whole menu. */
  const char *preview_dir = getenv("AR_OVERLAY_PREVIEW_DIR");
  /* Give the contact sheet a POPULATED layer editor. Without the hooks installed
   * every level tab renders its "enter a stage here" notice, which is the one
   * state that needs no review -- the layout worth eyeballing is a room with
   * planes, one of them expanded into its parameters. Fillmore act 2 with the
   * shipped rake on its water is the case the whole feature exists for. */
  if (preview_dir && preview_dir[0]) {
    SettingsOverlay_SetLayerEditorHooks(FakeLayerTable, FakeLayerRoom,
                                        FakeLayerSave);
    memset(&s_fake_layer_table, 0, sizeof(s_fake_layer_table));
    s_fake_room_live = true;
    DioramaRoomOverride *preview_room = DioramaLayerOrder_FindOrAdd(
        &s_fake_layer_table, s_fake_group, s_fake_map);
    if (preview_room) {
      DioramaLayerEditor_SetStrategy(&preview_room->planes[kDioramaPlane_Bg2Hi],
                                     kDioramaDepth_Stack);
      DioramaLayerEditor_SetStrategy(&preview_room->planes[kPpuOverlaySource_Bg1],
                                     kDioramaDepth_Rake);
    }
  }
  for (int section = 0; section < kDebugSectionCount; section++) {
    NavToSection(section);
    if (!renderer) continue;
    SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
    int selected = -1, top = -1, visible = -1, total = -1;
    CHECK(SettingsOverlay_GetNavigationState(
        &selected, &top, &visible, &total));
    CHECK(selected == section);
    CHECK(total == kDebugSectionCount);
    CHECK(selected >= top && selected < top + visible);
    if (!preview_dir || !preview_dir[0]) continue;

    int tabs = 0;
    CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
    CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
    for (int tab = 0; tab < tabs; tab++) {
      NavToTab(tab);
      /* Park the cursor on the authored water plane so its parameter block is
       * expanded in the shot -- the expansion is the layout decision most worth
       * reviewing, and it is only visible on the selected plane. */
      if (section == kSection_Layers &&
          strcmp(SettingsOverlay_SelectedKey(), "") != 0) {
        for (int guard = 0; guard < 32; guard++) {
          if (!strcmp(SettingsOverlay_SelectedKey(), "bg2hi")) break;
          CHECK(SettingsOverlay_HandleKey(SDLK_DOWN, true, false));
        }
        /* B expands rather than edits, so the shot shows the parameters without
         * changing the shape the room was seeded with. */
        CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      }
      char path[256];
      snprintf(path, sizeof(path), "%s/section%d-tab%d.bmp",
               preview_dir, section, tab);
      SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
      SDL_RenderClear(renderer);
      SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
      SDL_RenderPresent(renderer);
      CHECK(SDL_SaveBMP(surface, path));
    }
    CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  }

  /* Second contact sheet with debug settings OFF, so a layout review can see
   * the collapsed menu players actually get (Town 3D without Light/Weather,
   * System without Inspector, the dial rows gone). */
  if (renderer && preview_dir && preview_dir[0]) {
    g_settings.show_debug_settings = false;
    for (int section = 0; section < kPlayerSectionCount; section++) {
      NavToSection(section);
      int tabs = 0;
      CHECK(SettingsOverlay_GetTabState(NULL, &tabs));
      CHECK(SettingsOverlay_HandleKey(SDLK_Z, true, false));
      for (int tab = 0; tab < tabs; tab++) {
        NavToTab(tab);
        char path[256];
        snprintf(path, sizeof(path), "%s/section%d-tab%d-dbgoff.bmp",
                 preview_dir, section, tab);
        SDL_SetRenderDrawColor(renderer, 32, 24, 16, 255);
        SDL_RenderClear(renderer);
        SettingsOverlay_Render((SDL_Rect){0, 0, surface_width, surface_height});
        SDL_RenderPresent(renderer);
        CHECK(SDL_SaveBMP(surface, path));
      }
      CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
    }
    g_settings.show_debug_settings = true;
  }

  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_X, true, false));
  CHECK(!SettingsOverlay_IsOpen());
  SettingsOverlay_Open();
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, true));
  CHECK(SettingsOverlay_IsOpen());
  CHECK(SettingsOverlay_HandleKey(SDLK_ESCAPE, true, false));
  CHECK(!SettingsOverlay_IsOpen());

  SettingsOverlay_Open();
  CheckLayerEditorSection();
  SettingsOverlay_Close();

  /* Debug panels avoid the inspected point and can be moved without a click
   * falling through to the tool beneath them. */
  if (renderer) {
    SDL_SetRenderDrawColor(renderer, 22, 28, 34, 255);
    SDL_RenderClear(renderer);
    SettingsOverlay_RenderDebugPanel(
        "SCENE INSPECTOR",
        "CLICK 474,170  WORLD $008E,$00C6\n"
        "GF $016C STATE $00/$00 CAM $0080,$0080 MAP $0000,$0000\n"
        "PPU MODE 7 BRIGHT 15 MAIN $01 SUB $00 MARGIN 0/0\n"
        "BG1 T$03A P1 PAL3 PIX2 CENTER MAP$7104\n"
        "OBJ#12 16X16 BASE$80 SUB$91 PAL4 PRI2 PIX7\n"
        "CANDIDATES; WINDOWS/COLOR MATH MAY MASK A LAYER\n"
        "LEFT CLICK INSPECT  RIGHT CLICK CLEAR  F3 DISABLE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_RenderPresent(renderer);
    const char *debug_preview = getenv("AR_OVERLAY_DEBUG_TEST_BMP");
    if (debug_preview && debug_preview[0])
      CHECK(SDL_SaveBMP(surface, debug_preview));
    SDL_Rect panel_before = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_before));
    CHECK(panel_before.y < surface_height / 2);
    CHECK(panel_before.w < surface_width - 40);
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + panel_before.h - 4));
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_before.x + 4, panel_before.y + 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_before.x + 4, surface_height / 2);
    SettingsOverlay_EndDebugPanelDrag();
    CHECK(!SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_Rect panel_after = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_after));
    CHECK(panel_after.y != panel_before.y);
    CHECK(SettingsOverlay_BeginDebugPanelDrag(
        panel_after.x + panel_after.w - 4,
        panel_after.y + panel_after.h - 4));
    CHECK(SettingsOverlay_IsDebugPanelDragging());
    SettingsOverlay_DragDebugPanel(
        panel_after.x + panel_after.w - 4 - panel_after.w / 4,
        panel_after.y + panel_after.h - 4 - panel_after.h / 4);
    SettingsOverlay_EndDebugPanelDrag();
    SettingsOverlay_RenderDebugPanel(
        "DEBUG", "FIRST LINE\nSECOND LINE",
        (SDL_Point){ surface_width / 2, surface_height - 1 });
    SDL_Rect panel_resized = {0};
    CHECK(SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(panel_resized.w < panel_after.w);
    CHECK(panel_resized.h < panel_after.h);
    SettingsOverlay_HideDebugPanel();
    CHECK(!SettingsOverlay_GetDebugPanelRect(&panel_resized));
    CHECK(!SettingsOverlay_BeginDebugPanelDrag(0, 0));
  }

  SettingsOverlay_SetManualHooks(NULL);
  SettingsOverlay_Destroy();
  Settings_SetActionObserver(NULL);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(surface);
  SDL_Quit();
  remove(settings_path);
  remove(settings_temporary);

  if (s_failures) {
    fprintf(stderr, "settings overlay tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "settings overlay tests: pass\n");
  return 0;
}
