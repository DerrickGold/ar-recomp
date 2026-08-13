#ifndef SETTINGS_OVERLAY_H
#define SETTINGS_OVERLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

/* Host-owned settings overlay. It consumes SDL input before the SNES joypad
 * path and renders after the emulated framebuffer has been composited. */
bool SettingsOverlay_Init(SDL_Renderer *renderer,
                          const uint8_t *rom_data, size_t rom_size);
void SettingsOverlay_Destroy(void);

/* RENDER_TARGETS_RESET/DEVICE_RESET recovery: the overlay's atlases are
 * SDL_TEXTUREACCESS_STATIC (uploaded once at Init) so the driver empties them
 * on a reset. Rebuilds them from the persisted decoded font tiles + the ROM
 * dialog assets; UI/navigation state is untouched. No-op without a renderer. */
bool SettingsOverlay_ReloadTextures(const uint8_t *rom_data, size_t rom_size);

/* Optional live, read-only text shown below the Inspector controls. The
 * provider is called only while that menu is rendered and may emit newlines. */
typedef void (*SettingsOverlayInspectorInfoProvider)(char *buffer,
                                                     size_t buffer_size);
void SettingsOverlay_SetInspectorInfoProvider(
    SettingsOverlayInspectorInfoProvider provider);

/* Layer-editor hooks (System-adjacent "Layers" section, developer-only).
 *
 * The editor mutates diorama.c's per-room override table and writes the manifest
 * beside settings.ini. Both are injected rather than called directly because
 * this file's test target links five sources and NOT diorama.c -- reaching into
 * it here would drag the PPU and the SDL render path into the test. Same
 * reasoning as the inspector provider above.
 *
 *   table  the override table to edit, or NULL when there is none (the section
 *          then reports that instead of editing a table it does not have).
 *   room   the live room: false when no diorama room is active, so the editor
 *          says so rather than showing stale planes.
 *   save   persist the manifest; called on an edit, not every frame.
 */
struct DioramaLayerOrderTable;
typedef struct DioramaLayerOrderTable *(*SettingsOverlayLayerTableFn)(void);
typedef bool (*SettingsOverlayLayerRoomFn)(uint8_t *out_group,
                                           uint8_t *out_map,
                                           uint8_t *out_section);
typedef bool (*SettingsOverlayLayerSaveFn)(void);
void SettingsOverlay_SetLayerEditorHooks(SettingsOverlayLayerTableFn table,
                                         SettingsOverlayLayerRoomFn room,
                                         SettingsOverlayLayerSaveFn save);

/* The in-game manual, injected for the same reason the layer editor is: the
 * reader owns textures and an image decoder, and calling it directly from here
 * would drag both into every target that links this file -- including
 * tests/settings_overlay_test.c, which has no renderer at all.
 *
 * The reader is a MODE THIS OVERLAY IS IN, not a peer of it. While it is open
 * SettingsOverlay_IsOpen() stays true, so nothing else in the host has to learn
 * about a third state: the ~18 places that ask whether the menu has the game
 * suspended keep getting one answer. Unhooked, every call below is inert and the
 * overlay behaves exactly as it did before the manual existed. */
typedef struct SettingsOverlayManualHooks {
  /* Whether this build has a readable manual input. The entire Manual section
   * is omitted when false; a dead action row is not a useful fallback. */
  bool (*available)(void);
  bool (*is_open)(void);
  void (*close)(void);
  void (*render)(SDL_Rect viewport);
  /* Return true to consume. The reader MUST decline the keys that close the
   * menu outright, so a reader that fails to draw can never trap the player. */
  bool (*handle_key)(SDL_Keycode key, bool pressed, bool repeat);
  bool (*handle_pad)(const SDL_Event *event);
} SettingsOverlayManualHooks;
void SettingsOverlay_SetManualHooks(const SettingsOverlayManualHooks *hooks);

/* ── The game's menu font, for a nested mode to draw with ──────────────────
 *
 * The overlay owns the font atlases: they are built from the ROM's own dialog
 * glyphs, in the game's own menu colors. A nested mode that drew its text with
 * SDL_RenderDebugText instead would be an 8-pixel developer font sitting inside
 * a menu rendered from the game's -- which is what the manual reader did, and it
 * was illegible on a large window.
 *
 * Coordinates and sizes are in RENDERER OUTPUT PIXELS, not the overlay's own
 * scaled logical space, because a fullscreen mode is not laid out on the menu's
 * grid and should not have to pretend it is. `scale` multiplies the 8x8 glyph;
 * `alpha` fades the whole run and is what lets a hint line come and go without
 * the caller reaching into the atlas. */
