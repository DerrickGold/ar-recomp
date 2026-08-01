#include "dev_tools.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "actraiser_game.h"
#include "actraiser_rtl.h"
#include "crt_post.h"
#include "diorama.h"
#include "diorama_scroll_math.h"
#include "frame_slot.h"
#include "host_display.h"
#include "music_replacements.h"
#include "present.h"
#include "run_dir.h"
#include "scene_asset_dump.h"
#include "scene_inspector.h"
#include "settings.h"
#include "snes/ppu.h"
#include "snes/snes.h"

enum {
  kArgbBytesPerPixel = 4,
  kHudScaleStepPercent = 25,
  kHudScaleMinimumPercent = 25,
  kHudScaleMaximumPercent = 400,
  kHudScaleDefaultPercent = 100,
  kPercentScale = 100,
  kHudObjectCaptureCount = 4,
};

static void EnsureDirectoryExists(const char *path) {
#ifdef _WIN32
  const int result = _mkdir(path);
#else
  const int result = mkdir(path, 0755);
#endif
  if (result != 0 && errno != EEXIST)
    fprintf(stderr, "[dev-tools] could not create %s: %s\n",
            path, strerror(errno));
}

static const char *InspectorSceneName(uint8 map_group, uint8 map) {
  static const char *const regions[] = {
    "Non-action", "Fillmore act", "Bloodpool act", "Kasandora act",
    "Aitos act", "Marahna act", "Northwall act", "Death Heim", "Ending",
  };
  static const char *const non_action[] = {
    "Title", "Fillmore sim", "Bloodpool sim", "Kasandora sim", "Aitos sim",
    "Marahna sim", "Northwall sim", "Sky Palace", "Temple", "World map",
  };
  if (map_group == kActRaiserMapGroup_NonAction &&
      map < sizeof(non_action) / sizeof(non_action[0]))
    return non_action[map];
  if (map_group < sizeof(regions) / sizeof(regions[0]))
    return regions[map_group];
  return "Unknown";
}

void DevTools_FormatInspectorInfo(const DevToolsContext *context,
                                  char *buffer, size_t buffer_size) {
  if (!context || !buffer || !buffer_size) return;
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 map = g_ram[kActRaiserWram_CurrentMap];
  char music[128];
  MusicReplacements_FormatPlaybackStatus(music, sizeof(music));
  snprintf(buffer, buffer_size,
           "SCENE %-11.11s $18/$19 $%02X/$%02X\n"
           "GF $%04X HOST %d P:%c T:%s\n"
           "CAM $%04X,$%04X MAP %uX%u\n"
           "PPU MODE %u MAIN $%02X SUB $%02X\n"
           "%s",
           InspectorSceneName(map_group, map), map_group, map,
           ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
           snes_frame_counter, context->paused ? 'Y' : 'N',
           context->turbo ? "ON" : "OFF",
           ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX),
           ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY),
           ActRaiser_ReadWram16(kActRaiserWram_Bg1Width),
           ActRaiser_ReadWram16(kActRaiserWram_Bg1Height),
           context->ppu ? PPU_mode(context->ppu) : 0,
           context->ppu ? context->ppu->screenEnabled[0] : 0,
           context->ppu ? context->ppu->screenEnabled[1] : 0,
           music);
}

bool DevTools_DumpSceneAssets(const DevToolsContext *context) {
  if (!context || !context->ppu) return false;
  static unsigned dump_number;
  const unsigned game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  char directory[320];
  RunDirFile(directory, sizeof(directory), "scene_assets_%02u_h%d_gf%u",
             dump_number++, snes_frame_counter, game_frame);
  return SceneAssetDump_Write(
      directory, context->ppu, g_ram, snes_frame_counter);
}

/* Write the live framebuffer to an open PPM, cropped to the active display
 * rectangle. When a renderer exists, capture the actual host composite so an
 * independently scaled HUD is represented exactly. */
