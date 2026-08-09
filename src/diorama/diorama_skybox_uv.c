#include "diorama_skybox_uv.h"

enum { kAuthenticWidth = 256 };

static int ClampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

static void ValidSpanForEdge(int ws_extra, int budget,
                             int live_left, int live_right,
                             bool pad_captured_to_budget,
                             ActionBgEdgeMode edge, int tex_width,
                             int *out_x0, int *out_x1) {
  if (!out_x0 || !out_x1) return;
  if (tex_width <= 0) { *out_x0 = 0; *out_x1 = 0; return; }
  if (ws_extra < 0) ws_extra = 0;
  if (budget < 0) budget = 0;
  live_left = ClampInt(live_left, 0, budget);
  live_right = ClampInt(live_right, 0, budget);

  int margin_left, margin_right;
  switch (edge) {
    case kActionBgEdge_Mirror:
    case kActionBgEdge_Repeat:
      /* Fix A synthesized padding out to the whole budget. */
      margin_left = pad_captured_to_budget ? budget : live_left;
      margin_right = pad_captured_to_budget ? budget : live_right;
      break;
    case kActionBgEdge_Clamp:
    case kActionBgEdge_Transparent:
      /* Nothing outside the authentic 256 was ever drawn. */
      margin_left = 0;
      margin_right = 0;
      break;
    case kActionBgEdge_LiveWorld:
    case kActionBgEdge_RawWrap:
    default:
      margin_left = live_left;
      margin_right = live_right;
      break;
  }

  *out_x0 = ClampInt(ws_extra - margin_left, 0, tex_width);
  *out_x1 = ClampInt(ws_extra + kAuthenticWidth + margin_right, 0, tex_width);
  if (*out_x1 < *out_x0) *out_x1 = *out_x0;
}

static ActionBgEdgeMode EdgeAtCaptureRow(const ActionBgLayerPlan *layer,
                                         int authentic_y0, int capture_y) {
  if (!layer || !layer->valid || layer->band_count > kActionBgMaxBands)
    return kActionBgEdge_RawWrap;
  ActionBgEdgeMode edge = layer->default_edge;
  const int authentic_y = capture_y - authentic_y0;
  for (unsigned i = 0; i < layer->band_count; i++) {
    const ActionBgBand *band = &layer->bands[i];
    if (band->y0 < band->y1 && authentic_y >= (int)band->y0 &&
        authentic_y < (int)band->y1)
      edge = band->edge;
  }
  return edge;
}

void DioramaBgValidSpanPlan_Build(
    int ws_extra, int budget, int live_left, int live_right,
    bool pad_captured_to_budget, const ActionBgLayerPlan *layer,
    int authentic_y0, int capture_height, int tex_width,
    DioramaBgValidSpanPlan *out) {
  if (!out) return;
  *out = (DioramaBgValidSpanPlan){ 0 };
  if (capture_height <= 0 || tex_width <= 0) return;

  for (int y = 0; y < capture_height; y++) {
    int x0 = 0, x1 = 0;
    ValidSpanForEdge(ws_extra, budget, live_left, live_right,
                     pad_captured_to_budget,
                     EdgeAtCaptureRow(layer, authentic_y0, y),
                     tex_width, &x0, &x1);
    if (out->count) {
      DioramaBgValidSpan *previous = &out->spans[out->count - 1];
      if (previous->y1 == y && previous->x0 == x0 && previous->x1 == x1) {
        previous->y1 = y + 1;
        continue;
      }
    }
    /* The bound follows from the fixed band capacity. Treat malformed input
     * as fail-closed rather than writing past the handoff record. */
    if (out->count >= kDioramaBgMaxValidSpans) return;
    out->spans[out->count++] = (DioramaBgValidSpan) {
      .y0 = y,
      .y1 = y + 1,
      .x0 = x0,
      .x1 = x1,
    };
  }
}

void DioramaSkyboxUvRange(int tex_width, int valid_x0, int valid_x1,
                          float blur_radius, float *out_u0, float *out_u1) {
  if (!out_u0 || !out_u1) return;
  if (tex_width <= 0) { *out_u0 = 0.0f; *out_u1 = 0.0f; return; }
  if (blur_radius < 0.0f) blur_radius = 0.0f;
  const float width = (float)tex_width;
  /* radius+1 leaves one texel of slack beyond the kernel's furthest tap. */
  const float inset = (blur_radius + 1.0f) / width;
  float u0 = (float)valid_x0 / width + inset;
  float u1 = (float)valid_x1 / width - inset;
  /* Defensive: unreachable in practice (the span is always >= 256 texels, far
   * wider than any blur radius used), but an inverted range would sample
   * backwards rather than fail loudly. */
  if (u1 < u0) u1 = u0;
  *out_u0 = u0;
  *out_u1 = u1;
}
