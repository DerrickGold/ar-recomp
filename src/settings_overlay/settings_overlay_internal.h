#ifndef AR_SETTINGS_OVERLAY_INTERNAL_H
#define AR_SETTINGS_OVERLAY_INTERNAL_H

/* Private UI vocabulary shared by the menu, diagnostic panel and palette
 * picker. settings_overlay.h is the public API. The menu owns navigation and
 * draw primitives; settings_overlay_artwork.c owns fonts and atlas lifetimes. */

#include <stdint.h>
#include <SDL3/SDL.h>

#include "render/render_device.h"
#include "settings_overlay/menu_input.h"
#include "settings_overlay/settings_overlay_artwork.h"

/* ARGB pixel packing for overlay artwork and UI colors. */
#define ARGB(a, r, g, b) \
  ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | \
   (uint32_t)(g) << 8 | (uint32_t)(b))

enum {
  kDebugGlyphWidth = 6,
  kDebugGlyphHeight = 8,
  kDebugLineHeight = 10,
  kGlyphSize = 8,
  kSmallLineHeight = 9,
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

/* Modal UI input activity, with the disconnected-pad keyboard fallback. */
InputClass SettingsOverlay_MenuInputDevice(void);

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

/* Shared menu/modal drawing vocabulary. */
extern const uint32_t kSteelBlue, kSelectYellow, kGameGold, kMutedText;
ArRenderRectF ToRenderRect(ArRenderRectI rect);
void DrawSmallTextN(const MenuLayout *layout, int x, int y,
                    const char *text, int length, uint32_t color);
void DrawSmallText(const MenuLayout *layout, int x, int y,
                   const char *text, uint32_t color);

/* Family-local widgets consume values, never the shell's navigation state. */
extern const uint32_t kPanel, kHighlight, kSteelDim;
int CappedTextLength(const char *text, int max_chars);
int SmallTextWidth(const char *text);
void DrawTextN(const MenuLayout *layout, int x, int y,
               const char *text, int max_chars, TextStyle style);
void DrawOverlayIcon(const MenuLayout *layout, int x, int y, int size,
                     SettingsOverlayIcon icon, bool selected, int alpha);
void DrawScrollBar(const MenuLayout *layout, int x, int y, int height,
                   int total, int visible, int top, uint32_t accent);
int DrawWrappedSmallText(const MenuLayout *layout, int x, int y,
                          const char *text, int max_chars, int max_lines, uint32_t color);
void DrawSmallTextPreview(const MenuLayout *layout, int x, int y,
                          const char *text, int columns, int lines, uint32_t color);
enum { kMenuHintMax = 7 };
typedef struct MenuHints {
  int count;
  struct { const char *key, *label; } items[kMenuHintMax];
} MenuHints;

void AddMenuHint(MenuHints *hints, const char *key, const char *label);
void DrawMenuHints(const MenuLayout *layout, int x, int y, int width,
                   const MenuHints *hints, uint32_t key_color, uint32_t label_color);

typedef struct SettingsOverlayDetailsText {
  char title[256], body[16384];
} SettingsOverlayDetailsText;
void SettingsOverlay_ShowDetails(const SettingsOverlayDetailsText *text);
void SettingsOverlay_CloseDetails(void);
void SettingsOverlay_SetStatus(const char *text);

/* Defined in settings_overlay_debug_panel.c. Clears all panel state; called by
 * SettingsOverlay_Destroy so teardown owns no panel internals directly. */
void SettingsOverlayDebugPanel_Reset(void);

#endif  /* AR_SETTINGS_OVERLAY_INTERNAL_H */
