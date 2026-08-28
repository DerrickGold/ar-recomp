#ifndef AR_SETTINGS_OVERLAY_RENDER_H
#define AR_SETTINGS_OVERLAY_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "render/render_types.h"

/* Portable drawing surface shared by terminal host UI, comparison views, and
 * the manual. Window ownership and input events remain in settings_overlay.h.
 *
 * The overlay owns font atlases built from the ROM's dialog glyphs. All
 * coordinates and sizes below are physical renderer-output pixels, independent
 * of the menu's own scaled logical layout. */
enum { kSettingsOverlayGlyphSize = 8 };

/* Output-pixel width for centering or framing a text run. */
int SettingsOverlay_GameTextWidth(const char *text, int scale);
/* No-op before the atlas exists. `scale` multiplies the 8x8 glyph and `alpha`
 * fades the complete run. */
void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha,
                                  const char *text);
/* Draw the ROM dialog frame. Dimensions must be divisible by 8*scale. */
bool SettingsOverlay_DrawGameFrame(ArRenderRectI rect, int scale);

/* `game_viewport` is used only to resolve the HUD's "Match game" scale. The
 * menu itself covers the complete render output. */
void SettingsOverlay_Render(ArRenderRectI game_viewport);

/* Compact report panel for host debug tools. `text` may contain newlines; the
 * initial placement chooses the output half opposite `avoid_point`. */
void SettingsOverlay_RenderDebugPanel(const char *title, const char *text,
                                      ArRenderPointI avoid_point);
void SettingsOverlay_HideDebugPanel(void);

#endif /* AR_SETTINGS_OVERLAY_RENDER_H */