SDL_Point DevTools_WriteFramebufferPpm(FILE *file,
                                       const DevToolsContext *context) {
  if (!file || !context)
    return (SDL_Point){0, 0};

  FrameSlot frame_slot;
  bool have_composite = false;
  SDL_Surface *argb = NULL;
  if (context->renderer && context->hud_bg_texture) {
    FrameSlot_Capture(&frame_slot);
    PresentUpload(&frame_slot);
    /* Captures go through the CRT post chain too, so a screenshot shows what
     * the player actually sees. Both calls are no-ops while CRT is off, which
     * keeps the pixel-exact A/B harness comparing like with like. */
    CrtPost_Begin(context->renderer);
    PresentComposite(&frame_slot, NULL, kInterpPhaseNone);
    CrtPost_End(context->renderer, frame_slot.snes_height,
                ComputePresentationViewport(
                    context->renderer, frame_slot.ws_active,
                    frame_slot.ignore_aspect_ratio, frame_slot.pixel_aspect,
                    frame_slot.visible_width, frame_slot.snes_height));
    have_composite = true;
  }
  if (have_composite) {
    /* SDL3 returns a newly allocated surface in the renderer's native format.
     * Convert it so byte extraction below is backend-independent. */
    SDL_Surface *raw = SDL_RenderReadPixels(context->renderer, NULL);
    argb = raw
        ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888)
        : NULL;
    if (raw) SDL_DestroySurface(raw);
  }
  if (argb) {
    const int output_width = argb->w;
    const int output_height = argb->h;
    fprintf(file, "P6\n%d %d\n255\n", output_width, output_height);
    for (int y = 0; y < output_height; y++) {
      const uint8_t *row =
          (const uint8_t *)argb->pixels + (size_t)y * argb->pitch;
      for (int x = 0; x < output_width; x++) {
        fputc(row[x * kArgbBytesPerPixel + 2], file);
        fputc(row[x * kArgbBytesPerPixel + 1], file);
        fputc(row[x * kArgbBytesPerPixel + 0], file);
      }
    }
    SDL_DestroySurface(argb);
    return (SDL_Point){output_width, output_height};
  }

  const int visible_x = Settings_VisibleX0();
  const int visible_width = Settings_VisibleWidth();
  fprintf(file, "P6\n%d %d\n255\n", visible_width, context->snes_height);
  for (int y = 0; y < context->snes_height; y++) {
    const uint8_t *row = context->framebuffer_pixels +
        ((size_t)y * context->snes_width + visible_x) * kArgbBytesPerPixel;
    for (int x = 0; x < visible_width; x++) {
      fputc(row[x * kArgbBytesPerPixel + 2], file);
      fputc(row[x * kArgbBytesPerPixel + 1], file);
      fputc(row[x * kArgbBytesPerPixel + 0], file);
    }
  }
  return (SDL_Point){visible_width, context->snes_height};
}

void DevTools_TakeFullSnapshot(const DevToolsContext *context) {
  if (!context) return;
  static int snapshot_number;
  char snapshot_directory[320];
  RunDirFile(snapshot_directory, sizeof(snapshot_directory), "snapshots");
  EnsureDirectoryExists(snapshot_directory);

  const unsigned game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  char prefix[336];
  RunDirFile(prefix, sizeof(prefix), "snapshots/snap_%02d_gf%u",
             snapshot_number++, game_frame);
  ActRaiser_FullSnapshot(prefix);

  char screenshot_path[344];
  snprintf(screenshot_path, sizeof(screenshot_path), "%s.ppm", prefix);
  FILE *screenshot = fopen(screenshot_path, "wb");
  if (screenshot) {
    (void)DevTools_WriteFramebufferPpm(screenshot, context);
    fclose(screenshot);
  }
  fprintf(stderr,
          "[snap] -> %s.{wram,vram,cgram,oam,ppm} (gf=%u)\n",
          prefix, game_frame);
}

