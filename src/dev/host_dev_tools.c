#include "action/action_obj_apron.h"
#include "host_dev_tools.h"

#include "dev_tools.h"
#include "host/host_input.h"
#include "present.h"
#include "scene_inspector.h"
#include "settings.h"
#include "snes/ppu.h"

extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern Ppu *g_ppu;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
extern InspectorPresentationSelection g_scene_inspector_presentation;
extern int g_snes_width;
extern int g_snes_height;
extern int g_active_pixel_aspect;
extern int g_ws_extra;
extern bool g_ws_active;

static DevToolsContext CurrentContext(void) {
  return (DevToolsContext){
    .renderer = g_renderer,
    .hud_bg_texture = g_hud_bg_texture,
    .hud_obj_texture = g_hud_obj_texture,
    .ppu = g_ppu,
    .framebuffer_pixels = g_pixels + ActionApron_DisplayOffset(kPpuObjApron),
    .framebuffer_pitch =
        (g_snes_width + kPpuObjApron * 2) * 4,
    .obj_apron = kPpuObjApron,
    .hud_bg_pixels = g_hud_bg_pixels,
    .hud_obj_pixels = g_hud_obj_pixels,
    .diorama_layer_pixels = g_diorama_layer_pixels,
    .inspector_presentation = &g_scene_inspector_presentation,
    .snes_width = g_snes_width,
    .snes_height = g_snes_height,
    .pixel_aspect = g_active_pixel_aspect,
    .widescreen_extra = g_ws_extra,
    .widescreen_active = g_ws_active,
    .ignore_aspect_ratio = Settings_IgnoreAspectRatio(),
    .paused = HostInput_IsPaused(),
    .turbo = HostInput_IsTurbo(),
  };
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

SDL_Point HostDevTools_WriteFramebufferPpm(FILE *file) {
  const DevToolsContext context = CurrentContext();
  return DevTools_WriteFramebufferPpm(file, &context);
}
