#include "render_preparation.h"
#include "crt_post.h"
#include "diorama/diorama_effect_backend.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_cloud_effect_backend.h"
#include "sim/sim_shadow_effect_backend.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

/* Fault-injected portable boundary: no GPU, settings globals or game state. */
static bool available = true, fail_create, fail_clear, fail_end, fatal;
static int creates, destroys, ends, shader_calls;
static ArRenderTargetBeginResult begin_result = kArRenderTargetBegin_Ready;
static ArRenderBlendMode rejected_blend = kArRenderBlendMode_Opaque;
static Sim3DPreparedPipelines pipelines = {
  .depth = true, .linear_models = true, .radial = true, .surfaces = true, .spherical_body = true,
};
bool SessionFatal_Requested(void) { return fatal; }
bool ArRenderDevice_IsReady(const ArRenderDevice *device) { return device != NULL; }
const ArRenderCapabilities *ArRenderDevice_Capabilities(const ArRenderDevice *device) {
  return &device->capabilities;
}
Sim3DPreparedPipelines Sim3DDepthPass_PreparePipelines(ArRenderDevice *device) {
  (void)device; ++shader_calls; return pipelines;
}
bool DioramaEffectBackend_IsAvailable(ArRenderDevice *device, DioramaEffectKind kind) {
  (void)device; (void)kind; ++shader_calls; return available;
}
bool CrtPost_Prepare(ArRenderDevice *device) { (void)device; ++shader_calls; return available; }
bool SimCloudEffectBackend_IsAvailable(ArRenderDevice *device) {
  (void)device; ++shader_calls; return available;
}
bool SimShadowEffectBackend_IsAvailable(ArRenderDevice *device) {
  (void)device; ++shader_calls; return available;
}
bool ArRenderDevice_CreateTexture(ArRenderDevice *device,
    const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)device; assert(desc->width <= 8 && desc->height <= 8);
  if (fail_create) return false;
  *out = (ArRenderTexture){++creates}; return true;
}
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectI *rect, const void *pixels, int pitch) {
  (void)device; (void)rect;
  assert(texture.value && pixels && pitch == 4); return true;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device; if (texture.value) ++destroys;
}
ArRenderTargetBeginResult ArRenderDevice_BeginTarget(ArRenderDevice *device,
    ArRenderTexture texture, ArRenderTargetState *state) {
  (void)device; assert(texture.value); state->valid = true; return begin_result;
}
bool ArRenderDevice_EndTarget(ArRenderDevice *device, const ArRenderTargetState *state) {
  (void)device; assert(state->valid); ++ends; return !fail_end;
}
bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color) {
  (void)device; (void)color; return !fail_clear;
}
bool ArRenderDevice_DrawGeometryWithState(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count, const int32_t *indices,
    int index_count, const ArRenderDrawState *state) {
  (void)device; (void)texture;
  assert(vertices && indices && vertex_count == 3 && index_count == 3);
  return state->blend != rejected_blend;
}
int main(void) {
  ArRenderDevice device = {0};
  RenderFeatureMask features = 0;
  assert(!RenderPreparation_Prepare(NULL, &features));
  assert(!RenderPreparation_Prepare(&device, NULL));
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features == kRenderFeature_All && shader_calls == 7 && ends == 1);
  assert(creates == destroys);
  /* Linear meshes are an acceleration, not a distinct visual setting. A
   * prepared rejection retains ordinary SIM geometry and unrelated features. */
  pipelines.linear_models = false;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features == kRenderFeature_All && shader_calls == 14 && ends == 2 && creates == destroys);
  pipelines.linear_models = true;
  available = false;
  pipelines.radial = false;
  rejected_blend = kArRenderBlendMode_DestinationAlphaMask;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features == (kRenderFeature_Depth | kRenderFeature_Effects | kRenderFeature_SimSoftShadows));
  assert(shader_calls == 21 && creates == destroys);
  fail_clear = true;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(ends == 4 && creates == destroys); /* clear failure still restores */
  fail_clear = false;
  fail_end = true;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(ends == 5 && creates == destroys);
  fail_end = false;
  begin_result = kArRenderTargetBegin_StateLost;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(ends == 5 && creates == destroys); /* never End an unentered target */
  begin_result = kArRenderTargetBegin_Omitted;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(ends == 5 && creates == destroys);
  fail_create = true;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(creates == destroys);
  fatal = true;
  fail_create = false;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(creates == destroys);
  puts("render preparation: PASS");
}
