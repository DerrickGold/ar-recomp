#include "present_sim3d_clouds.h"
#include "present_sim3d_project.h"
#include "sim/sim_cloud_effect_backend.h"
#include "sim/sim3d_performance.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

ArRenderDevice g_render_device;
static unsigned projections, coverage_calls, draws, creates, unbinds;
static bool fail_create, fail_projection, gpu, fail_bind, fail_unbind, fail_draw;
static uint64_t clock_ms = 123456, geometry_hash;

uint64_t HostClock_Milliseconds(void) { return clock_ms; }
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  assert(vertices && indices);
}
float SimHeightWorldUnits(ArRenderRectI source, int height, unsigned scale) {
  return (float)height / source.h * scale / 100.0f;
}
float SimTerrainMaximumHeightWorld(const FrameSlot *slot, ArRenderRectI source) {
  (void)source;
  return slot->sim.landscape_height_pct / 1000.0f;
}
bool ProjectSimTexturePoint(const float matrix[16], ArRenderRectI source,
    ArRenderRectI viewport, float x, float y, float z, Scene3DPoint *out) {
  projections++;
  if (fail_projection) return false;
  float aspect = (float)viewport.w / viewport.h;
  if (!Scene3D_ProjectWorldPoint(matrix, ((x-source.x)/source.w-0.5f)*aspect,
          0.5f-(y-source.y)/source.h, z, viewport.w, viewport.h, out)) return false;
  out->x += viewport.x; out->y += viewport.y;
  return true;
}
/* Controlled dependency: exercise every coverage argument while the separate
 * SIM metadata tests own the authored coverage function's numeric oracle. */
float Sim3D_CloudCoverage(float x, float y, float x0, float x1, float y0, float y1,
    float inset, float falloff) {
  coverage_calls++;
  float distance = fmaxf(fmaxf(x0-x, x-x1), fmaxf(y0-y, y-y1));
  return fminf(1, fmaxf(0, (distance+inset)/fmaxf(1, falloff+inset)));
}
bool ArRenderDevice_CreateTexture(ArRenderDevice *device, const ArRenderTextureDesc *desc,
    ArRenderTexture *out) {
  (void)device; assert(desc->width > 0); creates++;
  *out = (ArRenderTexture){fail_create ? 0 : 1};
  return !fail_create;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device; (void)texture;
}
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectI *rect, const void *pixels, int pitch) {
  (void)device; (void)rect; assert(texture.value && pixels && pitch > 0); return true;
}
const char *ArRenderDevice_LastError(const ArRenderDevice *device) {
  (void)device; return "injected failure";
}
static void Hash(const void *bytes, size_t size) {
  const unsigned char *p = bytes;
  for (size_t i = 0; i < size; i++) geometry_hash = (geometry_hash ^ p[i]) * UINT64_C(1099511628211);
}
bool ArRenderDevice_DrawGeometryWithState(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int count, const int32_t *indices, int index_count,
    const ArRenderDrawState *state) {
  (void)device; assert(texture.value && state->address_u == kArRenderTextureAddressMode_Wrap);
  draws++;
  Hash(vertices, (size_t)count * sizeof(*vertices));
  Hash(indices, (size_t)index_count * sizeof(*indices));
  return !fail_draw;
}
bool SimCloudEffectBackend_IsAvailable(ArRenderDevice *device) { (void)device; return gpu; }
bool SimCloudEffectBackend_Bind(ArRenderDevice *device, const SimCloudEffectParams *params) {
  (void)device; assert(params); return !fail_bind;
}
bool SimCloudEffectBackend_Unbind(ArRenderDevice *device) { (void)device; unbinds++; return !fail_unbind; }
void SimCloudEffectBackend_Reset(ArRenderDevice *device) { (void)device; }

static PresentationOutcome Draw(const FrameSlot *slot, ArRenderRectI source,
    ArRenderRectI viewport, const float matrix[16]) {
  geometry_hash = UINT64_C(14695981039346656037);
  draws = projections = coverage_calls = 0;
  return DrawSimCloudShroud(slot, source, viewport, matrix);
}

