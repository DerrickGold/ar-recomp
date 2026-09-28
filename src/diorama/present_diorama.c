#include "diorama/present_diorama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "action/action_effect_projection.h"
#include "action/present_action_effects.h"
#include "app/session_fatal.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_upload.h"
#include "host/host_clock.h"
#include "present/present.h"
#include "present/presentation_surface.h"
#include "present/presentation_upload_mirror.h"
#include "render/present_hud.h"

static ArRenderTexture s_plane_textures[kDioramaPlane_Count];
static uint32_t s_diorama_uploaded_plane_mask;
static DioramaCoverageMask s_diorama_coverage_masks[kDioramaPlane_Count];
static uint64_t s_diorama_bg2_content_revision;
static DioramaSkyboxView s_diorama_skybox_view;
static ArRenderTexture s_diorama_skybox_texture;
static PresentationUploadMirror s_diorama_skybox_mirror;

/* Session history survives retained frames, room changes and GPU resets. */
static DioramaCameraPresenter s_diorama_camera = DIORAMA_CAMERA_PRESENTER_INIT;

void PresentDiorama_DestroyPlanes(ArRenderDevice *device) {
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(device, s_plane_textures[i]);
    s_plane_textures[i] = ArRenderTexture_Invalid();
  }
}

void PresentDiorama_CreatePlanes(ArRenderDevice *device) {
  /* The ABI's full render-target size covers every horizontal and vertical
   * margin without a realloc. Only the leading snes_width x
   * (snes_height + ws_extra_top + ws_extra_bottom) region is uploaded
   * each frame; Diorama_Composite's UV window is expressed against these
   * allocated dimensions. Zero the padding because the tilted view can
   * sample outside the captured rectangle. */
  uint8_t *zero_fill =
      calloc(1, (size_t)SR_PPU_SURFACE_MAX_WIDTH *
                    SR_PPU_SURFACE_MAX_HEIGHT * 4);
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    if (i == SR_PPU_OVERLAY_BG4)
      continue;
    const ArRenderTextureDesc desc = {
      .width = SR_PPU_SURFACE_MAX_WIDTH,
      .height = SR_PPU_SURFACE_MAX_HEIGHT,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = i == kDioramaPlane_Backdrop
          ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(
            device, &desc, &s_plane_textures[i]))
      continue;
    if (zero_fill)
      ArRenderDevice_UpdateTexture(
          device, s_plane_textures[i], NULL, zero_fill,
          SR_PPU_SURFACE_MAX_WIDTH * 4);
  }
  free(zero_fill);
}

static const SrPpuSurfaceView *DioramaPpuSurface(
    const FrameSlot *slot, int plane) {
  if (!slot) return NULL;
  if (plane >= SR_PPU_OVERLAY_BG1 && plane <= SR_PPU_OVERLAY_OBJ)
    return &slot->ppu_surfaces.overlays[plane][0];
  if (plane == kDioramaPlane_Backdrop)
    return &slot->ppu_surfaces.main;
  size_t band_count;
  const DioramaPriorityBand *bands = DioramaPlanes_PriorityBands(&band_count);
  for (size_t i = 0; i < band_count; i++)
    if (bands[i].plane == plane)
      return &slot->ppu_surfaces.overlays[bands[i].source][bands[i].band];
  return NULL;
}

static void CaptureDioramaPpuSurfaces(
    const FrameSlot *slot,
    const uint8_t *pixels[kDioramaPlane_Count],
    size_t pitch_bytes[kDioramaPlane_Count]) {
  const int width = slot->snes_width + slot->obj_apron * 2;
  const int height = slot->snes_height +
      slot->ws_extra_top + slot->ws_extra_bottom;
  memset(pixels, 0, sizeof(*pixels) * kDioramaPlane_Count);
  if (pitch_bytes)
    memset(pitch_bytes, 0, sizeof(*pitch_bytes) * kDioramaPlane_Count);
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    const SrPpuSurfaceView *surface = DioramaPpuSurface(slot, plane);
    if (!PresentationSurface_Holds(surface, width, height)) continue;
    pixels[plane] = surface->data;
    if (pitch_bytes)
      pitch_bytes[plane] = (size_t)surface->pitch_bytes;
  }
}

