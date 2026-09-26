#include "host/host_ppu_output.h"

#include <assert.h>
#include <stdio.h>
#include "action/action_obj_apron.h"
#include "host/host_frame_surfaces.h"
#include "present/display_geometry.h"
#include "render/present_hud.h"
#include "replacements/hd_replacement_host.h"
#include "snesrecomp/game_runtime.h"

int g_snes_width = 320, g_snes_height = 224;
static ActRaiserDisplayGeometry geometry = {.widescreen_active = true, .render_extra = 32};
const ActRaiserDisplayGeometry *const g_actraiser_display_geometry = &geometry;
static SrRunnerHandle *const runner = (SrRunnerHandle *)(uintptr_t)1;
static bool runner_available = true, reject_authentic;
static SrPpuOutputBindingRequest bindings[64];
static int bind_count, generation_queries, feature_index, margin_index;
static SrPpuHorizontalMarginRequest margin;
static uint64_t generation = 42;

SrRunnerHandle *RtlGameRunner(void) { return runner_available ? runner : NULL; }
static SrResult Generations(SrRunnerHandle *handle, SrGenerationSnapshot *snapshot) {
  assert(handle == runner);
  snapshot->lifetime_generation = generation;
  generation_queries++;
  return SR_RESULT_OK;
}
static SrResult Bind(SrRunnerHandle *handle, const SrPpuOutputBindingRequest *request) {
  assert(handle == runner && request->lifetime_generation == generation);
  assert(request->struct_size == sizeof(*request) && bind_count < 64);
  bindings[bind_count++] = *request;
  return reject_authentic && request->kind == SR_PPU_OUTPUT_AUTHENTIC && request->pixels
      ? SR_RESULT_INVALID_ARGUMENT : SR_RESULT_OK;
}
static SrResult Margin(SrRunnerHandle *handle, const SrPpuHorizontalMarginRequest *request) {
  assert(handle == runner && request->lifetime_generation == generation);
  margin = *request;
  margin_index = bind_count;
  return SR_RESULT_OK;
}
static SnesRunnerApi api = {
  .struct_size = sizeof(api), .capabilities = SR_RUNNER_CAP_PPU_OUTPUT_CONTROL,
  .query_generations = Generations, .bind_ppu_output_surface = Bind,
  .configure_ppu_horizontal_margin = Margin,
};
const SnesRunnerApi *sr_runner_get_api(uint32_t version) {
  assert(version == SR_RUNNER_ABI_VERSION);
  return &api;
}
ArRenderTexture PresentHud_BackgroundTexture(void) { return (ArRenderTexture){1}; }
ArRenderTexture PresentHud_ObjectTexture(void) { return ArRenderTexture_Invalid(); }
void HdReplacementHost_RebindSurfaces(const HostPpuOutputControl *control) {
  assert(control->runner == runner && control->lifetime_generation == generation);
  feature_index = bind_count;
}

int main(void) {
  HostPpuOutput_Reset();
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(!HostPpuOutput_AuthenticFrameSerial());
  HostPpuOutput_SetAuthenticEnabled(true);
  assert(HostPpuOutput_AuthenticEnabled() && bind_count == 1);
  assert(bindings[0].pixels == g_authentic_pixels && bindings[0].pitch_bytes == 320 * 4);
  HostPpuOutput_SetAuthenticEnabled(true);
  assert(bind_count == 1 && generation_queries == 1); /* Already bound: no redundant mutation. */
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(HostPpuOutput_AuthenticFrameSerial() == 1);
  HostPpuOutput_AuthenticFrameCompleted(false);
  assert(!HostPpuOutput_AuthenticFrameSerial());
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(HostPpuOutput_AuthenticFrameSerial() == 2);

  bind_count = 0;
  HostPpuOutput_Rebind();
  assert(bind_count == 6 && feature_index == 5 && margin_index == 5);
  assert(generation_queries == 2); /* One lifetime snapshot covers the whole batch. */
  assert(bindings[0].kind == SR_PPU_OUTPUT_MAIN && bindings[0].pixels == g_pixels);
  assert(bindings[0].pitch_bytes == ActionApron_SurfacePitch(320, SR_PPU_OBJ_APRON));
  assert(bindings[1].kind == SR_PPU_OUTPUT_AUTHENTIC && !bindings[1].pixels);
  assert(bindings[2].kind == SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES);
  assert(bindings[3].source == SR_PPU_OVERLAY_BG3 && bindings[3].pixels == g_hud_bg_pixels);
  assert(bindings[4].source == SR_PPU_OVERLAY_OBJ && !bindings[4].pixel_byte_size);
  assert(bindings[5].kind == SR_PPU_OUTPUT_AUTHENTIC && bindings[5].pixels);
  assert(margin.mode == SR_PPU_HORIZONTAL_MARGIN_CENTERED && margin.budget_pixels == 32);
  assert(!HostPpuOutput_AuthenticFrameSerial());
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(HostPpuOutput_AuthenticFrameSerial() == 3);

  /* Contracting geometry must discard the old capture before publishing a new pass. */
  g_snes_width = 256;
  geometry.widescreen_active = false;
  generation++;
  bind_count = 0;
  HostPpuOutput_Rebind();
  assert(bindings[5].pitch_bytes == 256 * 4 && !HostPpuOutput_AuthenticFrameSerial());
  assert(margin.mode == SR_PPU_HORIZONTAL_MARGIN_AVAILABLE && !margin.budget_pixels);
  reject_authentic = true;
  HostPpuOutput_Rebind();
  assert(!HostPpuOutput_AuthenticEnabled());
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(!HostPpuOutput_AuthenticFrameSerial());
  bind_count = 0;
  HostPpuOutput_SetAuthenticEnabled(true);
  assert(bind_count == 2 && bindings[0].pixels && !bindings[1].pixels);
  assert(!HostPpuOutput_AuthenticEnabled()); /* A rejected enable actively detaches old data. */

  reject_authentic = false;
  runner_available = false;
  bind_count = 0;
  HostPpuOutput_SetAuthenticEnabled(true);
  HostPpuOutput_AuthenticFrameCompleted(true);
  assert(!HostPpuOutput_AuthenticFrameSerial() && !bind_count);
  runner_available = true;
  HostPpuOutput_SetAuthenticEnabled(true);
  assert(bind_count == 1); /* A pending demand retries once the runner is available. */
  HostPpuOutput_SetAuthenticEnabled(false);
  assert(!HostPpuOutput_AuthenticEnabled() && !bindings[1].pixels);
  HostPpuOutputControl control;
  api.capabilities = 0;
  assert(!HostPpuOutputControl_Begin(&control));
  api.capabilities = SR_RUNNER_CAP_PPU_OUTPUT_CONTROL;
  api.struct_size = SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE - 1;
  assert(!HostPpuOutputControl_Begin(&control));
  HostPpuOutput_Reset();
  puts("PPU output owner: binding order, lifetimes, geometry and authentic capture passed");
  return 0;
}
