#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

/* Host-owned settings overlay. It consumes SDL input before the SNES joypad
 * path and renders after the emulated framebuffer has been composited. */
bool SettingsOverlay_Init(SDL_Renderer *renderer,
                          const uint8_t *rom_data, size_t rom_size);
void SettingsOverlay_Destroy(void);

bool SettingsOverlay_IsOpen(void);
void SettingsOverlay_Open(void);
void SettingsOverlay_Close(void);

/* Returns true when the event belongs to the overlay and must not reach the
 * host hotkey/SNES input paths. F2 is deliberately left available so visual
 * snapshots can include the menu. */
bool SettingsOverlay_HandleKey(SDL_Keycode key, bool pressed, bool repeat);
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
