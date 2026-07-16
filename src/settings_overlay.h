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
/* game_viewport is used only to resolve the HUD's "Match game" scale when
 * editing that row. The settings presentation itself covers the complete
 * renderer output and follows the window aspect ratio. */
void SettingsOverlay_Render(SDL_Rect game_viewport);
