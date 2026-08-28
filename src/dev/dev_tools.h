#ifndef DEV_TOOLS_H
#define DEV_TOOLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "present.h"
#include "snesrecomp/runner.h"

/* Live host-owned resources needed by diagnostic capture and inspector tools.
 * Keep this value snapshot explicit: these resources are recreated when video
 * geometry or the render device changes, so retaining a context is invalid. */
typedef struct DevToolsContext {
  SDL_Renderer *renderer;
  ArRenderTexture hud_bg_texture;
  ArRenderTexture hud_obj_texture;
  SrRunnerHandle *runner;
  SrPpuStateSnapshot ppu_state;
  SrPpuFrameSnapshot ppu_frame;
  SrBorrowedU16Span oam;
  SrBorrowedSpan high_oam;
  bool ppu_snapshot_valid;
  /* Points at the AUTHENTIC frame's first column: g_pixels is bound apron-wide
   * (it doubles as the diorama backdrop plane), so the host offsets past the
   * apron. framebuffer_pitch is the real row stride, which is therefore wider
   * than snes_width * 4. */
  const uint8_t *framebuffer_pixels;
  int framebuffer_pitch;
  /* Resolve apron per side on the captured layer surfaces (see FrameSlot). */
  int obj_apron;
  const uint8_t *hud_bg_pixels;
  const uint8_t *hud_obj_pixels;
  uint8_t *const *diorama_layer_pixels;
  InspectorPresentationSelection *inspector_presentation;
  int snes_width;
  int snes_height;
  int pixel_aspect;
  int widescreen_extra;
  bool widescreen_active;
  bool ignore_aspect_ratio;
  bool paused;
  bool turbo;
} DevToolsContext;

void DevTools_FormatInspectorInfo(const DevToolsContext *context,
                                  char *buffer, size_t buffer_size);
bool DevTools_DumpSceneAssets(const DevToolsContext *context);
SDL_Point DevTools_WriteFramebufferPpm(FILE *file,
                                       const DevToolsContext *context);
void DevTools_TakeFullSnapshot(const DevToolsContext *context);
void DevTools_DumpDioramaLayers(const DevToolsContext *context);
void DevTools_AdjustHudOutputScale(const DevToolsContext *context,
                                   int delta_percent);
bool DevTools_InspectWindowPoint(const DevToolsContext *context,
                                 int window_x, int window_y);

#endif /* DEV_TOOLS_H */
