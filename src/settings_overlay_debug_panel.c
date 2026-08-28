#include "settings_overlay.h"
#include "settings_overlay_internal.h"
#include "constants.h"
#include "render/render_output.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The draggable, resizable F-key diagnostic panel (scene inspector output,
 * live report text). A distinct feature from the settings menu, sharing only
 * the overlay render device, debug font, layout math and low-level draw
 * primitives declared in settings_overlay_internal.h. Split out of
 * settings_overlay.c so the menu core is not 3900 lines. State below is private
 * to the panel. */

enum {
  kDebugPanelMinimumScalePercent = 50,
  kDebugPanelMaximumScalePercent = 250,
  kDebugPanelScaleStepPercent = 5,
  kDebugPanelMaximumTextLines = 12,
  kDebugPanelOuterMargin = 8,
  kDebugPanelHorizontalPadding = 12,
  kDebugPanelTitleBaselineY = 9,
  kDebugPanelContentY = 21,
  kDebugPanelVerticalChrome = 30,
  kDebugPanelMinimumWidth = 200,
  kDebugPanelMaximumWidth = 560,
  kDebugPanelDragStripHeight = 20,
  kDebugPanelResizeHandleSize = 18,
};

static int ClampDebugPanelScale(int scale_percent) {
  if (scale_percent < kDebugPanelMinimumScalePercent)
    return kDebugPanelMinimumScalePercent;
  if (scale_percent > kDebugPanelMaximumScalePercent)
    return kDebugPanelMaximumScalePercent;
  return scale_percent;
}

static SDL_Rect s_debug_panel_rect;
static SDL_Rect s_debug_panel_drag_rect;
static SDL_Rect s_debug_panel_resize_rect;
static bool s_debug_panel_visible;
static bool s_debug_panel_dragging;
static bool s_debug_panel_resizing;
static bool s_debug_panel_user_position;
static int s_debug_panel_scale_percent;
static int s_debug_panel_render_scale_percent;
static int s_debug_panel_output_x;
static int s_debug_panel_output_y;
static int s_debug_panel_drag_offset_x;
static int s_debug_panel_drag_offset_y;
static int s_debug_panel_resize_start_x;
static int s_debug_panel_resize_start_y;
static int s_debug_panel_resize_start_width;
static int s_debug_panel_resize_start_height;
static int s_debug_panel_resize_start_scale;

void SettingsOverlayDebugPanel_Reset(void) {
  s_debug_panel_visible = false;
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
  s_debug_panel_user_position = false;
  s_debug_panel_scale_percent = 0;
  s_debug_panel_render_scale_percent = 0;
}

