#ifndef AR_MANUAL_CAPTION_H
#define AR_MANUAL_CAPTION_H

#include "manual_input.h"
#include "localization/ui_catalog.h"

enum { kManualCaptionBytes = 768, kManualCaptionLines = 8 };
typedef struct ManualCaptionLine {
  size_t offset, bytes, cells;
} ManualCaptionLine;
typedef struct ManualCaption {
  char text[kManualCaptionBytes];
  ManualCaptionLine lines[kManualCaptionLines];
  int line_count, scale, padding, height;
} ManualCaption;

/* Pure, bounded host presentation. One-based page/opening, no page image or
 * input-state mutation. Wraps complete graphemes and reduces integer scale if
 * needed. False leaves output unchanged, including an impossibly small view. */
bool ManualCaption_Build(ArUiLocale locale, ManualHintDevice device, bool zoomed,
    bool spread, int page, int total, int width, int height, int cell_pixels,
    ManualCaption *out);

#endif
