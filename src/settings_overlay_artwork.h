#ifndef SETTINGS_OVERLAY_ARTWORK_H
#define SETTINGS_OVERLAY_ARTWORK_H

#include "render/render_device.h"

/* Internal overlay resources. Callers borrow handles until reload/destroy;
 * CPU glyph data survives a renderer reset. */
typedef enum TextStyle {
  kText_Normal,
  kText_Dim,
  kText_Warning,
  kText_Value,
  kTextStyle_Count,
} TextStyle;

typedef enum SettingsOverlayIcon {
  kOverlayIcon_Video,
  kOverlayIcon_Action,
  kOverlayIcon_Town,
  kOverlayIcon_Audio,
  kOverlayIcon_Controls,
  kOverlayIcon_Cheats,
  kOverlayIcon_Save,
  kOverlayIcon_Manual,
  kOverlayIcon_System,
  kOverlayIcon_Localization,
  kOverlayIcon_Randomizer,
  kOverlayIcon_Layers,
  kOverlayIcon_Count,
} SettingsOverlayIcon;
enum { kIconSize = 16 };

typedef struct SettingsOverlayArtwork {
  ArRenderTexture fonts[kTextStyle_Count], debug_font, icons, dialog_frame;
  bool glyph_defined[256];
} SettingsOverlayArtwork;

extern const uint32_t kTextPalettes[kTextStyle_Count][4];
const SettingsOverlayArtwork *SettingsOverlayArtwork_Get(void);
bool SettingsOverlayArtwork_HasDebugGlyph(unsigned ch);
bool SettingsOverlayArtwork_Init(ArRenderDevice *device, const uint8_t *rom, size_t size);
bool SettingsOverlayArtwork_Reload(const uint8_t *rom, size_t size);
void SettingsOverlayArtwork_Destroy(void);
ArRenderTexture SettingsOverlayArtwork_CreateAtlas(ArRenderDevice *device, int width, int height,
                                                   const uint32_t *pixels);

#endif
