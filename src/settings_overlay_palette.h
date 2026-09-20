#ifndef SETTINGS_OVERLAY_PALETTE_H
#define SETTINGS_OVERLAY_PALETTE_H
#include "settings_overlay_internal.h"
#include "settings_overlay.h"
#include "diorama/diorama_layer_editor.h"

/* The modal owns its captured palette, selection and texture. The layer
 * editor remains responsible for committing or resetting the selected row. */
typedef void (*SettingsOverlayPaletteCommit)(const DioramaEditorRow *row, bool reset,
                                             uint8_t index);
void SettingsOverlayPalette_Open(const DioramaEditorRow *row,
                                 const uint16_t colors[kSettingsOverlayLayerPaletteEntries],
                                 SettingsOverlayPaletteCommit commit);
bool SettingsOverlayPalette_ApplyNav(MenuNav nav, bool repeat);
void SettingsOverlayPalette_Draw(const MenuLayout *layout);
void SettingsOverlayPalette_Close(void);
void SettingsOverlayPalette_ReleaseTexture(void);
#endif
