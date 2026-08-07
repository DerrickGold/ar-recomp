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
