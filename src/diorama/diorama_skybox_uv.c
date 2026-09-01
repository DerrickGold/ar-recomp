#include "diorama_skybox_uv.h"

#include "constants.h"

static int ClampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

static void ValidSpanForPolicy(int ws_extra, int budget,
                               int live_left, int live_right,
                               bool pad_captured_to_budget,
                               const ActionBgRowPolicy *policy, int tex_width,
                               int *out_x0, int *out_x1) {
  if (!out_x0 || !out_x1) return;
  if (tex_width <= 0) { *out_x0 = 0; *out_x1 = 0; return; }
  if (ws_extra < 0) ws_extra = 0;
  if (budget < 0) budget = 0;
  live_left = ClampInt(live_left, 0, budget);
  live_right = ClampInt(live_right, 0, budget);

  int margin_left, margin_right;
  switch (policy ? policy->edge : kActionBgEdge_RawWrap) {
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
  if (policy && policy->horizontal_extent.mode == kActionBgExtent_Fixed) {
    if (margin_left > policy->horizontal_extent.left)
      margin_left = policy->horizontal_extent.left;
    if (margin_right > policy->horizontal_extent.right)
      margin_right = policy->horizontal_extent.right;
  }

  *out_x0 = ClampInt(ws_extra - margin_left, 0, tex_width);
  *out_x1 = ClampInt(
      ws_extra + kActRaiserAuthenticWidth + margin_right, 0, tex_width);
  if (*out_x1 < *out_x0) *out_x1 = *out_x0;
}

static bool RowWithinVerticalExtent(const ActionBgLayerPlan *layer,
                                    int authentic_y) {
  if (!layer) return true;
  if (authentic_y < 0) {
    const int distance = -authentic_y;
    if (layer->world_height && distance > layer->camera_y) return false;
    if (layer->vertical_extent.mode == kActionBgExtent_Fixed &&
        distance > layer->vertical_extent.top)
      return false;
  } else if (authentic_y >= kActRaiserAuthenticHeight) {
    const int distance = authentic_y - (kActRaiserAuthenticHeight - 1);
    if (layer->world_height) {
      int available = (int)layer->world_height -
          kActRaiserActionCameraViewportHeight - (int)layer->camera_y;
      if (available < 0) available = 0;
      if (distance > available) return false;
    }
    if (layer->vertical_extent.mode == kActionBgExtent_Fixed &&
        distance > layer->vertical_extent.bottom)
      return false;
  }
  return true;
}

void DioramaBgValidSpanPlan_Build(
    int ws_extra, int budget, int live_left, int live_right,
    bool pad_captured_to_budget, const ActionBgLayerPlan *layer,
    int authentic_y0, int capture_height, int tex_width,
    DioramaBgValidSpanPlan *out) {
  if (!out) return;
  *out = (DioramaBgValidSpanPlan){ 0 };
  if (capture_height <= 0 || tex_width <= 0) return;
  const bool layer_valid = ActionBgLayerPlan_Validate(layer);

  for (int y = 0; y < capture_height; y++) {
    int x0 = 0, x1 = 0;
    const int authentic_y = y - authentic_y0;
    ActionBgRowPolicy policy = {
      .edge = kActionBgEdge_RawWrap,
      .horizontal_extent = { .mode = kActionBgExtent_Available },
    };
    if (layer_valid)
      ActionBgLayerPlan_ResolveValidatedRow(layer, authentic_y, &policy);
    if (!layer_valid || RowWithinVerticalExtent(layer, authentic_y)) {
      ValidSpanForPolicy(ws_extra, budget, live_left, live_right,
                         pad_captured_to_budget, &policy,
                         tex_width, &x0, &x1);
    }
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

bool DioramaBgValidSpanPlan_DrawableRowBounds(
    const DioramaBgValidSpanPlan *plan, int *out_y0, int *out_y1) {
  if (out_y0) *out_y0 = 0;
  if (out_y1) *out_y1 = 0;
  if (!plan || !plan->count || plan->count > kDioramaBgMaxValidSpans)
    return false;

  bool found = false;
  int y0 = 0, y1 = 0;
  for (uint8_t i = 0; i < plan->count; i++) {
    const DioramaBgValidSpan *span = &plan->spans[i];
    if (span->x1 <= span->x0 || span->y1 <= span->y0)
      continue;
    if (!found || span->y0 < y0) y0 = span->y0;
    if (!found || span->y1 > y1) y1 = span->y1;
    found = true;
  }
  if (!found) return false;
  if (out_y0) *out_y0 = y0;
  if (out_y1) *out_y1 = y1;
  return true;
}

bool DioramaSkyboxVerticalMapping_Build(
    const DioramaBgValidSpanPlan *plan, int capture_height,
    int texture_height, float blur_radius,
    DioramaSkyboxVerticalMapping *out) {
  if (out) *out = (DioramaSkyboxVerticalMapping){ 0 };
  if (!out || capture_height <= 0 || texture_height <= 0) return false;
  int y0 = 0, y1 = 0;
  if (!DioramaBgValidSpanPlan_DrawableRowBounds(plan, &y0, &y1))
    return false;
  y0 = ClampInt(y0, 0, capture_height);
  y1 = ClampInt(y1, 0, capture_height);
  if (y1 <= y0) return false;

  float sample_y0 = (float)y0;
  float sample_y1 = (float)y1;
  if (y0 != 0 || y1 != capture_height) {
    if (blur_radius < 0.0f) blur_radius = 0.0f;
    const float inset = blur_radius + 1.0f;
    sample_y0 += inset;
    sample_y1 -= inset;
    if (sample_y1 < sample_y0) sample_y1 = sample_y0;
  }
  *out = (DioramaSkyboxVerticalMapping) {
    .capture_y0 = y0,
    .capture_y1 = y1,
    .texture_v0 = sample_y0 / (float)texture_height,
    .texture_v1 = sample_y1 / (float)texture_height,
  };
  return true;
}

float DioramaSkyboxVerticalMapping_Fraction(
    const DioramaSkyboxVerticalMapping *mapping, int capture_y) {
  if (!mapping || mapping->capture_y1 <= mapping->capture_y0) return 0.0f;
  capture_y = ClampInt(
      capture_y, mapping->capture_y0, mapping->capture_y1);
  return (float)(capture_y - mapping->capture_y0) /
      (float)(mapping->capture_y1 - mapping->capture_y0);
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

void DioramaRomSkyboxUvRange(int display_width, int source_width,
                             float *out_u0, float *out_u1) {
  if (!out_u0 || !out_u1) return;
  if (display_width <= 0 || source_width <= 0) {
    *out_u0 = 0.0f;
    *out_u1 = 0.0f;
    return;
  }
  *out_u0 = 0.0f;
  *out_u1 = (float)display_width / (float)source_width;
}