static bool WritePngFromArgb(const char *path, const uint8_t *argb_pixels,
                             int width, int height) {
  const size_t row_bytes = (size_t)width * kArgbBytesPerPixel;
  uint8_t *rgba = malloc(row_bytes * (size_t)height);
  if (!rgba) return false;
  for (int y = 0; y < height; y++) {
    const uint8_t *source = argb_pixels + (size_t)y * row_bytes;
    uint8_t *destination = rgba + (size_t)y * row_bytes;
    for (int x = 0; x < width; x++) {
      destination[0] = source[2];
      destination[1] = source[1];
      destination[2] = source[0];
      destination[3] = source[3];
      source += kArgbBytesPerPixel;
      destination += kArgbBytesPerPixel;
    }
  }
  const bool succeeded = WritePng(path, rgba, width, height);
  free(rgba);
  return succeeded;
}

void DevTools_DumpDioramaLayers(const DevToolsContext *context) {
  if (!context || !context->diorama_layer_pixels) return;
  const unsigned game_frame =
      ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  char directory[320];
  RunDirFile(directory, sizeof(directory), "diorama_dump");
  EnsureDirectoryExists(directory);

  /* Primary slots contain each layer's priority-0 remainder after the band
   * splits are bound; the appended slots contain the higher priorities. */
  static const struct {
    int source;
    const char *name;
  } layers[] = {
    {kPpuOverlaySource_Bg1, "bg1"},
    {kDioramaPlane_Bg1Hi, "bg1_hi"},
    {kPpuOverlaySource_Bg2, "bg2"},
    {kDioramaPlane_Bg2Hi, "bg2_hi"},
    {kPpuOverlaySource_Bg3, "bg3"},
    {kPpuOverlaySource_Obj, "obj_p0"},
    {kDioramaPlane_Obj1, "obj_p1"},
    {kDioramaPlane_Obj2, "obj_p2"},
    {kDioramaPlane_Obj3, "obj_p3"},
  };
  int dumped_count = 0;
  for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
    const uint8_t *pixels = context->diorama_layer_pixels[layers[i].source];
    if (!pixels) continue;
    char path[344];
    snprintf(path, sizeof(path), "%s/%s_gf%u.png",
             directory, layers[i].name, game_frame);
    if (WritePngFromArgb(
            path, pixels, context->snes_width, kActRaiserAuthenticHeight))
      dumped_count++;
  }

  char backdrop_path[344];
  snprintf(backdrop_path, sizeof(backdrop_path), "%s/backdrop_gf%u.png",
           directory, game_frame);
  if (WritePngFromArgb(backdrop_path, context->framebuffer_pixels,
                       context->snes_width, kActRaiserAuthenticHeight))
    dumped_count++;
  fprintf(stderr,
          "[diorama] dumped %d layer PNGs to %s/ (gf=%u, w=%d)\n",
          dumped_count, directory, game_frame, context->snes_width);
}

void DevTools_AdjustHudOutputScale(const DevToolsContext *context,
                                   int delta_percent) {
  if (!context) return;
  const SettingDesc *descriptor = Settings_Find("hud_scale_percent");
  if (!descriptor) return;

  int current_percent = g_settings.hud_scale_percent;
  if (!current_percent && context->renderer) {
    const SDL_Rect viewport = ComputePresentationViewport(
        context->renderer, context->widescreen_active,
        context->ignore_aspect_ratio, context->pixel_aspect,
        Settings_VisibleWidth(), context->snes_height);
    current_percent =
        (viewport.h * kPercentScale + context->snes_height / 2) /
        context->snes_height;
    current_percent =
        ((current_percent + kHudScaleStepPercent / 2) /
         kHudScaleStepPercent) * kHudScaleStepPercent;
  }
  if (!current_percent) current_percent = kHudScaleDefaultPercent;

  int next_percent = current_percent + delta_percent;
  if (next_percent < kHudScaleMinimumPercent)
    next_percent = kHudScaleMinimumPercent;
  if (next_percent > kHudScaleMaximumPercent)
    next_percent = kHudScaleMaximumPercent;

  const SettingChangeResult result =
      Settings_SetLong(descriptor, next_percent);
  char formatted[32];
  Settings_FormatValue(descriptor, formatted, sizeof(formatted));
  fprintf(stderr, "[hud-overlay] scale -> %s (%s; 1.00x = native output)\n",
          formatted, Settings_ChangeResultName(result));
}

