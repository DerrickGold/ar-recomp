/* World geometry fixtures do not initialize the host font service. */
#include "settings_overlay/settings_overlay_render.h"
#include <string.h>
bool SettingsOverlay_DrawEnhancedLabel(ArRenderRectI bounds, uint8_t alpha, const char *text) {
  (void)bounds;
  (void)alpha;
  (void)text;
  return false;
}
int SettingsOverlay_GameTextWidth(const char *text, int scale) {
  return (int)strlen(text) * 8 * scale;
}
void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha, const char *text) {
  (void)x;
  (void)y;
  (void)scale;
  (void)alpha;
  (void)text;
}
