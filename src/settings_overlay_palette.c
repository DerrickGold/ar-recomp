#include "settings_overlay_palette.h"
#include "settings_overlay.h"
#include "settings_overlay_artwork.h"
#include "localization/ui_catalog.h"
#include "snes_bgr555.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_layer_palette_open;
static uint8_t s_layer_palette_cursor;
static uint16_t s_layer_palette[kSettingsOverlayLayerPaletteEntries];
static DioramaEditorRow s_layer_palette_row;
static ArRenderTexture s_layer_palette_texture;

enum {
  kLayerPaletteCell = 9,
  kLayerPaletteGridPixels = 16 * kLayerPaletteCell,
};

static SettingsOverlayPaletteCommit s_commit;
static bool RebuildLayerPaletteTexture(void);
static const char *Ui(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}

void SettingsOverlayPalette_Close(void) { s_layer_palette_open = false; }

void SettingsOverlayPalette_ReleaseTexture(void) {
  ArRenderDevice_DestroyTexture(s_render_device, s_layer_palette_texture);
  s_layer_palette_texture = ArRenderTexture_Invalid();
}

void SettingsOverlayPalette_Open(const DioramaEditorRow *row,
                                 const uint16_t colors[kSettingsOverlayLayerPaletteEntries],
                                 SettingsOverlayPaletteCommit commit) {
  s_commit = commit;
  s_layer_palette_row = *row;
  memcpy(s_layer_palette, colors, sizeof(s_layer_palette));
  s_layer_palette_cursor =
      row->effective_transparent_fill_set &&
              row->effective_transparent_fill_kind == kDioramaTransparentFill_Cgram
          ? row->effective_transparent_fill_cgram
          : 0;
  /* Snapshot CGRAM at modal open; moving the cursor never resamples the game. */
  SettingsOverlayPalette_ReleaseTexture();
  if (s_render_device) (void)RebuildLayerPaletteTexture();
  s_layer_palette_open = true;
}

bool SettingsOverlayPalette_ApplyNav(MenuNav nav, bool repeat) {
  if (!s_layer_palette_open) return false;
  switch (nav) {
    case kMenuNav_Up:
      s_layer_palette_cursor = (uint8_t)(s_layer_palette_cursor - 16);
      break;
    case kMenuNav_Down:
      s_layer_palette_cursor = (uint8_t)(s_layer_palette_cursor + 16);
      break;
    case kMenuNav_Left:
      s_layer_palette_cursor =
          (uint8_t)((s_layer_palette_cursor & 0xf0) | ((s_layer_palette_cursor - 1) & 0x0f));
      break;
    case kMenuNav_Right:
      s_layer_palette_cursor =
          (uint8_t)((s_layer_palette_cursor & 0xf0) | ((s_layer_palette_cursor + 1) & 0x0f));
      break;
    case kMenuNav_Confirm:
    case kMenuNav_Reset:
      if (!repeat) {
        s_commit(&s_layer_palette_row, nav == kMenuNav_Reset, s_layer_palette_cursor);
        s_layer_palette_open = false;
      }
      break;
    case kMenuNav_Back:
      if (!repeat) s_layer_palette_open = false;
      break;
    case kMenuNav_Close:
      if (!repeat) SettingsOverlay_Close();
      break;
    case kMenuNav_TabPrev:
    case kMenuNav_TabNext:
      break;
  }
  return true;
}

static uint32_t LayerPaletteColor(uint16_t bgr555) {
  return ARGB(255, ExpandColor5(bgr555, 15), ExpandColor5(bgr555 >> 5, 15),
              ExpandColor5(bgr555 >> 10, 15));
}

