#include "manual_caption.h"

#include <stdio.h>
#include <string.h>
#include "localization/interface_text.h"

bool ManualCaption_Build(ArUiLocale locale, ManualHintDevice device, bool zoomed,
    bool spread, int page, int total, int width, int height, int cell_pixels,
    ManualCaption *out) {
  if (!out || page < 1 || total < page || width < 1 || height < 1 ||
      cell_pixels < 1 || cell_pixels > 64) return false;
  ManualCaption result = {0};
  char key[64], page_text[16], total_text[16];
  snprintf(key, sizeof(key), "overlay.manual.controls.%d.%d",
           device == kManualHintDevice_Gamepad, zoomed);
  snprintf(page_text, sizeof(page_text), "%d", page);
  snprintf(total_text, sizeof(total_text), "%d", total);
  const ArUiTextArgument args[] = {
    {"page", page_text}, {"total", total_text}, {"controls",
      ArUiCatalog_Text(locale, key, ManualInput_HintText(device, zoomed))},
  };
  const char *format = ArUiCatalog_Text(locale, spread
      ? "overlay.manual.opening" : "overlay.manual.page", NULL);
  if (!format[0] || !ArUiCatalog_Format(result.text, sizeof(result.text), format, args, 3))
    return false;
  int scale = height / 320;
  if (scale < 1) scale = 1;
  if (scale > 4) scale = 4;
  size_t bytes = strlen(result.text);
  for (; scale >= 1; --scale) {
    int glyph = cell_pixels * scale, pad = glyph / 2;
    if (width <= 2 * pad) continue;
    size_t max_cells = (size_t)((width - 2 * pad) / glyph);
    size_t offset = 0;
    result.line_count = 0;
    while (offset < bytes && result.line_count < kManualCaptionLines) {
      ArInterfaceTextLine line;
      if (!ArInterfaceText_WrapLine(result.text + offset, bytes - offset,
            max_cells, sizeof(result.text) - 1, &line) || !line.consumed) break;
      result.lines[result.line_count++] = (ManualCaptionLine){offset, line.bytes, line.cells};
      offset += line.consumed;
    }
    result.height = result.line_count * glyph + 2 * pad;
    if (offset != bytes || result.height > height) continue;
    result.scale = scale;
    result.padding = pad;
    *out = result;
    return true;
  }
  return false;
}
