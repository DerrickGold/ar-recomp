#include "action/action_obj_apron.h"
#include "host_dev_tools.h"

#include <string.h>

#include "dev_tools.h"
#include "present/display_geometry.h"
#include "host/host_display.h"
#include "snesrecomp/game_runtime.h"
#include "host/host_input.h"
#include "platform/sdl/dev_tools_readback_sdl.h"
#include "present/present.h"
#include "snesrecomp/runner.h"
#include "scene_inspector.h"
#include "app/settings.h"
#include "host/host_video.h"
#include "present/presentation_textures.h"
#include "host/host_frame_surfaces.h"

/* What the renderer highlights for the inspector selection. The developer
 * tools write it through their context; FrameSlot_Capture copies it.
 * InspectorPresentationKind comes from the portable HUD-layout contract;
 * InspectorPresentationSelection lives in present.h. Both are shared by the
 * live hit-test and the FrameSlot-fed renderer. */
static InspectorPresentationSelection s_inspector_presentation;
bool g_diorama_dump_pending;

const InspectorPresentationSelection *HostDevTools_InspectorPresentation(void) {
  return &s_inspector_presentation;
}

void HostDevTools_ClearInspectorPresentation(void) {
  memset(&s_inspector_presentation, 0, sizeof(s_inspector_presentation));
}

static DevToolsContext CurrentContext(void) {
  DevToolsContext context = {
    .readback = {
      .capture_rgb24 = ArSdlDevTools_CaptureRgb24,
      .context = &g_render_device,
    },
    .render_device = &g_render_device,
    .hud_bg_texture = g_hud_bg_texture,
    .hud_obj_texture = g_hud_obj_texture,
    .runner = RtlGameRunner(),
    .framebuffer_pixels =
        g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON),
    .framebuffer_pitch =
        (g_snes_width + (int)SR_PPU_OBJ_APRON * 2) * 4,
    .obj_apron = SR_PPU_OBJ_APRON,
    .hud_bg_pixels = g_hud_bg_pixels,
    .hud_obj_pixels = g_hud_obj_pixels,
    .diorama_layer_pixels = g_diorama_layer_pixels,
    .inspector_presentation = &s_inspector_presentation,
    .snes_width = g_snes_width,
    .snes_height = g_snes_height,
    .pixel_aspect = g_active_pixel_aspect,
    .widescreen_extra = g_ws_extra,
    .widescreen_active = g_ws_active,
    .ignore_aspect_ratio = Settings_IgnoreAspectRatio(),
    .paused = HostInput_IsPaused(),
    .turbo = HostInput_IsTurbo(),
  };
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (!api || !context.runner ||
      api->struct_size < SNES_RUNNER_API_PPU_FRAME_STATE_SIZE ||
      (api->capabilities &
       (SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_BORROWED_U16_SPANS)) !=
          (SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_PPU_FRAME_STATE |
           SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
           SR_RUNNER_CAP_BORROWED_U16_SPANS))
    return context;

  context.ppu_state.struct_size = sizeof(context.ppu_state);
  context.ppu_frame.struct_size = sizeof(context.ppu_frame);
  context.oam.struct_size = sizeof(context.oam);
  context.high_oam.struct_size = sizeof(context.high_oam);
  if (api->query_ppu_state(context.runner, &context.ppu_state) !=
          SR_RESULT_OK ||
      api->query_ppu_frame_state(context.runner, &context.ppu_frame) !=
          SR_RESULT_OK ||
      api->borrow_u16_memory(context.runner, SR_MEMORY_OAM, &context.oam) !=
          SR_RESULT_OK ||
      api->borrow_memory(context.runner, SR_MEMORY_HIGH_OAM,
                         &context.high_oam) != SR_RESULT_OK)
    return context;

  const uint64_t generation = context.ppu_state.lifetime_generation;
  context.ppu_snapshot_valid =
      context.ppu_frame.lifetime_generation == generation &&
      context.oam.lifetime_generation == generation &&
      context.high_oam.lifetime_generation == generation;
  return context;
}

void HostDevTools_FormatInspectorInfo(char *buffer, size_t buffer_size) {
  const DevToolsContext context = CurrentContext();
  DevTools_FormatInspectorInfo(&context, buffer, buffer_size);
}

bool HostDevTools_DumpSceneAssets(void) {
  HostInput_RedrawPausedFrameIfNeeded();
  const DevToolsContext context = CurrentContext();
  return DevTools_DumpSceneAssets(&context);
}

void HostDevTools_TakeFullSnapshot(void) {
  HostInput_RedrawPausedFrameIfNeeded();
  const DevToolsContext context = CurrentContext();
  DevTools_TakeFullSnapshot(&context);
}

void HostDevTools_AdjustHudOutputScale(int delta_percent) {
  const DevToolsContext context = CurrentContext();
  DevTools_AdjustHudOutputScale(&context, delta_percent);
}

bool HostDevTools_InspectWindowPoint(int window_x, int window_y) {
  const bool had_selection = SceneInspector_HasSelection();
  const DevToolsContext context = CurrentContext();
  if (!DevTools_InspectWindowPoint(&context, window_x, window_y)) return false;
  HostInput_OnInspectorSelection(had_selection);
  return true;
}

void HostDevTools_DumpDioramaLayers(void) {
  const DevToolsContext context = CurrentContext();
  DevTools_DumpDioramaLayers(&context);
}

DevToolsCaptureResult HostDevTools_WriteFramebufferPpm(FILE *file, bool require_composite) {
  const DevToolsContext context = CurrentContext();
  return DevTools_WriteFramebufferPpm(file, &context, require_composite);
}