static bool RebuildLayerPaletteTexture(void) {
  if (!s_render_device) return false;
  uint32_t *pixels =
      calloc((size_t)kLayerPaletteGridPixels * kLayerPaletteGridPixels, sizeof(*pixels));
  if (!pixels) return false;
  for (int index = 0; index < kSettingsOverlayLayerPaletteEntries; index++) {
    const int x0 = (index & 15) * kLayerPaletteCell;
    const int y0 = (index >> 4) * kLayerPaletteCell;
    const uint32_t color = LayerPaletteColor(s_layer_palette[index]);
    for (int y = 0; y < kLayerPaletteCell - 1; y++)
      for (int x = 0; x < kLayerPaletteCell - 1; x++)
        pixels[(size_t)(y0 + y) * kLayerPaletteGridPixels + x0 + x] = color;
  }

  const ArRenderTexture texture = SettingsOverlayArtwork_CreateAtlas(
      s_render_device, kLayerPaletteGridPixels, kLayerPaletteGridPixels, pixels);
  const bool ready = ArRenderTexture_IsValid(texture);
  free(pixels);
  if (!ready) {
    return false;
  }
  s_layer_palette_texture = texture;
  return true;
}

void SettingsOverlayPalette_Draw(const MenuLayout *layout) {
  if (!s_layer_palette_open) return;
  enum {
    kPickerWidth = 172,
    kPickerHeight = 190,
    kPickerGridX = 14,
    kPickerGridY = 25,
  };
  const int x = (layout->logical_width - kPickerWidth) / 2;
  const int y = (layout->logical_height - kPickerHeight) / 2;
  DrawDialogPanel(layout, x, y, kPickerWidth, kPickerHeight);
  DrawSmallTextN(layout, x + 14, y + 11, Ui("overlay.palette.title"),
                 (kPickerWidth - 28) / kDebugGlyphWidth, kSteelBlue);

  if (!ArRenderTexture_IsValid(s_layer_palette_texture)) (void)RebuildLayerPaletteTexture();
  if (ArRenderTexture_IsValid(s_layer_palette_texture)) {
    const ArRenderRectF destination =
        ToRenderRect(LogicalRect(layout, x + kPickerGridX, y + kPickerGridY,
                                 kLayerPaletteGridPixels, kLayerPaletteGridPixels));
    (void)ArRenderDevice_DrawTexture(s_render_device, s_layer_palette_texture, NULL, &destination);
  } else {
    /* Texture creation failure should not make the editor unusable. This slow
     * fallback is exceptional; the normal path submits the entire grid once. */
    for (int index = 0; index < kSettingsOverlayLayerPaletteEntries; index++) {
      FillLogicalRect(layout, x + kPickerGridX + (index & 15) * kLayerPaletteCell,
                      y + kPickerGridY + (index >> 4) * kLayerPaletteCell, kLayerPaletteCell - 1,
                      kLayerPaletteCell - 1, LayerPaletteColor(s_layer_palette[index]));
    }
  }

  const int selected_x = x + kPickerGridX + (s_layer_palette_cursor & 15) * kLayerPaletteCell;
  const int selected_y = y + kPickerGridY + (s_layer_palette_cursor >> 4) * kLayerPaletteCell;
  FillLogicalRect(layout, selected_x, selected_y, kLayerPaletteCell, 1, kSelectYellow);
  FillLogicalRect(layout, selected_x, selected_y + kLayerPaletteCell - 1, kLayerPaletteCell, 1,
                  kSelectYellow);
  FillLogicalRect(layout, selected_x, selected_y, 1, kLayerPaletteCell, kSelectYellow);
  FillLogicalRect(layout, selected_x + kLayerPaletteCell - 1, selected_y, 1, kLayerPaletteCell,
                  kSelectYellow);

  char selected[32];
  snprintf(selected, sizeof(selected), "CGRAM $%02X", (unsigned)s_layer_palette_cursor);
  DrawSmallText(layout, x + 14, y + 173, selected, kGameGold);
  DrawSmallTextN(layout, x + 76, y + 173, Ui("overlay.palette.hint"),
                 (kPickerWidth - 90) / kDebugGlyphWidth, kMutedText);
}