enum { kSettingsOverlayGlyphSize = 8 };

/* Output-pixel width a run would occupy. Lets a caller centre or box it without
 * duplicating the glyph advance. */
int SettingsOverlay_GameTextWidth(const char *text, int scale);

/* Draw at output pixels. No-op before the atlases exist, so a caller does not
 * have to know the overlay's initialisation order. */
void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha,
                                  const char *text);

bool SettingsOverlay_IsOpen(void);
void SettingsOverlay_Open(void);
void SettingsOverlay_Close(void);

/* Read-only layout diagnostics used by preview/regression tests. Ordinals
 * count populated primary-navigation rows, including Restart and Exit. */
/* Key of the currently selected row, or "" when the overlay is closed. Lets a
 * test navigate to a row by name instead of counting keypresses, which breaks
 * every time a row is inserted above it. */
const char *SettingsOverlay_SelectedKey(void);

bool SettingsOverlay_GetNavigationState(int *selected_ordinal,
                                        int *top_ordinal,
                                        int *visible_rows,
                                        int *total_rows);

/* Which tab of the selected section is active, and how many that section has.
 * Tests step tabs with the normal key path and use this to know when they have
 * arrived, rather than assuming a section's tab count. */
bool SettingsOverlay_GetTabState(int *active_tab, int *tab_count);

/* Advances hold-to-accelerate value stepping. main.c calls this once per frame
 * before rendering while the overlay is open, so the render pass sees a stable
 * navigation state. */
void SettingsOverlay_Tick(void);

/* Test seam: the pure hold-acceleration curve — base steps to move for a row
 * that has been held `held_ms`. Exposed so the ramp can be checked without
 * driving real wall-clock time. */
struct SettingDesc;
long SettingsOverlay_HoldStepForTest(const struct SettingDesc *desc,
                                     uint64_t held_ms);
/* Drives the hold tick with an injected clock so a test can cross the ramp
 * thresholds deterministically without sleeping. */
void SettingsOverlay_TickAtForTest(uint64_t now_ms);

/* Returns true when the event belongs to the overlay and must not reach the
 * host hotkey/SNES input paths. F2 is deliberately left available so visual
 * snapshots can include the menu. */
bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat);

/* Gamepad path. Navigation follows the player's OWN gamepad bindings (menu
 * confirm is whatever they bound to SNES B, and so on), so a Steam Deck can
 * drive the whole menu — including rebinding — with no keyboard attached.
 * Returns true when the overlay owned the event. */
bool SettingsOverlay_HandleGamepadEvent(const SDL_Event *event);

/* A binding row is armed and waiting for the next physical input. main.c must
 * offer raw events here BEFORE its own hotkey chain while this is true, or a
 * key like F9 would run its hotkey instead of being bound. */
/* True while a row's text-entry field is active. Numeric rows never enter this
 * state (they step); it is reached only by the Mask/Custom string holdouts. */
bool SettingsOverlay_IsEditing(void);
bool SettingsOverlay_IsCapturing(void);
bool SettingsOverlay_HandleCaptureEvent(const SDL_Event *event);
/* Text events are accepted only while a descriptor is in direct-edit mode. */
bool SettingsOverlay_HandleText(const char *text);
/* game_viewport is used only to resolve the HUD's "Match game" scale when
 * editing that row. The settings presentation itself covers the complete
 * renderer output and follows the window aspect ratio. */
void SettingsOverlay_Render(SDL_Rect game_viewport);

/* Compact, color-coded monospace panel used by read-only host debug tools
 * while the settings menu itself is closed. `text` may contain newlines. The
 * panel is initially placed on the half of the output opposite `avoid_point`.
 * Its frame remains the native ActRaiser dialog frame. The title strip moves
 * it; the lower-right grip uniformly rescales it. */
void SettingsOverlay_RenderDebugPanel(const char *title, const char *text,
                                      SDL_Point avoid_point);
void SettingsOverlay_HideDebugPanel(void);
/* These drag functions handle both title movement and corner rescaling. */
bool SettingsOverlay_BeginDebugPanelDrag(int output_x, int output_y);
void SettingsOverlay_DragDebugPanel(int output_x, int output_y);
void SettingsOverlay_EndDebugPanelDrag(void);
bool SettingsOverlay_IsDebugPanelDragging(void);
bool SettingsOverlay_GetDebugPanelRect(SDL_Rect *rect);

#endif  /* SETTINGS_OVERLAY_H */
