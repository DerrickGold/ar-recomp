#ifndef SETTINGS_OVERLAY_INTERNAL_H
#define SETTINGS_OVERLAY_INTERNAL_H

/* Internal contract between settings_overlay.c (the menu core) and
 * settings_overlay_debug_panel.c (the draggable F-key diagnostic panel). NOT a
 * public API — settings_overlay.h is the public one. The panel is a distinct
 * feature that shares the overlay's renderer, ROM/debug fonts, layout math and
 * low-level draw primitives, so those few internals are declared here rather
 * than duplicated. Everything here stays owned by settings_overlay.c. */

#include <stdint.h>
#include <SDL3/SDL.h>

#include "render/render_device.h"

/* ARGB pixel packing, shared by both translation units' colour constants. */
#define ARGB(a, r, g, b) \
  ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
   (uint32_t)(g) << 8 | (uint32_t)(b))

enum {
  kDebugGlyphWidth = 6,
  kDebugGlyphHeight = 8,
  kDebugLineHeight = 10,
  kGlyphSize = 8,
};

/* Debug/inspector text roles, mapped to kDebugTextColors in settings_overlay.c.
 * Shared because DrawDebugTextN (core) takes one and the panel passes them. */
typedef enum DebugTextStyle {
  kDebugText_Normal,
  kDebugText_Label,
  kDebugText_Value,
  kDebugText_Target,
  kDebugText_Warning,
  kDebugText_Dim,
  kDebugTextStyle_Count,
} DebugTextStyle;

/* Resolved per-frame geometry for one overlay draw pass. */
typedef struct MenuLayout {
  int output_width;
  int output_height;
  int scale_percent;
  int logical_width;
  int logical_height;
  int origin_x;
  int origin_y;
} MenuLayout;

/* Presentation resources owned by settings_overlay.c (created in
 * SettingsOverlay_Init). The panel reads these; it does not create or free
 * them. */
extern ArRenderDevice *s_render_device;
extern ArRenderTexture s_debug_font_texture;

/* Layout + draw primitives defined in settings_overlay.c, reused by the panel
 * so both surfaces scale and render text identically. */
MenuLayout BuildLayout(int output_width, int output_height);
MenuLayout BuildLayoutAtScale(int output_width, int output_height, int scale);
int SnappedFitScale(int output_width, int output_height);
ArRenderRectI LogicalRect(const MenuLayout *layout,
                          int x, int y, int width, int height);
void FillLogicalRect(const MenuLayout *layout,
                     int x, int y, int width, int height, uint32_t color);
void DrawDialogPanel(const MenuLayout *layout,
                     int x, int y, int width, int height);
void DrawDebugTextN(const MenuLayout *layout, int x, int y,
                    const char *text, int length, DebugTextStyle style);
void DrawDebugHighlightedLine(const MenuLayout *layout,
                              int x, int y, const char *text, int length);

/* Defined in settings_overlay_debug_panel.c. Clears all panel state; called by
 * SettingsOverlay_Destroy so teardown owns no panel internals directly. */
void SettingsOverlayDebugPanel_Reset(void);

#endif  /* SETTINGS_OVERLAY_INTERNAL_H */
