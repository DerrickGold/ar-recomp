#include "diorama_skybox_uv.h"

/* BG2 is PPU layer 1. */
enum { kBg2LayerBit = 1u << 1 };
enum { kAuthenticWidth = 256 };

static int ClampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

int DioramaBg2MarginSource_Classify(uint8_t ws_clamp, uint8_t ws_mirror,
                                    uint8_t ws_repeat, bool bg2_repeat_band) {
  /* Order matters. A clamp bit wins outright: PpuLayerExtra returns 0 for a
   * clamped layer, so no margin is rendered regardless of anything else. A
   * repeat BAND applies to some rows only, which one span cannot express, so it
   * is folded into the conservative answer rather than the optimistic one. */
  if (ws_clamp & kBg2LayerBit) return kBg2Margin_Clamped;
  if (bg2_repeat_band) return kBg2Margin_Clamped;
  if ((ws_mirror | ws_repeat) & kBg2LayerBit) return kBg2Margin_Padded;
  return kBg2Margin_Live;
}

void DioramaBg2ValidSpan(int ws_extra, int budget, int live_left, int live_right,
                         int margin_source, int tex_width,
                         int *out_x0, int *out_x1) {
  if (!out_x0 || !out_x1) return;
  if (tex_width <= 0) { *out_x0 = 0; *out_x1 = 0; return; }
  if (ws_extra < 0) ws_extra = 0;
  if (budget < 0) budget = 0;
  live_left = ClampInt(live_left, 0, budget);
  live_right = ClampInt(live_right, 0, budget);

  int margin_left, margin_right;
  switch (margin_source) {
    case kBg2Margin_Padded:
      /* Fix A synthesized padding out to the whole budget. */
      margin_left = budget;
      margin_right = budget;
      break;
    case kBg2Margin_Clamped:
      /* Nothing outside the authentic 256 was ever drawn. */
      margin_left = 0;
      margin_right = 0;
      break;
    case kBg2Margin_Live:
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

static int MarginSourceForEdge(ActionBgEdgeMode edge,
                               bool pad_captured_to_budget) {
  switch (edge) {
    case kActionBgEdge_Clamp:
    case kActionBgEdge_Transparent:
      return kBg2Margin_Clamped;
    case kActionBgEdge_Mirror:
    case kActionBgEdge_Repeat:
      return pad_captured_to_budget ? kBg2Margin_Padded : kBg2Margin_Live;
    case kActionBgEdge_LiveWorld:
    case kActionBgEdge_RawWrap:
    default:
      return kBg2Margin_Live;
  }
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
    DioramaBg2ValidSpan(
        ws_extra, budget, live_left, live_right,
        MarginSourceForEdge(
            EdgeAtCaptureRow(layer, authentic_y0, y),
            pad_captured_to_budget),
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
