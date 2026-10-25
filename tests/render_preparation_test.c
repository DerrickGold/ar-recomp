#include "present/render_preparation.h"
#include "render/crt_post.h"
#include "app/settings.h"
#include "present/preparation_indicator.h"
#include "diorama/diorama_bg_gpu.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_frame_generation.h"
#include "render/gpu_compile_log.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim_cloud_effect_backend.h"
#include "sim/sim3d/sim_shadow_effect_backend.h"
#include "sim/world_nav/completion_vista_backend.h"
#include "sim/world_nav/present_world_nav.h"
#include "support/test_assert.h"
#include <stdio.h>

/* Fault-injected portable boundary: no GPU, settings globals or game state. */
static bool available = true, fail_create, fail_clear, fail_end, fail_finish, fatal;
static int creates, destroys, ends, shader_calls;
static int finishes;
static int completion_prepares;
static ArRenderTargetBeginResult begin_result = kArRenderTargetBegin_Ready;
static ArRenderBlendMode rejected_blend = kArRenderBlendMode_Opaque;
static Sim3DPreparedPipelines pipelines = {
    .depth = true,
    .linear_models = true,
    .radial = true,
    .surfaces = true,
    .spherical_body = true,
};
Settings g_settings;
static bool diorama_on;
static int bg_prepares, action_prepares, interpolation_prepares, rift_prepares;
bool DioramaAstralRift_Prepare(ArRenderDevice *device) {
  (void)device;
  ++rift_prepares;
  return available;
}
bool DioramaEffectBackend_PrepareSceneryLight(ArRenderDevice *device) {
  (void)device;
  return available;
}
bool Diorama_ModeIsOn(void) { return diorama_on; }
bool Diorama_InterpolationIsOn(void) { return diorama_on && g_settings.gpu_interp_enabled; }
bool DioramaBgGpu_Prepare(ArRenderDevice *device) {
  (void)device;
  ++bg_prepares;
  return true;
}
bool DioramaFrameGeneration_PreparePipelines(ArRenderDevice *device, bool interpolation) {
  (void)device;
  ++action_prepares;
  interpolation_prepares += interpolation;
  return true;
}
static int indicator_begins, indicator_ticks, indicator_ends;
void PreparationIndicator_Begin(ArRenderDevice *device) {
  (void)device;
  ++indicator_begins;
}
void PreparationIndicator_Tick(ArRenderDevice *device) {
  (void)device;
  ++indicator_ticks;
}
void PreparationIndicator_End(ArRenderDevice *device) {
  (void)device;
  ++indicator_ends;
}
void GpuCompileLog_EndPreparation(void) {}
static GpuCompileLogObserver compile_observer;
static void *compile_observer_context;
void GpuCompileLog_BeginPreparation(GpuCompileLogObserver observer, void *context) {
  compile_observer = observer;
  compile_observer_context = context;
}
bool SessionFatal_Requested(void) { return fatal; }
bool ArRenderDevice_IsReady(const ArRenderDevice *device) { return device != NULL; }
bool ArRenderDevice_FinishPreparation(ArRenderDevice *device) {
  assert(device && creates == destroys);
  ++finishes;
  return !fail_finish;
}
const ArRenderCapabilities *ArRenderDevice_Capabilities(const ArRenderDevice *device) {
  return &device->capabilities;
}
Sim3DPreparedPipelines Sim3DDepthPass_PreparePipelines(ArRenderDevice *device) {
  (void)device;
  ++shader_calls;
  return pipelines;
}
bool DioramaEffectBackend_IsAvailable(ArRenderDevice *device, DioramaEffectKind kind) {
  (void)device;
  (void)kind;
  ++shader_calls;
  return available;
}
bool CrtPost_Prepare(ArRenderDevice *device) {
  (void)device;
  ++shader_calls;
  return available;
}
bool SimCloudEffectBackend_IsAvailable(ArRenderDevice *device) {
  (void)device;
  ++shader_calls;
  return available;
}
bool SimShadowEffectBackend_IsAvailable(ArRenderDevice *device) {
  (void)device;
  ++shader_calls;
  return available;
}
bool CompletionVistaBackend_IsAvailable(ArRenderDevice *device) {
  (void)device;
  ++shader_calls;
  return available;
}
bool PresentWorldNav_PrepareCompletionResources(void) {
  ++completion_prepares;
  return true;
}
bool ArRenderDevice_CreateTexture(ArRenderDevice *device, const ArRenderTextureDesc *desc,
                                  ArRenderTexture *out) {
  (void)device;
  assert(desc->width <= 8 && desc->height <= 8);
  if (fail_create) return false;
  *out = (ArRenderTexture){++creates};
  return true;
}
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device, ArRenderTexture texture,
                                  const ArRenderRectI *rect, const void *pixels, int pitch) {
  (void)device;
  (void)rect;
  assert(texture.value && pixels && pitch == 4);
  return true;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device;
  if (texture.value) ++destroys;
}
ArRenderTargetBeginResult ArRenderDevice_BeginTarget(ArRenderDevice *device,
                                                     ArRenderTexture texture,
                                                     ArRenderTargetState *state) {
  (void)device;
  assert(texture.value);
  state->valid = true;
  return begin_result;
}
bool ArRenderDevice_EndTarget(ArRenderDevice *device, const ArRenderTargetState *state) {
  (void)device;
  assert(state->valid);
  ++ends;
  return !fail_end;
}
bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color) {
  (void)device;
  (void)color;
  return !fail_clear;
}
bool ArRenderDevice_DrawGeometryWithState(ArRenderDevice *device, ArRenderTexture texture,
                                          const ArRenderVertex2D *vertices, int vertex_count,
                                          const int32_t *indices, int index_count,
                                          const ArRenderDrawState *state) {
  (void)device;
  (void)texture;
  assert(vertices && indices && vertex_count == 3 && index_count == 3);
  return state->blend != rejected_blend;
}
int main(void) {
  ArRenderDevice device = {0};
  RenderFeatureMask features = 0;
  assert(!RenderPreparation_Prepare(NULL, &features));
  assert(!RenderPreparation_Prepare(&device, NULL));
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features == kRenderFeature_All && shader_calls == 9 && ends == 1);
  assert(completion_prepares == 1);
  /* Both pipelines and Astral Rift art are ready even with their settings off. */
  assert(finishes == 1 && interpolation_prepares == 1 && rift_prepares == 1);
  assert(creates == destroys);
  /* Linear meshes are an acceleration, not a distinct visual setting. A
   * prepared rejection retains ordinary SIM geometry and unrelated features. */
  pipelines.linear_models = false;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features == kRenderFeature_All && shader_calls == 18 && ends == 2 && creates == destroys);
  pipelines.linear_models = true;
  available = false;
  pipelines.radial = false;
  rejected_blend = kArRenderBlendMode_DestinationAlphaMask;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(features ==
         (kRenderFeature_Depth | kRenderFeature_Effects | kRenderFeature_SimSoftShadows));
  assert(shader_calls == 27 && creates == destroys);
  assert(completion_prepares == 2); /* Unavailable shader needs no cloud bake. */
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
  fatal = false;
  begin_result = kArRenderTargetBegin_Ready;

  /* Boot prepares every optional feature even from a flat menu with
   * interpolation disabled. Setting changes reuse it; reset rebuilds it. */
  bg_prepares = action_prepares = interpolation_prepares = rift_prepares = 0;
  RenderPreparation_PrepareSelectedFeatures(&device);
  assert(bg_prepares == 0 && action_prepares == 0);
  diorama_on = true;
  RenderPreparation_PrepareSelectedFeatures(&device);
  RenderPreparation_PrepareSelectedFeatures(&device);
  assert(bg_prepares == 0 && action_prepares == 0 && interpolation_prepares == 0);
  g_settings.gpu_interp_enabled = true;
  RenderPreparation_PrepareSelectedFeatures(&device);
  RenderPreparation_PrepareSelectedFeatures(&device);
  assert(action_prepares == 0 && interpolation_prepares == 0);
  g_settings.death_heim_final_battle = true;
  RenderPreparation_PrepareSelectedFeatures(&device);
  assert(rift_prepares == 0); /* Toggle reuses the boot-prepared artwork. */
  assert(RenderPreparation_Prepare(&device, &features));
  assert(action_prepares == 1 && interpolation_prepares == 1 && rift_prepares == 1);
  RenderPreparation_PrepareSelectedFeatures(NULL);
  assert(action_prepares == 1);

  /* A flat boot must also prepare the saved interpolation preference; action
   * entry must not introduce a second compilation bracket. */
  diorama_on = false;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(action_prepares == 2 && interpolation_prepares == 2 && rift_prepares == 2);
  diorama_on = true;
  RenderPreparation_PrepareSelectedFeatures(&device);
  assert(action_prepares == 2 && interpolation_prepares == 2);

  fail_finish = true;
  assert(!RenderPreparation_Prepare(&device, &features));
  fail_finish = false;

  /* The indicator brackets every preparation, including one ended early by a
   * fatal request, ticks between steps, and is the compile log's observer. */
  indicator_begins = indicator_ticks = indicator_ends = 0;
  assert(RenderPreparation_Prepare(&device, &features));
  assert(indicator_begins == 1 && indicator_ends == 1 && indicator_ticks >= 8);
  const int ticks = indicator_ticks;
  compile_observer(compile_observer_context);
  assert(indicator_ticks == ticks + 1);
  fatal = true;
  assert(!RenderPreparation_Prepare(&device, &features));
  assert(indicator_begins == 1 && indicator_ends == 1); /* fatal before Begin */
  fatal = false;
  puts("render preparation: PASS");
}