static bool PointInRect(int x, int y, SDL_Rect rectangle) {
  return x >= rectangle.x && x < rectangle.x + rectangle.w &&
         y >= rectangle.y && y < rectangle.y + rectangle.h;
}

static bool HudChunkPixelVisible(const DevToolsContext *context,
                                 const HudPresentationChunk *chunk,
                                 int source_x, int source_y) {
  const uint8_t *pixels =
      chunk->inspector_kind == kInspectorPresentation_HudObj
          ? context->hud_obj_pixels
          : context->hud_bg_pixels;
  const int texture_x =
      source_x + (context->snes_width - kPpuXPixels) / 2;
  if (!pixels || texture_x < 0 || texture_x >= context->snes_width ||
      source_y < 0 || source_y >= context->snes_height)
    return false;
  return pixels[
      ((size_t)source_y * context->snes_width + texture_x) *
      kArgbBytesPerPixel + 3] != 0;
}

static void FillLiveHudProjectionInputs(const DevToolsContext *context,
                                        HudProjectionInputs *inputs) {
  memset(inputs, 0, sizeof(*inputs));
  inputs->hud_bg_texture = context->hud_bg_texture;
  inputs->hud_obj_texture = context->hud_obj_texture;
  inputs->hud_scale_percent =
      Settings_ScalePercentToOutput(g_settings.hud_scale_percent);
  inputs->pixel_aspect = context->pixel_aspect;
  inputs->snes_width = context->snes_width;
  inputs->snes_height = context->snes_height;
  inputs->visible_width = Settings_VisibleWidth();
  if (!context->ppu) return;

  inputs->hud_split_height = context->ppu->wsHudSplitHeight;
  inputs->hud_left_end = context->ppu->wsHudLeftEnd;
  inputs->hud_right_start = context->ppu->wsHudRightStart;
  inputs->hud_player_row_y = context->ppu->wsHudPlayerRowY;
  inputs->hud_left_only_y = context->ppu->wsHudLeftOnlyY;
  inputs->extra_left_right = context->ppu->extraLeftRight;
  const PpuOverlayCapture *bg3_capture =
      &context->ppu->overlayCaptures[kPpuOverlaySource_Bg3];
  if (bg3_capture->y1 > (int16_t)inputs->hud_split_height &&
      bg3_capture->y1 <= kHostDisplayFramebufferHeight)
    inputs->hud_body_y1 = (uint8_t)bg3_capture->y1;

  const PpuOverlayCapture *obj_capture =
      &context->ppu->overlayCaptures[kPpuOverlaySource_Obj];
  if (obj_capture->oamCount == kHudObjectCaptureCount) {
    const int first = obj_capture->oamFirst;
    inputs->obj_icon_x = (context->ppu->oam[first * 2] & 0xff) |
        ((context->ppu->highOam[first >> 2] >> ((first & 3) * 2)) & 1) << 8;
    inputs->obj_icon_y = context->ppu->oam[first * 2] >> 8;
    inputs->obj_icon_valid = true;
  }
}

