#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

/* Host-owned settings overlay. It consumes SDL input before the SNES joypad
 * path and renders after the emulated framebuffer has been composited. */
bool SettingsOverlay_Init(SDL_Renderer *renderer,
                          const uint8_t *rom_data, size_t rom_size);
void SettingsOverlay_Destroy(void);

/* Optional live, read-only text shown below the Inspector controls. The
 * provider is called only while that menu is rendered and may emit newlines. */
typedef void (*SettingsOverlayInspectorInfoProvider)(char *buffer,
                                                     size_t buffer_size);
void SettingsOverlay_SetInspectorInfoProvider(
    SettingsOverlayInspectorInfoProvider provider);

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
 * on the main thread while the overlay is open (never the present thread — a
 * settings write there would deadlock against the present-thread quiesce). */
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