/* AR_PLANESTAT=1: report the alpha-bearing fraction and content bounding box
 * of each plane that actually synchronized for presentation. This runs while
 * the borrowed producer surfaces are still owned by PresentUpload; retained
 * re-presents must not rescan pointers that a later game tick can rewrite. */
static void PlaneStatCensus(
    const uint8_t *pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    int width, int height, uint32_t plane_mask) {
  static int enabled = -1;
  static unsigned long frames;
  static double covered_sum[kDioramaPlane_Count];
  static double bbox_sum[kDioramaPlane_Count];
  static unsigned long present_count[kDioramaPlane_Count];
  if (enabled < 0) {
    const char *value = getenv("AR_PLANESTAT");
    enabled = value && value[0] && value[0] != '0';
  }
  if (!enabled || width <= 0 || height <= 0) return;
  frames++;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (!(plane_mask & (1u << plane)) ||
        !pixels[plane] || !pitch_bytes[plane])
      continue;
    long covered = 0;
    int x0 = width, x1 = -1, y0 = height, y1 = -1;
    for (int y = 0; y < height; y++) {
      const uint32_t *row = (const uint32_t *)(
          pixels[plane] + (size_t)y * pitch_bytes[plane]);
      for (int x = 0; x < width; x++) {
        if ((row[x] >> 24) == 0u) continue;
        covered++;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
    present_count[plane]++;
    const double area = (double)width * (double)height;
    covered_sum[plane] += (double)covered / area;
    bbox_sum[plane] += x1 < 0 ? 0.0
        : (double)(x1 - x0 + 1) * (double)(y1 - y0 + 1) / area;
  }
  if (frames % 300u != 0u) return;
  fprintf(stderr, "[planestat] after %lu frames (%dx%d)\n",
          frames, width, height);
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (!present_count[plane]) continue;
    fprintf(stderr,
            "  plane %2d: present %5.1f%% covered %5.1f%% bbox %5.1f%%\n",
            plane, 100.0 * (double)present_count[plane] / (double)frames,
            100.0 * covered_sum[plane] / (double)present_count[plane],
            100.0 * bbox_sum[plane] / (double)present_count[plane]);
  }
}