void SettingsOverlay_RenderDebugPanel(const char *title, const char *text,
                                      SDL_Point avoid_point) {
  if (!s_render_device || !ArRenderTexture_IsValid(s_debug_font_texture) ||
      !text || !text[0])
    return;
  int output_width = 0, output_height = 0;
  if (!ArRenderOutput_UseFull(
          s_render_device, &output_width, &output_height))
    return;

  MenuLayout layout = BuildLayout(output_width, output_height);
  /* Debug reports should remain information-dense even when the settings
   * menu itself is enlarged for couch-distance use. A lower-right resize grip
   * can then override this automatic scale without changing the report's
   * logical width or truncating additional columns. */
  int automatic_scale = layout.scale_percent;
  automatic_scale = ClampDebugPanelScale(automatic_scale);
  int maximum_scale = SnappedFitScale(output_width, output_height);
  maximum_scale = ClampDebugPanelScale(maximum_scale);
  int debug_scale = s_debug_panel_scale_percent > 0
      ? s_debug_panel_scale_percent : automatic_scale;
  if (debug_scale > maximum_scale) debug_scale = maximum_scale;
  debug_scale = ClampDebugPanelScale(debug_scale);
  layout = BuildLayoutAtScale(output_width, output_height, debug_scale);
  s_debug_panel_render_scale_percent = debug_scale;
  const char *debug_title = title ? title : "DEBUG";
  int lines = 0;
  int longest_line = 0;
  const char *measure = text;
  while (*measure && lines < kDebugPanelMaximumTextLines) {
    const char *newline = strchr(measure, '\n');
    int length = newline ? (int)(newline - measure) : (int)strlen(measure);
    if (length > longest_line) longest_line = length;
    lines++;
    if (!newline) break;
    measure = newline + 1;
  }
  if (lines < 1) lines = 1;
  int panel_height =
      kDebugPanelVerticalChrome + lines * kDebugLineHeight;
  if (panel_height > layout.logical_height - 2 * kDebugPanelOuterMargin)
    panel_height = layout.logical_height - 2 * kDebugPanelOuterMargin;
  int title_length = (int)strlen(debug_title);
  int content_chars = longest_line > title_length
      ? longest_line : title_length;
  int panel_width = content_chars * kDebugGlyphWidth +
      2 * kDebugPanelHorizontalPadding;
  if (panel_width < kDebugPanelMinimumWidth)
    panel_width = kDebugPanelMinimumWidth;
  panel_width = (panel_width + kGlyphSize - 1) & ~(kGlyphSize - 1);
  int maximum_panel_width =
      layout.logical_width - 2 * kDebugPanelOuterMargin;
  if (maximum_panel_width > kDebugPanelMaximumWidth)
    maximum_panel_width = kDebugPanelMaximumWidth;
  maximum_panel_width &= ~(kGlyphSize - 1);
  if (panel_width > maximum_panel_width) panel_width = maximum_panel_width;
  int panel_x = (layout.logical_width - panel_width) / 2;
  int panel_y = avoid_point.y < output_height / 2
      ? layout.logical_height - panel_height - kDebugPanelOuterMargin
      : kDebugPanelOuterMargin;
  if (s_debug_panel_user_position) {
    panel_x = (s_debug_panel_output_x - layout.origin_x) *
        kPercentScale /
        layout.scale_percent;
    panel_y = (s_debug_panel_output_y - layout.origin_y) *
        kPercentScale /
        layout.scale_percent;
  }
  int max_x = layout.logical_width - panel_width;
  int max_y = layout.logical_height - panel_height;
  if (panel_x < 0) panel_x = 0;
  if (panel_y < 0) panel_y = 0;
  if (panel_x > max_x) panel_x = max_x;
  if (panel_y > max_y) panel_y = max_y;
  DrawDialogPanel(&layout, panel_x, panel_y, panel_width, panel_height);
  DrawDebugTextN(&layout,
                 panel_x + kDebugPanelHorizontalPadding,
                 panel_y + kDebugPanelTitleBaselineY,
                 debug_title, (int)strlen(debug_title), kDebugText_Label);

  int max_chars =
      (panel_width - 2 * kDebugPanelHorizontalPadding) /
      kDebugGlyphWidth;
  const char *cursor = text;
  for (int line = 0; line < lines && *cursor; line++) {
    const char *newline = strchr(cursor, '\n');
    int length = newline ? (int)(newline - cursor) : (int)strlen(cursor);
    if (length > max_chars) length = max_chars;
    DrawDebugHighlightedLine(
        &layout, panel_x + kDebugPanelHorizontalPadding,
        panel_y + kDebugPanelContentY + line * kDebugLineHeight,
        cursor, length);
    if (!newline) break;
    cursor = newline + 1;
  }

  /* Three short diagonal bars advertise the scale handle without replacing
   * the native bottom-right frame corner. */
  FillLogicalRect(&layout, panel_x + panel_width - 13,
                  panel_y + panel_height - 5, 9, 1,
                  ARGB(255, 92, 196, 255));
  FillLogicalRect(&layout, panel_x + panel_width - 10,
                  panel_y + panel_height - 8, 6, 1,
                  ARGB(255, 92, 196, 255));
  FillLogicalRect(&layout, panel_x + panel_width - 7,
                  panel_y + panel_height - 11, 3, 1,
                  ARGB(255, 92, 196, 255));

  s_debug_panel_rect = LogicalRect(
      &layout, panel_x, panel_y, panel_width, panel_height);
  /* The title strip moves the panel and the lower-right corner scales it.
   * Remaining report-body clicks pass through to the scene inspector so a
   * panel covering a requested sample cannot retain an old crosshair. */
  s_debug_panel_drag_rect = LogicalRect(
      &layout, panel_x, panel_y, panel_width,
      kDebugPanelDragStripHeight);
  s_debug_panel_resize_rect = LogicalRect(
      &layout, panel_x + panel_width - kDebugPanelResizeHandleSize,
      panel_y + panel_height - kDebugPanelResizeHandleSize,
      kDebugPanelResizeHandleSize, kDebugPanelResizeHandleSize);
  s_debug_panel_visible = true;
  if (s_debug_panel_user_position) {
    s_debug_panel_output_x = s_debug_panel_rect.x;
    s_debug_panel_output_y = s_debug_panel_rect.y;
  }

}

void SettingsOverlay_HideDebugPanel(void) {
  s_debug_panel_visible = false;
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
}

