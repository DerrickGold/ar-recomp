#include "sim_backdrop_render.h"

#include "constants.h"
#include "scene3d_math.h"

/* The finite ground reveals this gradient only where no opaque geometry was
 * drawn. Supported camera pitches keep the real vanishing line off-screen, so
 * a synthetic horizon anchors the gradient unless the real one is visible. */
static const ArRenderColorF kSkyHorizon = {0.60f, 0.74f, 0.90f, 1.0f};
static const ArRenderColorF kSkyZenith = {0.16f, 0.33f, 0.66f, 1.0f};

enum {
  /* Percent, at full strength: how far each end is taken toward its sky
   * colour. The horizon is the readable half; the zenith stays quieter. */
  kHorizonMixPct = 82,
  kZenithMixPct = 62,
};

static float GradientAt(float screen_y, float horizon_y, float span) {
  if (span <= 0.0f) return 0.0f;
  const float t = (horizon_y - screen_y) / span;
  return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

bool SimBackdropRender_Build(const SimBackdropRenderInput *input,
                             SimBackdropRenderBatch *batch) {
  if (!batch) return false;
  *batch = (SimBackdropRenderBatch){0};
  if (!input || input->viewport.w <= 0 || input->viewport.h <= 0)
    return false;

  const uint32_t backdrop = input->backdrop_argb;
  const float base_r = (float)((backdrop >> 16) & 0xFF) / 255.0f;
  const float base_g = (float)((backdrop >> 8) & 0xFF) / 255.0f;
  const float base_b = (float)(backdrop & 0xFF) / 255.0f;
  const float strength =
      (float)input->strength_pct / (float)kPercentScale;
  const float horizon_mix =
      (float)kHorizonMixPct / (float)kPercentScale * strength;
  const float zenith_mix =
      (float)kZenithMixPct / (float)kPercentScale * strength;
  const ArRenderColorF horizon = {
    base_r + (kSkyHorizon.r - base_r) * horizon_mix,
    base_g + (kSkyHorizon.g - base_g) * horizon_mix,
    base_b + (kSkyHorizon.b - base_b) * horizon_mix,
    1.0f,
  };
  const ArRenderColorF zenith = {
    base_r + (kSkyZenith.r - base_r) * zenith_mix,
    base_g + (kSkyZenith.g - base_g) * zenith_mix,
    base_b + (kSkyZenith.b - base_b) * zenith_mix,
    1.0f,
  };

  const float top = (float)input->viewport.y;
  const float bottom = (float)(input->viewport.y + input->viewport.h);
  float horizon_y = 0.0f;
  const bool horizon_visible = input->matrix &&
      Scene3D_GroundHorizonScreenY(
          input->matrix, input->viewport.h, &horizon_y) &&
      (horizon_y += (float)input->viewport.y,
       horizon_y > top && horizon_y < bottom);
  const float anchor = horizon_visible
      ? horizon_y
      : top + (float)input->viewport.h *
            (float)input->horizon_pct / (float)kPercentScale;

  float rows[3];
  int row_count = 0;
  rows[row_count++] = top;
  if (anchor > top && anchor < bottom) rows[row_count++] = anchor;
  rows[row_count++] = bottom;

  float span = anchor - top;
  if (span < 1.0f) span = 1.0f;
  const float left = (float)input->viewport.x;
  const float right = (float)(input->viewport.x + input->viewport.w);
  for (int row = 0; row < row_count; row++) {
    const float t = GradientAt(rows[row], anchor, span);
    const ArRenderColorF color = {
      horizon.r + (zenith.r - horizon.r) * t,
      horizon.g + (zenith.g - horizon.g) * t,
      horizon.b + (zenith.b - horizon.b) * t,
      1.0f,
    };
    batch->vertices[batch->vertex_count++] =
        (ArRenderVertex2D){{left, rows[row]}, color, {0.0f, 0.0f}};
    batch->vertices[batch->vertex_count++] =
        (ArRenderVertex2D){{right, rows[row]}, color, {0.0f, 0.0f}};
  }
  for (int row = 0; row + 1 < row_count; row++) {
    const int32_t top_left = row * 2;
    batch->indices[batch->index_count++] = top_left;
    batch->indices[batch->index_count++] = top_left + 1;
    batch->indices[batch->index_count++] = top_left + 3;
    batch->indices[batch->index_count++] = top_left;
    batch->indices[batch->index_count++] = top_left + 3;
    batch->indices[batch->index_count++] = top_left + 2;
  }
  return true;
}