void PresentDiorama_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  s_diorama_skybox_view.texture = ArRenderTexture_Invalid();
  if (!ArRenderDevice_IsReady(device)) return;
  if (!slot->diorama_active) {
    s_diorama_uploaded_plane_mask = 0;
    memset(s_diorama_coverage_masks, 0, sizeof(s_diorama_coverage_masks));
    return;
  }
  bool skybox_changed = false;
  const SrPpuSurfaceView *skybox =
      PresentationSurface_Bound(&slot->diorama_skybox_surface);
  const uint8_t *skybox_pixels = PresentationSurface_Region(skybox, 0, 0,
      skybox ? (int)skybox->width_pixels : 0,
      skybox ? (int)skybox->height_pixels : 0);
  if (skybox_pixels) {
    if (!ArRenderTexture_IsValid(s_diorama_skybox_texture)) {
      const ArRenderTextureDesc desc = {
        .width = kFrameSlotLayerTextureWidth,
        .height = kFrameSlotLayerTextureHeight,
        .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming,
        .filter = kArRenderFilter_Linear, .blend = kArRenderBlendMode_Opaque,
      };
      (void)ArRenderDevice_CreateTexture(
          device, &desc, &s_diorama_skybox_texture);
    }
    /* The blur prefilter visits the fixed allocation, not just the view.
     * Initialize its padding, including after a capture extent shrinks. */
    if (ArRenderTexture_IsValid(s_diorama_skybox_texture) &&
        (!s_diorama_skybox_mirror.valid ||
         s_diorama_skybox_mirror.width != (int)skybox->width_pixels ||
         s_diorama_skybox_mirror.height != (int)skybox->height_pixels)) {
      uint32_t *empty = calloc(kFrameSlotLayerTextureWidth *
          kFrameSlotLayerTextureHeight, sizeof(uint32_t));
      const bool cleared = empty && ArRenderDevice_UpdateTexture(
          device, s_diorama_skybox_texture, NULL, empty,
          kFrameSlotLayerTextureWidth * sizeof(uint32_t));
      free(empty);
      if (!cleared) {
        ArRenderDevice_DestroyTexture(device, s_diorama_skybox_texture);
        s_diorama_skybox_texture = ArRenderTexture_Invalid();
      }
      PresentationUploadMirror_Reset(&s_diorama_skybox_mirror);
    }
    PresentationUploadResult result;
    if (ArRenderTexture_IsValid(s_diorama_skybox_texture) &&
        PresentationUploadMirror_UploadArgb8888(
            &s_diorama_skybox_mirror, device,
            s_diorama_skybox_texture, skybox_pixels,
            (int)skybox->width_pixels, (int)skybox->height_pixels,
            (int)skybox->pitch_bytes, 0, 0, &result)) {
      skybox_changed = result.changed;
      if (result.changed) s_diorama_skybox_view.revision++;
      s_diorama_skybox_view.texture = s_diorama_skybox_texture;
      s_diorama_skybox_view.width = (int)skybox->width_pixels;
    }
  }
  const uint8_t *pixels[kDioramaPlane_Count];
  size_t pitch_bytes[kDioramaPlane_Count];
  CaptureDioramaPpuSurfaces(slot, pixels, pitch_bytes);
  uint32_t upload_mask = slot->diorama_plane_request_mask &
                         slot->diorama_plane_content_mask;
  /* Row 0 is the top of the captured world band. Upload both sides; the
   * authentic frame begins at ws_extra_top and the lower band follows it. */
  const DioramaUploadResult upload = Diorama_Upload(
      device, s_plane_textures, pixels, pitch_bytes,
      slot->snes_width + slot->obj_apron * 2,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      slot->obj_apron, upload_mask);
  s_diorama_uploaded_plane_mask = upload.synchronized_plane_mask;
  if (upload.changed_plane_mask &
      (UINT32_C(1) << SR_PPU_OVERLAY_BG2))
    s_diorama_bg2_content_revision++;
  memcpy(s_diorama_coverage_masks, upload.coverage_masks,
         sizeof(s_diorama_coverage_masks));
  PlaneStatCensus(
      pixels, pitch_bytes,
      slot->snes_width + slot->obj_apron * 2,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      s_diorama_uploaded_plane_mask);
  /* A failed raw upload cannot be a valid endpoint: exclude it before
   * retaining/analyzing the pair so generation never interpolates from an
   * image that was not actually presentable. */
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    if (!(s_diorama_uploaded_plane_mask & (1u << plane)))
      pixels[plane] = NULL;
  DioramaPerformanceScope frame_analysis =
      DioramaPerformance_Begin(kDioramaPerformance_FrameAnalysis);
  DioramaFrameGeneration_CaptureWithSkybox(
      device, slot, s_plane_textures, pixels, pitch_bytes,
      upload.changed_plane_mask, s_diorama_skybox_view.texture, skybox_changed);
  DioramaPerformance_End(frame_analysis);
}