int main(void) {
  static FrameSlot slot;
  slot.sim.underlay_serial = 1;
  slot.sim.cloud_opacity_pct = 60;
  slot.sim.cloud_altitude_px = 20;
  slot.sim.height_scale_x100 = 100;
  slot.sim.cloud_clear_x1 = 256;
  slot.sim.cloud_clear_y1 = 224;
  slot.sim.cloud_falloff_px = 100;
  slot.sim.cloud_inset_px = 8;
  slot.sim.cloud_drift_pct = 100;
  ArRenderRectI source = {0, 0, 256, 224}, viewport = {0, 0, 800, 600};
  Scene3DCamera camera = {.tilt_x=-0.35f, .distance=5, .fov_y=0.4f};
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, matrix);
  const unsigned vertices = (kSimUnderlayColumns+1) * (kSimUnderlayRows+1);
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(projections == vertices && coverage_calls == vertices && draws == 3);
  const uint64_t original = geometry_hash;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(!projections && !coverage_calls && geometry_hash == original);
  clock_ms += 1234;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(!projections && geometry_hash != original);
  clock_ms -= 1234;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(!projections && geometry_hash == original);
  for (int change = 0; change < 24; change++) {
    switch (change) {
      case 0: slot.sim.camera_x++; break;
      case 1: slot.sim.camera_y++; break;
      case 2: slot.sim.underlay_origin_tile_x++; break;
      case 3: slot.sim.underlay_origin_tile_y++; break;
      case 4: slot.sim.underlay_screen_x0++; break;
      case 5: slot.sim.cloud_clear_x0++; break;
      case 6: slot.sim.cloud_clear_x1++; break;
      case 7: slot.sim.cloud_clear_y0++; break;
      case 8: slot.sim.cloud_clear_y1++; break;
      case 9: slot.sim.cloud_falloff_px++; break;
      case 10: slot.sim.cloud_inset_px++; break;
      case 11: slot.sim.cloud_opacity_pct++; break;
      case 12: slot.sim.cloud_altitude_px++; break;
      case 13: slot.sim.height_scale_x100++; break;
      case 14: slot.sim.landscape_height_pct++; break;
      case 15: source.x++; break;
      case 16: source.y++; break;
      case 17: source.w++; break;
      case 18: source.h++; break;
      case 19: viewport.x++; break;
      case 20: viewport.y++; break;
      case 21: viewport.w++; break;
      case 22: viewport.h++; break;
      case 23: matrix[12] += 0.01f; break;
    }
    assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
    assert(projections == vertices && draws == 3);
    const uint64_t changed = geometry_hash;
    assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
    assert(!projections && geometry_hash == changed);
    const float saved = matrix[12];
    matrix[12] += 0.02f;
    (void)Draw(&slot, source, viewport, matrix);
    matrix[12] = saved;
    assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
    assert(projections == vertices && geometry_hash == changed);
  }
  gpu = true;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(draws == 1 && unbinds == 1);
  fail_bind = true;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(draws == 3 && unbinds == 2);
  fail_unbind = true;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_CoreFailure);
  assert(draws == 0);
  fail_bind = fail_unbind = false;
  fail_draw = true;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_OptionalOmitted);
  assert(draws == 1); /* Never double-draw a partially submitted effect. */
  fail_draw = gpu = false;
  PresentSim3DClouds_ResetResources();
  fail_projection = true;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(projections == 1 && !draws);
  fail_projection = false;
  matrix[12] += 0.01f;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(projections == vertices && draws == 3);
  slot.sim.cloud_opacity_pct = 0;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(!draws && !projections);
  slot.sim.cloud_opacity_pct = 60;
  PresentSim3DClouds_ResetResources();
  fail_create = true;
  unsigned before = creates;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_OptionalOmitted);
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_OptionalOmitted);
  assert(creates == before+1 && !draws);
  PresentSim3DClouds_ResetResources();
  fail_create = false;
  assert(Draw(&slot, source, viewport, matrix) == kPresentationOutcome_Complete);
  assert(projections == vertices);
  PresentSim3DClouds_ResetResources();
  puts("present_sim3d_clouds_test: PASS");
  return 0;
}
