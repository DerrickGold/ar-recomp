#include "present_sim3d_internal.h"
#include "sim/sim_backdrop_render.h"
#include "sim/sim3d_performance.h"
extern ArRenderDevice g_render_device;

void DrawSimBackdrop(const FrameSlot *slot, ArRenderRectI viewport,
                     const float matrix[16]) {
  const SimBackdropRenderInput input = {
    .backdrop_argb = slot->sim.separated_backdrop_argb,
    .strength_pct = slot->sim.backdrop_strength_pct,
    .horizon_pct = slot->sim.backdrop_horizon_pct,
    .viewport = {viewport.x, viewport.y, viewport.w, viewport.h},
    .matrix = matrix,
  };
  SimBackdropRenderBatch batch;
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (SimBackdropRender_Build(&input, &batch) &&
      ArRenderDevice_DrawGeometryWithState(
          &g_render_device, ArRenderTexture_Invalid(),
          batch.vertices, batch.vertex_count,
          batch.indices, batch.index_count, &state)) {
    Sim3DPerformance_AddDraw(
        (uint64_t)batch.vertex_count, (uint64_t)batch.index_count);
  }
}