void PresentDiorama_Draw(ArRenderDevice *device, const FrameSlot *slot, float alpha) {
  DioramaPerformanceScope presentation_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Total);
  const uint8_t *pixels[kDioramaPlane_Count];
  CaptureDioramaPpuSurfaces(slot, pixels, NULL);
  /* PresentUpload recorded exactly which requested/content-bearing surfaces
   * uploaded successfully before releasing their producer. A NULL entry is
   * already Diorama_Composite's established "plane absent" contract, and
   * also prevents stale texture contents from resurfacing after an empty
   * priority band or failed upload. */
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    if (!(s_diorama_uploaded_plane_mask & (1u << plane)))
      pixels[plane] = NULL;
  ArRenderTexture current_textures[kDioramaPlane_Count];
  ArRenderTexture scene_textures[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    current_textures[plane] = s_plane_textures[plane];
  DioramaPerformanceScope frame_synthesis =
      DioramaPerformance_Begin(kDioramaPerformance_FrameSynthesis);
  DioramaSkyboxView skybox_view = s_diorama_skybox_view;
  const uint32_t generated_plane_mask = DioramaFrameGeneration_PrepareWithSkybox(
      device, slot, alpha, current_textures,
      s_diorama_uploaded_plane_mask, scene_textures,
      skybox_view.texture, &skybox_view.texture);
  skybox_view.dynamic =
      (generated_plane_mask & (1u << kDioramaFrameGenerationSkybox)) != 0;
  DioramaPerformance_End(frame_synthesis);
  /* The existing graphics setting now selects frame-space generation.
   * Prepare fails individual planes closed when either endpoint or pair
   * continuity is unavailable, leaving their current raw textures intact. */
  const DioramaCameraView camera = DioramaCamera_Present(
      &s_diorama_camera, &slot->diorama_camera,
      slot->timestamp_ns, HostClock_Nanoseconds());

  /* Fix B/BH6: resolve BG2's row-banded valid capture spans from the slot
   * alone (the presenter only reads the captured frame). ws_extra, not
   * extra_left_right, is the offset: the capture pitch and Diorama_Upload's
   * rect are both derived from ws_extra, so it is what texture column 0
   * corresponds to. Keep the two concepts distinct even when their values are
   * equal, so either can change without altering the other's meaning. */
  DioramaBgValidSpanPlan bg2_valid_spans;
  /* + obj_apron: each span is in SURFACE columns, and screen x = 0 sits at
   * column obj_apron + ws_extra now that the surfaces carry resolve headroom
   * on both sides. Without it the skybox would crop its sky an apron early. */
  DioramaBgValidSpanPlan_Build(
      slot->ws_extra + slot->obj_apron,
      slot->extra_left_right,
      slot->extra_left_cur, slot->extra_right_cur,
      slot->bg_capture_pad_to_budget,
      &slot->action_bg_plan.layer[kActionBgPlanLayerCount - 1],
      slot->ws_extra_top,
      slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      kFrameSlotLayerTextureWidth, &bg2_valid_spans);
  ArRenderRectI output_viewport;
  if (!Present_ResolveOutputViewport(device, slot, &output_viewport)) {
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    SessionFatal_Request(
        "The renderer could not resolve the Diorama output viewport (%s). "
        "Restart the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(device));
    return;
  }
  DioramaProjection action_projection;
  const uint8_t required_effect_obj_priorities =
      (slot->action_effect_lighting || slot->action_effect_particles)
          ? ActionEffectProjection_RequiredObjPriorityMask(
                &slot->action_effects, &slot->action_scene_effects)
          : 0;
  const uint8_t effect_obj_priority_mask =
      Diorama_FilterObjEffectProjectionMask(
          required_effect_obj_priorities,
          slot->diorama_plane_request_mask,
          slot->diorama_plane_content_mask,
          s_diorama_uploaded_plane_mask);
  const uint32_t required_effect_bg_planes =
      (slot->action_effect_lighting || slot->action_effect_particles ||
       slot->action_environmental_effects)
          ? ActionEffectProjection_RequiredBgPlaneMask(
                &slot->action_effects, &slot->action_scene_effects)
          : 0;
  const uint32_t effect_bg_plane_mask =
      Diorama_FilterBgEffectProjectionMask(
          required_effect_bg_planes,
          slot->diorama_plane_request_mask,
          slot->diorama_plane_content_mask,
          s_diorama_uploaded_plane_mask);
  (void)PresentActionHeat_Begin(device, slot, output_viewport);
  const ArRenderRectI viewport = PresentActionHeat_SceneViewport(output_viewport);
  PresentActionPlaneEffectContext plane_effect = {
    device, slot, viewport,
  };
  ArRenderPointF plane_offsets[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    plane_offsets[plane] = DioramaFrameGeneration_PlaneOffset(plane);
  const DioramaCapture capture = {
      .plane_capture_offsets = plane_offsets,
      .width = slot->snes_width,
      .height =
          slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
      .authentic_y0 = slot->ws_extra_top,
      .obj_apron = slot->obj_apron,
      .textures = scene_textures,
      .pixels = pixels,
      .bg_transparent_fill_configured =
          slot->diorama_bg_transparent_fill_configured,
      .bg_transparent_fill_argb = slot->diorama_bg_transparent_fill_argb,
      .coverage_masks =
          slot->interp_setting_enabled ? NULL : s_diorama_coverage_masks,
      .bg2_valid_spans = &bg2_valid_spans,
      .skybox = &skybox_view,
      .bg2_revision = s_diorama_bg2_content_revision,
      .bg2_dynamic =
          (generated_plane_mask & (UINT32_C(1) << SR_PPU_OVERLAY_BG2)) != 0,
  };
  const DioramaView view = {
      .camera = camera.pose,
      .distance_scale = camera.distance_scale,
      .center_camera_vertically = camera.center_vertically,
      .pixel_aspect = slot->pixel_aspect,
      .ignore_aspect_ratio = slot->ignore_aspect_ratio,
      .visible_width = slot->visible_width,
      .viewport = viewport,
  };
  const DioramaScene scene = {
      .map_group = slot->diorama_map_group,
      .map_number = slot->diorama_map_number,
      .layer_section = slot->diorama_layer_section,
      .bg1_dimming = PresentActionEffects_Bg1Dimming(slot),
      .additive_plane_mask =
          slot->diorama_plane_additive_mask & s_diorama_uploaded_plane_mask,
      .effect_obj_priority_mask = effect_obj_priority_mask,
      .effect_bg_plane_mask = effect_bg_plane_mask,
      .plane_effect = PresentActionEffects_DrawDioramaPlane,
      .plane_effect_userdata = &plane_effect,
  };
  const PresentationOutcome diorama = Diorama_Composite(
      device, &capture, &view, &scene, &action_projection);
  if (!PresentationOutcome_IsUsable(diorama)) {
    PresentActionHeat_Cancel(device);
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    SessionFatal_Request(
        "The selected Diorama renderer could not complete its core scene "
        "(%s). Restart the game. If this happens again, update your "
        "graphics driver or disable Diorama mode before entering the room.",
        ArRenderDevice_LastError(device));
    return;
  }
  DioramaPerformanceScope callback_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Callback);
  PresentActionEffects_Draw(device, slot, viewport, &action_projection);
  DioramaPerformance_End(callback_performance);
  PresentActionHeat_End(device, slot, output_viewport);
  /* Flat HUD mode leaves BG3 in the same RemoveFromGame capture used by flat
   * presentation. Reconstruct its split pieces into one texture before
   * drawing the screen-space overlay; drawing them directly creates seams
   * (see PresentHud_DrawComposited).
   *
   * With diorama_hud_flat off, capture rebinds BG3 into the diorama layer
   * buffer so it renders as the ordinary tilted BG3 plane in
   * Diorama_Composite's own
   * per-layer loop above — skip the anchored overlay entirely here so the
   * two don't both draw a HUD. */
  if (slot->diorama_hud_flat)
    PresentHud_DrawComposited(
        device, slot, (ArRenderRectI){
          output_viewport.x, output_viewport.y,
          output_viewport.w, output_viewport.h,
        });
  DioramaPerformance_End(presentation_performance);
  DioramaPerformance_PresentCompleted();
}

void PresentDiorama_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_diorama_skybox_texture);
  s_diorama_skybox_texture = ArRenderTexture_Invalid();
  s_diorama_skybox_view = (DioramaSkyboxView){0};
  PresentationUploadMirror_Reset(&s_diorama_skybox_mirror);
  DioramaFrameGeneration_Reset();
}