bool SettingsOverlay_BeginDebugPanelDrag(int output_x, int output_y) {
  if (!s_debug_panel_visible) return false;
  if (output_x >= s_debug_panel_resize_rect.x &&
      output_x < s_debug_panel_resize_rect.x + s_debug_panel_resize_rect.w &&
      output_y >= s_debug_panel_resize_rect.y &&
      output_y < s_debug_panel_resize_rect.y + s_debug_panel_resize_rect.h) {
    s_debug_panel_resizing = true;
    s_debug_panel_dragging = false;
    s_debug_panel_user_position = true;
    s_debug_panel_output_x = s_debug_panel_rect.x;
    s_debug_panel_output_y = s_debug_panel_rect.y;
    s_debug_panel_resize_start_x = output_x;
    s_debug_panel_resize_start_y = output_y;
    s_debug_panel_resize_start_width = s_debug_panel_rect.w;
    s_debug_panel_resize_start_height = s_debug_panel_rect.h;
    s_debug_panel_resize_start_scale = s_debug_panel_render_scale_percent;
    return true;
  }
  if (output_x < s_debug_panel_drag_rect.x ||
      output_x >= s_debug_panel_drag_rect.x + s_debug_panel_drag_rect.w ||
      output_y < s_debug_panel_drag_rect.y ||
      output_y >= s_debug_panel_drag_rect.y + s_debug_panel_drag_rect.h)
    return false;
  s_debug_panel_dragging = true;
  s_debug_panel_user_position = true;
  s_debug_panel_output_x = s_debug_panel_rect.x;
  s_debug_panel_output_y = s_debug_panel_rect.y;
  s_debug_panel_drag_offset_x = output_x - s_debug_panel_rect.x;
  s_debug_panel_drag_offset_y = output_y - s_debug_panel_rect.y;
  return true;
}

void SettingsOverlay_DragDebugPanel(int output_x, int output_y) {
  if ((!s_debug_panel_dragging && !s_debug_panel_resizing) ||
      !s_render_device)
    return;
  if (s_debug_panel_resizing) {
    int dx = output_x - s_debug_panel_resize_start_x;
    int dy = output_y - s_debug_panel_resize_start_y;
    int change_x = s_debug_panel_resize_start_width > 0
        ? dx * kPercentScale /
            s_debug_panel_resize_start_width : 0;
    int change_y = s_debug_panel_resize_start_height > 0
        ? dy * kPercentScale /
            s_debug_panel_resize_start_height : 0;
    int change = abs(change_x) >= abs(change_y) ? change_x : change_y;
    int scale = s_debug_panel_resize_start_scale *
        (kPercentScale + change) /
        kPercentScale;
    scale = ((scale + kDebugPanelScaleStepPercent / 2) /
             kDebugPanelScaleStepPercent) *
        kDebugPanelScaleStepPercent;
    scale = ClampDebugPanelScale(scale);
    s_debug_panel_scale_percent = scale;
    return;
  }
  int output_width = 0, output_height = 0;
  if (!ArRenderDevice_GetOutputSize(
          s_render_device, &output_width, &output_height) ||
      output_width <= 0 || output_height <= 0)
    return;
  int x = output_x - s_debug_panel_drag_offset_x;
  int y = output_y - s_debug_panel_drag_offset_y;
  int max_x = output_width - s_debug_panel_rect.w;
  int max_y = output_height - s_debug_panel_rect.h;
  if (max_x < 0) max_x = 0;
  if (max_y < 0) max_y = 0;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > max_x) x = max_x;
  if (y > max_y) y = max_y;
  s_debug_panel_output_x = x;
  s_debug_panel_output_y = y;
  int drag_dx = x - s_debug_panel_rect.x;
  int drag_dy = y - s_debug_panel_rect.y;
  s_debug_panel_rect.x = x;
  s_debug_panel_rect.y = y;
  s_debug_panel_drag_rect.x += drag_dx;
  s_debug_panel_drag_rect.y += drag_dy;
  s_debug_panel_resize_rect.x += drag_dx;
  s_debug_panel_resize_rect.y += drag_dy;
}

void SettingsOverlay_EndDebugPanelDrag(void) {
  s_debug_panel_dragging = false;
  s_debug_panel_resizing = false;
}

bool SettingsOverlay_IsDebugPanelDragging(void) {
  return s_debug_panel_dragging || s_debug_panel_resizing;
}

bool SettingsOverlay_GetDebugPanelRect(SDL_Rect *rect) {
  if (!s_debug_panel_visible || !rect) return false;
  *rect = s_debug_panel_rect;
  return true;
}