bool DevTools_InspectWindowPoint(const DevToolsContext *context,
                                 int window_x, int window_y) {
  if (!context || !context->inspector_presentation) return false;

  int output_x = 0;
  int output_y = 0;
  if (!HostDisplay_WindowPointToOutput(
          window_x, window_y, &output_x, &output_y))
    return false;

  const SDL_Rect viewport = ComputePresentationViewport(
      context->renderer, context->widescreen_active,
      context->ignore_aspect_ratio, context->pixel_aspect,
      Settings_VisibleWidth(), context->snes_height);
  int output_width = 0;
  int output_height = 0;
  SDL_GetRenderOutputSize(
      context->renderer, &output_width, &output_height);

  HudProjectionInputs hud_inputs;
  FillLiveHudProjectionInputs(context, &hud_inputs);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  const int chunk_count =
      BuildHudPresentationChunks(viewport, &hud_inputs, chunks);
  bool selected = false;
  for (int i = chunk_count - 1; i >= 0 && !selected; i--) {
    const HudPresentationChunk *chunk = &chunks[i];
    if (!PointInRect(output_x, output_y, chunk->output_destination))
      continue;

    const double source_x = chunk->screen_source.x +
        (double)(output_x - chunk->output_destination.x) *
        chunk->screen_source.w / chunk->output_destination.w;
    const double source_y = chunk->screen_source.y +
        (double)(output_y - chunk->output_destination.y) *
        chunk->screen_source.h / chunk->output_destination.h;
    const int sample_x = (int)source_x;
    const int sample_y = (int)source_y;
    if (!HudChunkPixelVisible(context, chunk, sample_x, sample_y)) continue;

    const int inspector_x = sample_x + chunk->inspector_x_bias;
    const unsigned background_mask =
        chunk->inspector_kind == kInspectorPresentation_HudBg
            ? kSceneInspectorBg3
            : 0;
    const bool inspect_objects =
        chunk->inspector_kind == kInspectorPresentation_HudObj;
    if (!SceneInspector_SelectFiltered(
            inspector_x, sample_y, background_mask, inspect_objects))
      continue;

    *context->inspector_presentation = (InspectorPresentationSelection){
      chunk->inspector_kind, source_x, source_y,
      output_x, output_y, output_width, output_height,
    };
    fprintf(stderr,
            "[scene-inspector-hit] event=%d,%d output=%d,%d target=%s "
            "source=%.3f,%.3f dst=%d,%d,%d,%d\n",
            window_x, window_y, output_x, output_y,
            chunk->inspector_kind == kInspectorPresentation_HudBg
                ? "hud-bg3"
                : "hud-obj",
            source_x, source_y,
            chunk->output_destination.x,
            chunk->output_destination.y,
            chunk->output_destination.w,
            chunk->output_destination.h);
    selected = true;
  }

  if (selected) return true;
  if (!PointInRect(output_x, output_y, viewport)) return false;

  const int visible_left =
      Settings_VisibleX0() - context->widescreen_extra;
  const double screen_position_x = visible_left +
      (double)(output_x - viewport.x) * Settings_VisibleWidth() / viewport.w;
  const double screen_position_y =
      (double)(output_y - viewport.y) * context->snes_height / viewport.h;
  const int screen_x = visible_left +
      (int)((double)(output_x - viewport.x) * Settings_VisibleWidth() /
            viewport.w);
  const int screen_y =
      (int)((double)(output_y - viewport.y) * context->snes_height /
            viewport.h);
  if (!SceneInspector_Select(screen_x, screen_y)) return false;

  *context->inspector_presentation = (InspectorPresentationSelection){
    kInspectorPresentation_Base, screen_position_x, screen_position_y,
    output_x, output_y, output_width, output_height,
  };
  fprintf(stderr,
          "[scene-inspector-hit] event=%d,%d output=%d,%d target=base "
          "screen=%.3f,%.3f viewport=%d,%d,%d,%d\n",
          window_x, window_y, output_x, output_y,
          screen_position_x, screen_position_y,
          viewport.x, viewport.y, viewport.w, viewport.h);
  return true;
}
