/* Present-time rendering is isolated from live game state. This file must NOT
 * declare or extern g_ppu, g_settings, g_snes_width, g_ws_extra,
 * g_active_pixel_aspect, or call Settings_Visible*() — every present-time
 * decision comes from the `const FrameSlot *` handed in. Leaving those symbols
 * undeclared makes a stray live read a compile error.
 *
 * Presentation resources are different: the renderer, window, textures, and
 * host-derived pixel products are boot-owned and used synchronously on the
 * render/main thread. PPU-bound output surfaces arrive through FrameSlot's
 * runner-ABI snapshot. */

#include <SDL3/SDL.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "action/action_bg_tuner.h"
#include "action/action_effect_projection.h"
#include "present.h"
#include "action/action_effect_render.h"
#include "constants.h"
#include "crt_post.h"
#include "snesrecomp/game/types.h"
#include "diorama/diorama.h"
#include "diorama/diorama_frame_generation.h"
#include "diorama/diorama_performance.h"
#include "diorama/diorama_upload.h"
#include "diorama/diorama_skybox_uv.h"
#include "diorama/diorama_planes.h"
#include "hd_replacement_host.h"
#include "settings_overlay.h"
#include "dev/scene_inspector.h"
#include "render_capabilities.h"
#include "sim/sim_render_atlas.h"
#include "sim/sim_background_voxel_renderer.h"
#include "sim/sim3d.h"
#include "sim/sim3d_performance.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
#include "present_internal.h"
#include "render_comparison.h"
#include "session_fatal.h"
#include "presentation_geometry.h"
#include "presentation_upload_mirror.h"
#include "render/render_output.h"


extern SDL_Renderer *g_renderer;
extern ArRenderDevice g_render_device;
extern ArRenderTexture g_texture;
extern ArRenderTexture g_authentic_texture;
extern ArRenderTexture g_hud_bg_texture;
extern ArRenderTexture g_hud_obj_texture;
extern ArRenderTexture g_diorama_textures[kDioramaPlane_Count];
extern ArRenderTexture g_sim_obj_atlas_texture;

extern ArRenderTexture g_sim3d_layer_textures[kSim3DPlane_Count];
extern ArRenderTexture g_sim3d_flat_texture;
static uint32_t s_diorama_uploaded_plane_mask;
static ArRenderTexture s_action_bg1_mask_texture;
static ArRenderTexture s_action_bg2_mask_texture;
static ArRenderTexture s_action_plane_effect_target;
static int s_action_plane_effect_w, s_action_plane_effect_h;
static bool s_action_plane_blend_supported = true;
static ArRenderTexture s_action_heat_target;
static int s_action_heat_w, s_action_heat_h;
static bool s_action_heat_supported = true;
static bool s_action_heat_engaged;

typedef struct ActionHeatPassState {
  ArRenderTargetState target_state;
  bool valid;
} ActionHeatPassState;

typedef struct ActionHeatMeshCache {
  ActionHeatRenderMesh mesh;
  SDL_Rect viewport;
  int target_width, target_height, source_width;
  uint16_t game_frame;
  bool valid;
} ActionHeatMeshCache;

static ActionHeatPassState s_action_heat_saved_state;
static ActionHeatMeshCache s_action_heat_mesh_cache;

static const SrPpuSurfaceView *BoundPpuSurface(
    const SrPpuSurfaceView *surface) {
  return surface && surface->data &&
      (surface->flags & SR_PPU_SURFACE_BOUND) != 0u &&
      surface->pixel_format == SR_PPU_PIXEL_FORMAT_ARGB8888_U32 &&
      surface->pitch_bytes != 0u &&
      surface->pitch_bytes <= INT_MAX &&
      surface->width_pixels == surface->pitch_bytes / sizeof(uint32_t) &&
      surface->byte_size >=
          surface->pitch_bytes * (uint64_t)surface->height_pixels
      ? surface : NULL;
}

static bool PpuSurfaceHolds(
    const SrPpuSurfaceView *surface, int width, int height) {
  surface = BoundPpuSurface(surface);
  return surface && width > 0 && height > 0 &&
      (uint32_t)width <= surface->width_pixels &&
      (uint32_t)height <= surface->height_pixels;
}

static const uint8_t *PpuSurfaceRegion(
    const SrPpuSurfaceView *surface, int x, int y, int width, int height) {
  surface = BoundPpuSurface(surface);
  if (!surface || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      (uint64_t)(uint32_t)x + (uint32_t)width > surface->width_pixels ||
      (uint64_t)(uint32_t)y + (uint32_t)height > surface->height_pixels)
    return NULL;
  return surface->data + (size_t)y * (size_t)surface->pitch_bytes +
      (size_t)x * sizeof(uint32_t);
}

static const SrPpuSurfaceView *DioramaPpuSurface(
    const FrameSlot *slot, int plane) {
  if (!slot) return NULL;
  if (plane >= SR_PPU_OVERLAY_BG1 && plane <= SR_PPU_OVERLAY_OBJ)
    return &slot->ppu_surfaces.overlays[plane][0];
  switch (plane) {
    case kDioramaPlane_Backdrop:
      return &slot->ppu_surfaces.main;
    case kDioramaPlane_Bg1Hi:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][1];
    case kDioramaPlane_Bg2Hi:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][1];
    case kDioramaPlane_Obj1:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][1];
    case kDioramaPlane_Obj2:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][2];
    case kDioramaPlane_Obj3:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][3];
    case kDioramaPlane_Bg1Far:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][2];
    case kDioramaPlane_Bg2Far:
      return &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][2];
    default:
      return NULL;
  }
}

static const SrPpuSurfaceView *Sim3DPpuSurface(
    const FrameSlot *slot, int plane) {
  return slot && plane >= 0 && plane < kSim3DPlane_Count
      ? &slot->sim3d_output_surfaces.planes[plane] : NULL;
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
    if (!PpuSurfaceHolds(surface, width, height)) continue;
    pixels[plane] = surface->data;
    if (pitch_bytes)
      pitch_bytes[plane] = (size_t)surface->pitch_bytes;
  }
}

/* Effect builders are synchronous and presentation runs on one render thread,
 * so one reusable workspace covers actor and every depth-ordered decoration
 * pass. Keep these large bounded arrays out of automatic storage: the scene
 * batch alone is roughly half a MiB and can exhaust default Windows/custom
 * thread stacks before SDL or a backend driver gets its own frame. */
typedef struct ActionEffectRenderScratch {
  ActionEffectRenderBatch spell;
  ActionSceneEffectRenderBatch scene;
} ActionEffectRenderScratch;

static ActionEffectRenderScratch s_action_effect_render_scratch;

/* Streaming textures retain their last successfully uploaded pixels. Exact CPU
 * mirrors let static presentation surfaces cost no bus upload and locally
 * animated surfaces update only their changed bounding rectangle. Byte-exact
 * comparison avoids making rendering correctness depend on a hash. */
enum {
  kSim3DUploadSurface_Flat = kSim3DPlane_Count,
  kSim3DUploadSurface_Atlas,
  kSim3DUploadSurface_Count,
};

enum {
  kActionUploadSurface_Frame,
  kActionUploadSurface_Authentic,
  kActionUploadSurface_Bg1Mask,
  kActionUploadSurface_Bg2Mask,
  kActionUploadSurface_HudBg,
  kActionUploadSurface_HudObj,
  kActionUploadSurface_Count,
};

static PresentationUploadMirror
    s_sim3d_upload_mirrors[kSim3DUploadSurface_Count];
static PresentationUploadMirror
    s_action_upload_mirrors[kActionUploadSurface_Count];
static uint64_t s_authentic_uploaded_frame_serial;

static void ResetSim3DUploadMirrors(void) {
  for (int surface = 0; surface < kSim3DUploadSurface_Count; surface++)
    PresentationUploadMirror_Reset(&s_sim3d_upload_mirrors[surface]);
}

static void ResetActionUploadMirrors(void) {
  for (int surface = 0; surface < kActionUploadSurface_Count; surface++)
    PresentationUploadMirror_Reset(&s_action_upload_mirrors[surface]);
  s_authentic_uploaded_frame_serial = 0;
}

uint64_t PresentAuthenticUploadedFrameSerial(void) {
  return s_authentic_uploaded_frame_serial;
}

static bool UploadChangedSurface(
    ArRenderTexture texture, PresentationUploadMirror *mirror,
    const uint8_t *pixels, int width, int height, int source_pitch,
    int destination_x, int destination_y) {
  PresentationUploadResult result;
  const bool uploaded = PresentationUploadMirror_UploadArgb8888(
      mirror, &g_render_device, texture, pixels, width, height, source_pitch,
      destination_x, destination_y, &result);
  if (uploaded && result.uploaded_bytes)
    Sim3DPerformance_AddUpload(result.uploaded_bytes);
  return uploaded;
}

static bool UploadChangedSim3DSurface(
    ArRenderTexture texture, int surface, const uint32_t *pixels,
    int width, int height, int source_pitch_pixels) {
  if (surface < 0 || surface >= kSim3DUploadSurface_Count ||
      source_pitch_pixels > INT_MAX / (int)sizeof(uint32_t))
    return false;
  return UploadChangedSurface(
      texture,
      &s_sim3d_upload_mirrors[surface],
      (const uint8_t *)pixels, width, height,
      source_pitch_pixels * (int)sizeof(uint32_t), 0, 0);
}

static void DisableActionPlaneEffect(const char *operation) {
  if (!s_action_plane_blend_supported) return;
  s_action_plane_blend_supported = false;
  fprintf(stderr,
          "[action-fx] flat BG-local effect unavailable at %s (%s); "
          "disabled\n",
          operation ? operation : "unknown operation",
          ArRenderDevice_LastError(&g_render_device));
}

SDL_FRect ToFRect(SDL_Rect r) {
  return (SDL_FRect){ (float)r.x, (float)r.y, (float)r.w, (float)r.h };
}

static ArRenderRectF ToRenderRectF(SDL_Rect rectangle) {
  return (ArRenderRectF){
    (float)rectangle.x, (float)rectangle.y,
    (float)rectangle.w, (float)rectangle.h,
  };
}

static int ScaledHudPixels(int pixels, double scale) {
  int result = (int)(pixels * scale + 0.5);
  return result > 0 ? result : 1;
}

static void RenderHudChunk(ArRenderTexture texture, SDL_Rect src, SDL_Rect dst) {
  if (!ArRenderTexture_IsValid(texture) ||
      src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
    return;
  const ArRenderRectF source = ToRenderRectF(src);
  const ArRenderRectF destination = ToRenderRectF(dst);
  (void)ArRenderDevice_DrawTexture(
      &g_render_device, texture, &source, &destination);
}

static void AddHudPresentationChunk(HudPresentationChunk *chunks, int *count,
                                    ArRenderTexture texture,
                                    SDL_Rect texture_source,
                                    SDL_Rect screen_source,
                                    SDL_Rect output_destination,
                                    InspectorPresentationKind kind,
                                    int inspector_x_bias) {
  if (!chunks || !count || *count >= kHudPresentationChunkCapacity ||
      !ArRenderTexture_IsValid(texture) ||
      texture_source.w <= 0 || texture_source.h <= 0 ||
      screen_source.w <= 0 || screen_source.h <= 0 ||
      output_destination.w <= 0 || output_destination.h <= 0)
    return;
  chunks[(*count)++] = (HudPresentationChunk){
    texture, texture_source, screen_source, output_destination,
    kind, inspector_x_bias,
  };
}

/* One pure geometry description drives both compositing and hit-testing.
 * Presentation supplies FrameSlot-derived inputs; DevTools_InspectWindowPoint
 * supplies a live snapshot. */
int BuildHudPresentationChunks(SDL_Rect viewport,
                               const HudProjectionInputs *in,
                               HudPresentationChunk *chunks) {
  if (!ArRenderTexture_IsValid(in->hud_bg_texture) ||
      !in->hud_split_height)
    return 0;

  int count = 0;
  double scale_y, scale_x;
  if (in->hud_scale_percent == 0) {
    /* Auto: derived from the viewport, so already in physical output pixels. */
    scale_y = (double)viewport.h / in->snes_height;
    scale_x = (double)viewport.w / in->visible_width;
  } else {
    /* Pinned: the percentage is SNES-pixels-per-OUTPUT-pixel, and the output is
     * physical under SDL_WINDOW_HIGH_PIXEL_DENSITY. The FrameSlot already
     * carries the density-corrected value (D6 — no live settings read here). */
    scale_y = in->hud_scale_percent / (double)kPercentScale;
    scale_x = scale_y * (in->pixel_aspect == kPixelAspect_Crt43 ? 7.0 / 6.0 : 1.0);
  }

  int tex_extra = (in->snes_width - kFrameSlotAuthenticWidth) / 2;
  int height = in->hud_split_height;
  int player_y = in->hud_player_row_y;
  int enemy_y = in->hud_left_only_y;
  if (player_y > height) player_y = height;
  if (enemy_y > height) enemy_y = height;
  if (player_y > enemy_y) player_y = enemy_y;

  /* Band 1: top row (ACT/TIME/SCORE) — 3-way left/center/right split. */
  int upper_h = player_y;
  int upper_dh = ScaledHudPixels(upper_h, scale_y);

  SDL_Rect src = { tex_extra, 0, in->hud_left_end, upper_h };
  SDL_Rect dst = { viewport.x, viewport.y,
                   ScaledHudPixels(src.w, scale_x), upper_dh };
  AddHudPresentationChunk(
      chunks, &count, in->hud_bg_texture, src,
      (SDL_Rect){ 0, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, -in->extra_left_right);

  if (in->hud_left_end < in->hud_right_start) {
    src.x = tex_extra + in->hud_left_end;
    src.w = in->hud_right_start - in->hud_left_end;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.x = viewport.x + (viewport.w - dst.w) / 2;
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ in->hud_left_end, 0, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, 0);
  }

  int right_source_w = kFrameSlotAuthenticWidth - in->hud_right_start;
  int right_dest_w = ScaledHudPixels(right_source_w, scale_x);
  src.x = tex_extra + in->hud_right_start;
  src.w = right_source_w;
  dst.x = viewport.x + viewport.w - right_dest_w;
  dst.w = right_dest_w;
  AddHudPresentationChunk(
      chunks, &count, in->hud_bg_texture, src,
      (SDL_Rect){ in->hud_right_start, 0, src.w, src.h }, dst,
      kInspectorPresentation_HudBg, in->extra_left_right);

  /* Band 2: player row (PLAYER health + magic-scroll) — left+right split
   * at hud_right_start so health pips stay left-anchored and scroll tiles
   * stay right-anchored regardless of HP level. */
  if (player_y < enemy_y) {
    int mid_h = enemy_y - player_y;
    int mid_dh = ScaledHudPixels(mid_h, scale_y);
    int mid_dy = viewport.y + ScaledHudPixels(player_y, scale_y);

    src.x = tex_extra;
    src.y = player_y;
    src.w = in->hud_right_start;
    src.h = mid_h;
    dst.x = viewport.x;
    dst.y = mid_dy;
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = mid_dh;
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ 0, player_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, -in->extra_left_right);

    src.x = tex_extra + in->hud_right_start;
    src.w = kFrameSlotAuthenticWidth - in->hud_right_start;
    dst.x = viewport.x + viewport.w - ScaledHudPixels(src.w, scale_x);
    dst.w = ScaledHudPixels(src.w, scale_x);
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ in->hud_right_start, player_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, in->extra_left_right);
  }

  /* Band 3: enemy row — full-width left-anchored (boss health spans the
   * entire screen). */
  if (enemy_y < height) {
    int low_h = height - enemy_y;
    src.x = tex_extra;
    src.y = enemy_y;
    src.w = kFrameSlotAuthenticWidth;
    src.h = low_h;
    dst.x = viewport.x;
    dst.y = viewport.y + ScaledHudPixels(enemy_y, scale_y);
    dst.w = ScaledHudPixels(src.w, scale_x);
    dst.h = ScaledHudPixels(low_h, scale_y);
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, src,
        (SDL_Rect){ 0, enemy_y, src.w, src.h }, dst,
        kInspectorPresentation_HudBg, -in->extra_left_right);
  }

  /* Band 4 (diorama): everything on BG3 BELOW the status bar — the act-title
   * card and the pause text. Unlike the bands above, this content is authored
   * for the authentic 256px screen and has no left/right anchor semantics, so
   * it is drawn as a single centered chunk at its authentic Y. Only present
   * when the capture side extended BG3's rectangle past the split (diorama +
   * diorama_hud_flat); in flat mode these rows are still drawn by the game
   * into the framebuffer and hud_body_y1 stays 0. */
  if (in->hud_body_y1 > height) {
    int body_h = in->hud_body_y1 - height;
    int body_dw = ScaledHudPixels(kFrameSlotAuthenticWidth, scale_x);
    SDL_Rect body_src = { tex_extra, height, kFrameSlotAuthenticWidth, body_h };
    SDL_Rect body_dst = { viewport.x + (viewport.w - body_dw) / 2,
                          viewport.y + ScaledHudPixels(height, scale_y),
                          body_dw, ScaledHudPixels(body_h, scale_y) };
    AddHudPresentationChunk(
        chunks, &count, in->hud_bg_texture, body_src,
        (SDL_Rect){ 0, height, kFrameSlotAuthenticWidth, body_h }, body_dst,
        kInspectorPresentation_HudBg, 0);
  }

  /* Action's selected-magic icon (4 OAM), simulation's hourglass (4 OAM), and
   * Sky Palace's magic icon (4 OAM for Magical Fire, 1 for the other three
   * spells) are separately validated OAM signatures; the caller resolves the
   * icon x/y (from live oam/highOam or the FrameSlot snapshot) and passes it in
   * already resolved, so this function stays free of oam[]/highOam[] entirely.
   * Every one of those signatures covers the same 16x16 footprint, which is why
   * the slot COUNT never reaches this far and one chunk size serves them all. */
  if (ArRenderTexture_IsValid(in->hud_obj_texture) && in->obj_icon_valid &&
      in->obj_icon_x < kFrameSlotAuthenticWidth) {
    int x = in->obj_icon_x, y = in->obj_icon_y;
    int icon_w = 16, icon_h = 16;
    SDL_Rect obj_src = { tex_extra + x, y, icon_w, icon_h };
    SDL_Rect obj_dst = {
      viewport.x + viewport.w - right_dest_w - ScaledHudPixels(20, scale_x),
      viewport.y + ScaledHudPixels(y, scale_y),
      ScaledHudPixels(icon_w, scale_x),
      ScaledHudPixels(icon_h, scale_y),
    };
    AddHudPresentationChunk(
        chunks, &count, in->hud_obj_texture, obj_src,
        (SDL_Rect){ x, y, icon_w, icon_h }, obj_dst,
        kInspectorPresentation_HudObj, 0);
  }
  return count;
}

SDL_Rect ComputePresentationViewport(SDL_Renderer *renderer,
                                     bool ignore_aspect_ratio,
                                     int pixel_aspect, int visible_width,
                                     int snes_height) {
  return ComputePresentationViewportWithOutput(
      renderer, ignore_aspect_ratio, pixel_aspect, visible_width,
      snes_height, NULL);
}

SDL_Rect ComputePresentationViewportWithOutput(
    SDL_Renderer *renderer, bool ignore_aspect_ratio,
    int pixel_aspect, int visible_width, int snes_height,
    SDL_Point *output_size) {
  int out_w = 0, out_h = 0;
  SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
  if (output_size) *output_size = (SDL_Point){out_w, out_h};
  return PresentationGeometry_CalculateViewport(
      out_w, out_h, ignore_aspect_ratio,
      pixel_aspect == kPixelAspect_Crt43, visible_width, snes_height);
}

static HudProjectionInputs BuildProjectionInputsFromSlot(const FrameSlot *slot) {
  HudProjectionInputs in = {0};
  in.hud_bg_texture = g_hud_bg_texture;
  in.hud_obj_texture = g_hud_obj_texture;
  in.hud_scale_percent = slot->hud_scale_percent;
  in.pixel_aspect = slot->pixel_aspect;
  in.snes_width = slot->snes_width;
  in.snes_height = slot->snes_height;
  in.visible_width = slot->visible_width;
  in.hud_split_height = slot->hud_split_height;
  in.hud_left_end = slot->hud_left_end;
  in.hud_right_start = slot->hud_right_start;
  in.hud_player_row_y = slot->hud_player_row_y;
  in.hud_left_only_y = slot->hud_left_only_y;
  in.extra_left_right = slot->extra_left_right;
  {
    const FrameSlotOverlayCapture *bg3 =
        &slot->overlay_captures[kFrameSlotOverlay_Bg3];
    if (bg3->y1 > (int16_t)slot->hud_split_height && bg3->y1 <= 240)
      in.hud_body_y1 = (uint8_t)bg3->y1;
  }

  /* The promote's own latched range, NOT overlay_captures[Obj].oamFirst/Count:
   * in diorama mode that capture describes the full-frame 0..127 scene claim
   * that legitimately overwrote the icon's capture, so keying off it dropped
   * obj_icon_valid and the icon fell back to whatever the scene did with it
   * (drawn tilted and centered rather than anchored beside the right group).
   *
   * Any nonzero count, not ==4: the promote only ever latches a range it has
   * validated as a 16x16 HUD icon, and Sky Palace spends 1 slot on that icon
   * for three of the four spells and 4 for Magical Fire. Demanding 4 here was
   * the second half of the bug that stranded those three at centre screen. */
  if (slot->oam_valid && slot->hud_icon_count) {
    int first = slot->hud_icon_first;
    in.obj_icon_x = (slot->oam[first * 2] & 0xff) |
        ((slot->high_oam[first >> 2] >> ((first & 3) * 2)) & 1) << 8;
    in.obj_icon_y = slot->oam[first * 2] >> 8;
    in.obj_icon_valid = true;
  }
  return in;
}

static void PresentHudOverlay(const FrameSlot *slot, SDL_Rect viewport) {
  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, &in, chunks);
  for (int i = 0; i < count; i++)
    RenderHudChunk(chunks[i].texture, chunks[i].texture_source,
                   chunks[i].output_destination);
}

/* A7 (followup doc), diorama variant. A straight port of PresentHudOverlay
 * into the diorama branch produced visible seams between the ACT/TIME/SCORE
 * bands because the scene and overlay followed different viewport policies.
 * Both branches now receive the same aspect-fit viewport; retaining a single
 * composite texture also makes per-chunk rounding self-contained.
 *
 * Reconstruct the whole HUD into one dedicated texture first (recreated
 * whenever the output size changes — same resolution the chunks would have
 * rendered at, just isolated so any residual per-chunk rounding stays
 * self-contained instead of visible against the tilted scene), then draw
 * that single texture as a plain, undistorted screen overlay — same
 * screen-space blit as the flat branch, just one draw call instead of up to
 * kHudPresentationChunkCapacity. */
static ArRenderTexture s_hud_composite_texture;
static int s_hud_composite_w, s_hud_composite_h;

static ArRenderTexture EnsureHudCompositeTexture(int w, int h) {
  if (!ArRenderDevice_IsReady(&g_render_device) || w <= 0 || h <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_hud_composite_texture) &&
      s_hud_composite_w == w &&
      s_hud_composite_h == h)
    return s_hud_composite_texture;
  ArRenderDevice_DestroyTexture(&g_render_device, s_hud_composite_texture);
  s_hud_composite_texture = ArRenderTexture_Invalid();
  s_hud_composite_w = w;
  s_hud_composite_h = h;
  const ArRenderTextureDesc desc = {
    .width = w,
    .height = h,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  (void)ArRenderDevice_CreateTexture(
      &g_render_device, &desc, &s_hud_composite_texture);
  return s_hud_composite_texture;
}

void PresentHudOverlayComposited(const FrameSlot *slot,
                                        SDL_Rect viewport) {
  ArRenderTexture composite = EnsureHudCompositeTexture(
      viewport.w, viewport.h);
  if (!ArRenderTexture_IsValid(composite)) return;

  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  SDL_Rect local_viewport = { 0, 0, viewport.w, viewport.h };
  int count = BuildHudPresentationChunks(local_viewport, &in, chunks);
  if (count <= 0) return;

  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      &g_render_device, composite, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (begin == kArRenderTargetBegin_StateLost)
      SessionFatal_Request(
          "The renderer lost its scene target while beginning HUD composition "
          "(%s). Restart the game; if this repeats, update your graphics "
          "driver.", ArRenderDevice_LastError(&g_render_device));
    return;
  }
  const bool target_ready =
      ArRenderDevice_UseOutputCoordinates(&g_render_device) &&
      ArRenderDevice_Clear(
          &g_render_device,
          (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  if (target_ready) {
    for (int i = 0; i < count; i++)
      RenderHudChunk(chunks[i].texture, chunks[i].texture_source,
                     chunks[i].output_destination);
  }
  if (!ArRenderDevice_EndTarget(&g_render_device, &target_state)) {
    SessionFatal_Request(
        "The renderer could not restore its scene target after composing the "
        "HUD (%s). Restart the game; if this repeats, update your graphics "
        "driver.", ArRenderDevice_LastError(&g_render_device));
    return;
  }
  if (!target_ready) return;

  const ArRenderRectF destination = ToRenderRectF(viewport);
  (void)ArRenderDevice_DrawTexture(
      &g_render_device, composite, NULL, &destination);
}

static void PresentMode7Composite(const FrameSlot *slot, SDL_Rect viewport) {
  if (!ArRenderTexture_IsValid(g_m7_texture) || !slot->m7_active) return;
  SDL_Rect src = { slot->visible_x0 * kHdMode7Scale, 0,
                   slot->visible_width * kHdMode7Scale,
                   slot->snes_height * kHdMode7Scale };
  const ArRenderRectF source = ToRenderRectF(src);
  const ArRenderRectF destination = ToRenderRectF(viewport);
  (void)ArRenderDevice_DrawTexture(
      &g_render_device, g_m7_texture, &source, &destination);
}

/* Draw every active HD replacement over the region its capture removed this
 * frame. Master brightness is resolved on the host texture so INIDISP fades
 * apply to the substituted art; forced blank suppresses it entirely. */
static void PresentHdReplacements(const FrameSlot *slot, SDL_Rect viewport) {
  if (slot->inidisp & 0x80) return;

  int vis_w = slot->visible_width;
  int vis_x0 = slot->visible_x0;
  int extra = (slot->snes_width - kFrameSlotAuthenticWidth) / 2;
  double scale_x = (double)viewport.w / vis_w;
  double scale_y = (double)viewport.h / slot->snes_height;

  for (int i = 0; i < slot->hd_entry_count; i++) {
    const FrameSlotHdEntry *entry = &slot->hd_entries[i];
    if (!entry->active || !ArRenderTexture_IsValid(entry->texture)) continue;
    const FrameSlotOverlayCapture *capture =
        &slot->overlay_captures[entry->source];
    if (capture->x1 <= capture->x0 || capture->y1 <= capture->y0 ||
        !(capture->flags & kFrameSlotOverlayFlag_RemoveFromGame))
      continue;
    int dx0 = (int)((capture->x0 + extra - vis_x0) * scale_x + 0.5);
    int dx1 = (int)((capture->x1 + extra - vis_x0) * scale_x + 0.5);
    int dy0 = (int)(capture->y0 * scale_y + 0.5);
    int dy1 = (int)(capture->y1 * scale_y + 0.5);
    SDL_Rect dst = { viewport.x + dx0, viewport.y + dy0,
                     dx1 - dx0, dy1 - dy0 };
    if (dst.w <= 0 || dst.h <= 0) continue;

    Uint8 mod = entry->brightness_mod
        ? (Uint8)((slot->inidisp & 0xf) * 255 / 15) : 255;
    const float modulation = (float)mod / 255.0f;
    const ArRenderRectF destination = ToRenderRectF(dst);
    (void)ArRenderDevice_DrawTextureTinted(
        &g_render_device, entry->texture, NULL, &destination,
        (ArRenderColorF){modulation, modulation, modulation, 1.0f});
  }
}

static int InspectorScreenToOutputX(SDL_Rect viewport, double screen_x,
                                    const FrameSlot *slot) {
  int visible_left = slot->visible_x0 - slot->ws_extra;
  return viewport.x + (int)((screen_x - visible_left) * viewport.w /
                            slot->visible_width + 0.5);
}

static int InspectorScreenToOutputY(SDL_Rect viewport, double screen_y,
                                    const FrameSlot *slot) {
  return viewport.y + (int)(screen_y * viewport.h / slot->snes_height + 0.5);
}

static int HudSourceToOutputX(const HudPresentationChunk *chunk, double source_x) {
  return chunk->output_destination.x +
      (int)((source_x - chunk->screen_source.x) *
            chunk->output_destination.w / chunk->screen_source.w + 0.5);
}

static int HudSourceToOutputY(const HudPresentationChunk *chunk, double source_y) {
  return chunk->output_destination.y +
      (int)((source_y - chunk->screen_source.y) *
            chunk->output_destination.h / chunk->screen_source.h + 0.5);
}

static bool HudHighlightToOutput(const HudPresentationChunk *chunk,
                                 int x0, int y0, int x1, int y1,
                                 SDL_Rect *output) {
  if (!chunk || !output) return false;
  x0 -= chunk->inspector_x_bias;
  x1 -= chunk->inspector_x_bias;
  const SDL_Rect source = chunk->screen_source;
  if (x0 < source.x) x0 = source.x;
  if (y0 < source.y) y0 = source.y;
  if (x1 > source.x + source.w) x1 = source.x + source.w;
  if (y1 > source.y + source.h) y1 = source.y + source.h;
  if (x1 <= x0 || y1 <= y0) return false;
  int output_x0 = HudSourceToOutputX(chunk, x0);
  int output_y0 = HudSourceToOutputY(chunk, y0);
  int output_x1 = HudSourceToOutputX(chunk, x1);
  int output_y1 = HudSourceToOutputY(chunk, y1);
  *output = (SDL_Rect){
    output_x0, output_y0, output_x1 - output_x0, output_y1 - output_y0,
  };
  return output->w > 0 && output->h > 0;
}

static bool FindSelectedHudChunk(const FrameSlot *slot, SDL_Rect viewport,
                                 HudPresentationChunk *selected) {
  if (slot->inspector_selection.kind == kInspectorPresentation_Base)
    return false;
  HudProjectionInputs in = BuildProjectionInputsFromSlot(slot);
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  int count = BuildHudPresentationChunks(viewport, &in, chunks);
  for (int i = count - 1; i >= 0; i--) {
    const SDL_Rect source = chunks[i].screen_source;
    if (chunks[i].inspector_kind != slot->inspector_selection.kind ||
        slot->inspector_selection.source_x < source.x ||
        slot->inspector_selection.source_x >= source.x + source.w ||
        slot->inspector_selection.source_y < source.y ||
        slot->inspector_selection.source_y >= source.y + source.h)
      continue;
    if (selected) *selected = chunks[i];
    return true;
  }
  return false;
}

static void PresentSceneInspector(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->scene_inspector_enabled || !SceneInspector_HasSelection())
    return;
  int x = 0, y = 0;
  if (!SceneInspector_GetPoint(&x, &y)) return;
  HudPresentationChunk hud_chunk;
  bool hud_selection = FindSelectedHudChunk(slot, viewport, &hud_chunk);
  int projected_px = hud_selection
      ? HudSourceToOutputX(&hud_chunk, slot->inspector_selection.source_x)
      : InspectorScreenToOutputX(viewport, slot->inspector_selection.source_x, slot);
  int projected_py = hud_selection
      ? HudSourceToOutputY(&hud_chunk, slot->inspector_selection.source_y)
      : InspectorScreenToOutputY(viewport, slot->inspector_selection.source_y, slot);
  int output_width = 0, output_height = 0;
  (void)ArRenderDevice_GetOutputSize(
      &g_render_device, &output_width, &output_height);
  bool same_output = output_width == slot->inspector_selection.output_width &&
                     output_height == slot->inspector_selection.output_height;
  int px = same_output ? slot->inspector_selection.output_x : projected_px;
  int py = same_output ? slot->inspector_selection.output_y : projected_py;
  int anchor_dx = px - projected_px;
  int anchor_dy = py - projected_py;

  const ArRenderColorF gold = {
    1.0f, 192.0f / 255.0f, 32.0f / 255.0f, 1.0f,
  };
  /* Crosshair arms scale with the output (7 SNES pixels' worth at the
   * current viewport scale, min the historical 7px) — a fixed 7 output
   * pixels is near-invisible at 4K/high-density output. */
  enum { kInspectorCrosshairMinimumArmPixels = 7 };
  int arm = viewport.h > 0
      ? (viewport.h * kInspectorCrosshairMinimumArmPixels +
         kFrameSlotAuthenticHeight / 2) /
          kFrameSlotAuthenticHeight
      : kInspectorCrosshairMinimumArmPixels;
  if (arm < kInspectorCrosshairMinimumArmPixels)
    arm = kInspectorCrosshairMinimumArmPixels;
  (void)ArRenderDevice_DrawLine(
      &g_render_device, (ArRenderPointF){(float)(px - arm), (float)py},
      (ArRenderPointF){(float)(px + arm), (float)py},
      1.0f, gold, kArRenderBlendMode_Alpha);
  (void)ArRenderDevice_DrawLine(
      &g_render_device, (ArRenderPointF){(float)px, (float)(py - arm)},
      (ArRenderPointF){(float)px, (float)(py + arm)},
      1.0f, gold, kArRenderBlendMode_Alpha);

  int x0, y0, x1, y1;
  if (SceneInspector_GetHighlight(&x0, &y0, &x1, &y1)) {
    SDL_Rect rect;
    bool have_rect = hud_selection &&
        HudHighlightToOutput(&hud_chunk, x0, y0, x1, y1, &rect);
    if (!hud_selection) {
      rect = (SDL_Rect){
        InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y0, slot),
        InspectorScreenToOutputX(viewport, x1, slot) -
            InspectorScreenToOutputX(viewport, x0, slot),
        InspectorScreenToOutputY(viewport, y1, slot) -
            InspectorScreenToOutputY(viewport, y0, slot),
      };
      have_rect = rect.w > 0 && rect.h > 0;
    }
    if (have_rect) {
      rect.x += anchor_dx;
      rect.y += anchor_dy;
      const float x0f = (float)rect.x;
      const float y0f = (float)rect.y;
      const float x1f = (float)(rect.x + rect.w);
      const float y1f = (float)(rect.y + rect.h);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0f, y0f},
          (ArRenderPointF){x1f, y0f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x1f, y0f},
          (ArRenderPointF){x1f, y1f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x1f, y1f},
          (ArRenderPointF){x0f, y1f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0f, y1f},
          (ArRenderPointF){x0f, y0f}, 1.0f, gold,
          kArRenderBlendMode_Alpha);
    }
  }
  SettingsOverlay_RenderDebugPanel(
      "SCENE INSPECTOR", SceneInspector_PanelText(), (SDL_Point){ px, py });
}

static void UploadActionWinnerMask(ArRenderTexture *texture, int mirror,
                                   const uint8_t *pixels,
                                   int pitch_bytes,
                                   const FrameSlot *slot) {
  if (!texture || !pixels || !slot || mirror < 0 ||
      mirror >= kActionUploadSurface_Count || pitch_bytes <= 0)
    return;
  if (!ArRenderTexture_IsValid(*texture)) {
    const ArRenderTextureDesc desc = {
      .width = kFrameSlotLayerTextureWidth,
      .height = kFrameSlotAuthenticHeight,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest,
      .blend = kArRenderBlendMode_Multiply,
    };
    (void)ArRenderDevice_CreateTexture(
        &g_render_device, &desc, texture);
  }
  if (ArRenderTexture_IsValid(*texture)) {
    const SDL_Rect mask = {0, 0, slot->snes_width, slot->snes_height};
    UploadChangedSurface(
        *texture, &s_action_upload_mirrors[mirror], pixels,
        mask.w, mask.h, pitch_bytes,
        mask.x, mask.y);
  }
}

void PresentUpload(const FrameSlot *slot) {
  if (!g_renderer || !ArRenderTexture_IsValid(g_texture)) return;
  Sim3DPerformanceScope performance = {0};
  if (slot->sim.view == kSimView_Enhanced)
    performance = Sim3DPerformance_Begin(kSim3DPerformance_Upload);

  if (ArRenderTexture_IsValid(g_authentic_texture) &&
      slot->authentic_frame_serial) {
    const int authentic_height = slot->snes_height + slot->ws_extra_top +
                                 slot->ws_extra_bottom;
    const SrPpuSurfaceView *surface =
        BoundPpuSurface(&slot->ppu_surfaces.authentic);
    const uint8_t *pixels = PpuSurfaceRegion(
        surface, 0, 0, slot->snes_width, authentic_height);
    if (pixels && UploadChangedSurface(
        g_authentic_texture,
        &s_action_upload_mirrors[kActionUploadSurface_Authentic],
        pixels, slot->snes_width, authentic_height,
        (int)surface->pitch_bytes, 0, 0)) {
      s_authentic_uploaded_frame_serial = slot->authentic_frame_serial;
    } else {
      s_authentic_uploaded_frame_serial = 0;
      if (RenderComparison_RequiresAuthenticFrame()) {
        const char *sdl_error = SDL_GetError();
        SessionFatal_Request(
            "Authentic comparison could not upload its current native frame "
            "(%s). Restart the game; if this repeats, update your graphics "
            "driver or select a different SDL renderer.",
            sdl_error[0] ? sdl_error : "texture upload failed");
      }
    }
  }

  if (slot->diorama_active) {
    const uint8_t *pixels[kDioramaPlane_Count];
    size_t pitch_bytes[kDioramaPlane_Count];
    CaptureDioramaPpuSurfaces(slot, pixels, pitch_bytes);
    uint32_t upload_mask = slot->diorama_plane_request_mask &
                           slot->diorama_plane_content_mask;
    /* Row 0 is the top of the captured world band. Upload both sides; the
     * authentic frame begins at ws_extra_top and the lower band follows it. */
    const DioramaUploadResult upload = Diorama_Upload(
        &g_render_device, g_diorama_textures, pixels, pitch_bytes,
        slot->snes_width + slot->obj_apron * 2,
        slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
        slot->obj_apron, upload_mask);
    s_diorama_uploaded_plane_mask = upload.synchronized_plane_mask;
    /* A failed raw upload cannot be a valid endpoint: exclude it before
     * retaining/analyzing the pair so generation never interpolates from an
     * image that was not actually presentable. */
    for (int plane = 0; plane < kDioramaPlane_Count; plane++)
      if (!(s_diorama_uploaded_plane_mask & (1u << plane)))
        pixels[plane] = NULL;
    DioramaPerformanceScope frame_analysis =
        DioramaPerformance_Begin(kDioramaPerformance_FrameAnalysis);
    DioramaFrameGeneration_Capture(
        &g_render_device, slot, pixels, pitch_bytes,
        upload.changed_plane_mask);
    DioramaPerformance_End(frame_analysis);
  } else {
    s_diorama_uploaded_plane_mask = 0;
    SDL_Rect upload = { 0, 0, slot->snes_width, slot->snes_height };
    const SrPpuSurfaceView *surface =
        BoundPpuSurface(&slot->ppu_surfaces.main);
    /* The main view reports the physical column for screen x=0. Upload starts
     * at screen x=-ws_extra, leaving any resolve apron outside the texture. */
    const int source_x = surface ? surface->origin_x - slot->ws_extra : -1;
    const int source_y = surface ? surface->origin_y - slot->ws_extra_top : -1;
    const uint8_t *pixels = PpuSurfaceRegion(
        surface, source_x, source_y, upload.w, upload.h);
    if (pixels)
      UploadChangedSurface(
          g_texture,
          &s_action_upload_mirrors[kActionUploadSurface_Frame],
          pixels, upload.w, upload.h, (int)surface->pitch_bytes,
          upload.x, upload.y);
  }

  const SrPpuSurfaceView *bg1_surface =
      BoundPpuSurface(
          &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0]);
  const SrPpuSurfaceView *bg2_surface =
      BoundPpuSurface(
          &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0]);
  if (!slot->diorama_active && slot->action_bg1_mask_valid &&
      PpuSurfaceHolds(bg1_surface, slot->snes_width, slot->snes_height))
    UploadActionWinnerMask(
        &s_action_bg1_mask_texture, kActionUploadSurface_Bg1Mask,
        bg1_surface->data, (int)bg1_surface->pitch_bytes, slot);
  if (!slot->diorama_active && slot->action_bg2_mask_valid &&
      PpuSurfaceHolds(bg2_surface, slot->snes_width, slot->snes_height))
    UploadActionWinnerMask(
        &s_action_bg2_mask_texture, kActionUploadSurface_Bg2Mask,
        bg2_surface->data, (int)bg2_surface->pitch_bytes, slot);

  /* Refresh HUD textures for both presentation paths. Diorama anchors the HUD
   * through the same textures as flat mode; skipping this upload would combine
   * stale pixels with the current frame's split geometry. */
  if (slot->hud_split_height) {
    int split_rows = slot->hud_split_height;
    if (ArRenderTexture_IsValid(g_hud_bg_texture)) {
      int rows = slot->overlay_captures[kFrameSlotOverlay_Bg3].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, slot->snes_width, rows };
      const SrPpuSurfaceView *surface = BoundPpuSurface(
          &slot->sim3d_output_surfaces.hud_bg);
      if (!surface)
        surface = BoundPpuSurface(
            &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0]);
      if (PpuSurfaceHolds(surface, hud.w, hud.h))
        UploadChangedSurface(
            g_hud_bg_texture,
            &s_action_upload_mirrors[kActionUploadSurface_HudBg],
            surface->data, hud.w, hud.h,
            (int)surface->pitch_bytes, hud.x, hud.y);
    }
    if (ArRenderTexture_IsValid(g_hud_obj_texture)) {
      int rows = slot->overlay_captures[kFrameSlotOverlay_Obj].y1;
      if (rows < split_rows) rows = split_rows;
      SDL_Rect hud = { 0, 0, slot->snes_width, rows };
      const SrPpuSurfaceView *surface = BoundPpuSurface(
          &slot->sim3d_output_surfaces.hud_obj);
      if (!surface)
        surface = BoundPpuSurface(
            &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0]);
      if (PpuSurfaceHolds(surface, hud.w, hud.h))
        UploadChangedSurface(
            g_hud_obj_texture,
            &s_action_upload_mirrors[kActionUploadSurface_HudObj],
            surface->data, hud.w, hud.h,
            (int)surface->pitch_bytes, hud.x, hud.y);
    }
  }

  if (ArRenderTexture_IsValid(g_m7_texture) && slot->m7_active) {
    SDL_Rect src = { slot->visible_x0 * kHdMode7Scale, 0,
                     slot->visible_width * kHdMode7Scale,
                     slot->snes_height * kHdMode7Scale };
    const SrPpuSurfaceView *surface =
        BoundPpuSurface(&slot->ppu_surfaces.mode7);
    const uint8_t *pixels =
        PpuSurfaceRegion(surface, src.x, src.y, src.w, src.h);
    const ArRenderRectI destination = {src.x, src.y, src.w, src.h};
    if (pixels && ArRenderDevice_UpdateTexture(
            &g_render_device, g_m7_texture, &destination, pixels,
            (int)surface->pitch_bytes)) {
      Sim3DPerformance_AddUpload(
          (uint64_t)src.w * (uint64_t)src.h * sizeof(uint32_t));
    }
  }

  /* D1b: the raw atlas follows the same upload-before-release ownership as
   * every other frame pixel buffer. Only the packed used rectangle is copied;
   * all descriptors in this immutable slot are bounded by that rectangle. */
  if (ArRenderTexture_IsValid(g_sim_obj_atlas_texture) &&
      slot->sim.town && slot->sim.atlas_valid &&
      slot->sim.atlas_used_width && slot->sim.atlas_used_height) {
    SDL_Rect atlas = { 0, 0, slot->sim.atlas_used_width,
                      slot->sim.atlas_used_height };
    UploadChangedSim3DSurface(
        g_sim_obj_atlas_texture, kSim3DUploadSurface_Atlas,
        g_sim_obj_atlas_pixels, atlas.w, atlas.h,
        kSimObjAtlasWidth);
  }

  if (slot->sim.separated_valid) {
    SDL_Rect frame = { 0, 0, slot->snes_width, slot->snes_height };
    uint32_t plane_upload_mask =
        Sim3D_PlaneTextureUploadMask(
            slot->sim.effective_features,
            slot->sim.separated_plane_mask);
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      const SrPpuSurfaceView *surface =
          BoundPpuSurface(Sim3DPpuSurface(slot, plane));
      if ((plane_upload_mask & (1u << plane)) &&
          ArRenderTexture_IsValid(g_sim3d_layer_textures[plane]) &&
          PpuSurfaceHolds(surface, frame.w, frame.h)) {
        UploadChangedSim3DSurface(
            g_sim3d_layer_textures[plane], plane,
            (const uint32_t *)surface->data,
            frame.w, frame.h,
            (int)(surface->pitch_bytes / sizeof(uint32_t)));
      }
    }
    /* Ground projection samples the separated planes directly. Upload the
     * CPU flat composite only for the fallback stage that actually draws it. */
    if (ArRenderTexture_IsValid(g_sim3d_flat_texture) &&
        !(slot->sim.effective_features & kSimFeature_GroundProjection)) {
      UploadChangedSim3DSurface(
          g_sim3d_flat_texture, kSim3DUploadSurface_Flat,
          g_sim3d_flat_pixels,
          frame.w, frame.h, frame.w);
    }
  }
  UploadSimTownCanvas();
  SimBackgroundVoxelRenderer_Upload(&g_render_device);
  UploadWorldNavigationComposition(slot);
  Sim3DPerformance_End(performance);
}

/* Presentation-owned effective camera. Free Cam uses the persisted pose from
 * the FrameSlot. Dynamic Cam eases reactive lean and event kicks around its
 * dedicated baseline. Diorama_Composite receives that resolved pose and never
 * reads producer-owned g_diorama_cam. */
static DioramaCameraPose g_diorama_render_cam;
static int g_diorama_render_cam_mode = -1;    /* -1: no frame composited yet */
static uint64_t g_diorama_render_cam_last_ns;

/* Dynamic-camera response constants. */
static const float kDioramaDampTau = 0.15f;    /* seconds, 1-exp(-dt/tau) */
static const float kDioramaLeanYaw = 0.10f;    /* rad, max yaw lean @ full run speed */
/* Doc's provisional 0.06 rad (half of yaw's 0.10) turned out imperceptible
 * in play (AR_DYNCAM_LOG confirmed the render camera genuinely swings
 * ~2.4 deg during a jump — this isn't a pipeline bug), most likely because
 * pitch reads far less visually salient than yaw in this 3/4 view (weaker
 * differential parallax between layers than a lateral sway produces) and a
 * running jump has both swinging at once, with yaw dominating. Raised to
 * match yaw's peak so a jump reads as clearly as running does. */
static const float kDioramaLeanPitch = 0.12f;  /* rad, max pitch lean @ full vertical speed */

/* B4-kick (followup doc): event-triggered impulses, decaying independently
 * of the baseline+lean damping above (a jolt should feel crisp, not get
 * folded into the slower position-ease target) — added on top of a LOCAL
 * copy of g_diorama_render_cam each frame, never baked into the persisted
 * render-cam state itself. kDioramaKickPitch/kDioramaKickTau are the doc's
 * literal event_kick_magnitude/event_kick_decay.
 *
 * The zoom-punch (kDioramaKickZoom) was originally spec'd for the boost
 * event, but PlayerBoost turned out not to be a clean trigger (fired
 * constantly just holding a direction — disabled, see event_hit/event_land
 * below). Repurposed onto the HIT event instead (live design call, 2026-07-21
 * — the effect itself read well, it just needed a reliable source): a hit
 * uses the reliable invuln-bit edge already relied on elsewhere
 * (AR_NO_KNOCKBACK), and now gets BOTH the jolt and the zoom-punch, making
 * it read as more dramatic than a routine landing (jolt only). */
static float g_diorama_kick_pitch;       /* rad, landing/hit jolt, decays to 0 */
static float g_diorama_kick_zoom;        /* fraction, hit zoom-punch, decays to 0 (negative = closer) */
static uint64_t g_diorama_last_slot_ns;  /* detects a genuinely NEW FrameSlot capture */
static const float kDioramaKickPitch = 0.05f;  /* rad */
static const float kDioramaKickZoom = -0.15f;  /* fraction; "slight" zoom-in */
static const float kDioramaKickTau = 0.20f;    /* seconds, wall-clock exp decay */

/* Host effects use the renderer abstraction's standard additive/alpha blend
 * modes and untextured geometry, not a backend shader. Those are portable
 * API paths,
 * but not a promise of pixel-identical rasterization across Metal, Vulkan,
 * Direct3D and software. Capability is verified at the point of use: SDL may
 * legally substitute the closest blend mode, so a successful set is followed
 * by a get-and-compare. Any rejection or substitution fails the stages closed. */
static SDL_AtomicInt s_effect_blend_supported = { .value = 1 };
static SDL_AtomicInt s_effect_geometry_supported = { .value = 1 };

void DisableEffectBlend(const char *operation) {
  if (!SDL_CompareAndSwapAtomicInt(
          &s_effect_blend_supported, 1, 0))
    return;
  fprintf(stderr,
          "[host-effects] effect blend pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, SDL_GetError());
}

static void DisableEffectGeometry(const char *operation) {
  if (!SDL_CompareAndSwapAtomicInt(
          &s_effect_geometry_supported, 1, 0))
    return;
  fprintf(stderr,
          "[host-effects] geometry pass unavailable at %s (%s) — "
          "effect lighting and particles disabled\n",
          operation, SDL_GetError());
}

bool EffectRendererAvailable(void) {
  return SDL_GetAtomicInt(&s_effect_blend_supported) != 0 &&
      SDL_GetAtomicInt(&s_effect_geometry_supported) != 0;
}

bool Present_EffectRendererSupported(void) {
  return EffectRendererAvailable();
}

static void DisableActionHeat(const char *operation) {
  if (!s_action_heat_supported) return;
  s_action_heat_supported = false;
  fprintf(stderr,
          "[action-fx] lava heat refraction unavailable at %s (%s); "
          "disabled\n",
          operation ? operation : "unknown operation", SDL_GetError());
}

static void FailActionHeatTargetState(const char *operation) {
  DisableActionHeat(operation);
  SessionFatal_Request(
      "The action heat-refraction pass could not restore the active render "
      "target (%s). Restart the game; if this repeats, update your graphics "
      "driver or disable action particles.",
      ArRenderDevice_LastError(&g_render_device));
}

static bool FrameUsesActionHeat(const FrameSlot *slot) {
  if (!slot || !slot->action_effect_particles ||
      (slot->diorama_active && !slot->diorama_hud_flat))
    return false;
  /* The haze is volcanic-room atmosphere, not reservoir geometry. Keying it
   * to the bounded visible-map scan made the post-process blink off whenever
   * the camera crossed a window where no complete bank-to-bank lake signature
   * fit, even though lava remained plainly visible. */
  return ActionEffects_IsAitosAct2LavaRoom(
      slot->diorama_map_group, slot->diorama_map_number);
}

static ArRenderTexture EnsureActionHeatTarget(int width, int height) {
  if (!s_action_heat_supported || width <= 0 || height <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_action_heat_target) &&
      s_action_heat_w == width &&
      s_action_heat_h == height)
    return s_action_heat_target;
  ArRenderDevice_DestroyTexture(&g_render_device, s_action_heat_target);
  s_action_heat_target = ArRenderTexture_Invalid();
  s_action_heat_w = width;
  s_action_heat_h = height;
  const ArRenderTextureDesc desc = {
    .width = width,
    .height = height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &desc, &s_action_heat_target)) {
    s_action_heat_w = s_action_heat_h = 0;
    DisableActionHeat("target creation");
  }
  return s_action_heat_target;
}

static void ClearActionHeatSavedState(void) {
  s_action_heat_saved_state = (ActionHeatPassState){0};
}

static bool ActionHeatMeshMatches(
    const ActionHeatMeshCache *cache, uint16_t game_frame,
    SDL_Rect viewport, int target_width, int target_height,
    int source_width) {
  return cache && cache->valid && cache->game_frame == game_frame &&
      cache->viewport.x == viewport.x && cache->viewport.y == viewport.y &&
      cache->viewport.w == viewport.w && cache->viewport.h == viewport.h &&
      cache->target_width == target_width &&
      cache->target_height == target_height &&
      cache->source_width == source_width;
}

static const ActionHeatRenderMesh *ActionHeatMeshFor(
    uint16_t game_frame, SDL_Rect viewport,
    int target_width, int target_height, int source_width) {
  if (ActionHeatMeshMatches(
          &s_action_heat_mesh_cache, game_frame, viewport,
          target_width, target_height, source_width))
    return &s_action_heat_mesh_cache.mesh;
  s_action_heat_mesh_cache.valid = false;
  if (!ActionHeatRender_Build(
          game_frame,
          (ArRenderRectI){viewport.x, viewport.y, viewport.w, viewport.h},
          target_width, target_height,
          source_width, &s_action_heat_mesh_cache.mesh))
    return NULL;
  s_action_heat_mesh_cache.viewport = viewport;
  s_action_heat_mesh_cache.target_width = target_width;
  s_action_heat_mesh_cache.target_height = target_height;
  s_action_heat_mesh_cache.source_width = source_width;
  s_action_heat_mesh_cache.game_frame = game_frame;
  s_action_heat_mesh_cache.valid = true;
  return &s_action_heat_mesh_cache.mesh;
}

/* Route the world composite into a viewport-sized texture. Letterbox pixels
 * never enter this target, which avoids reserving and clearing memory that the
 * heat pass cannot display. The matching end pass clears the real output once
 * and resolves this texture with one subtly UV-warped mesh; HUD and host
 * overlays are intentionally drawn afterward. */
static bool BeginActionHeat(const FrameSlot *slot, SDL_Rect viewport) {
  if (s_action_heat_engaged || !FrameUsesActionHeat(slot) ||
      !EffectRendererAvailable() || !s_action_heat_supported)
    return false;
  if (viewport.w <= 0 || viewport.h <= 0) {
    DisableActionHeat("invalid viewport");
    return false;
  }
  const ArRenderTexture target = EnsureActionHeatTarget(
      viewport.w, viewport.h);
  if (!ArRenderTexture_IsValid(target)) return false;
  ActionHeatPassState saved = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      &g_render_device, target, &saved.target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (begin == kArRenderTargetBegin_StateLost)
      FailActionHeatTargetState("failed-begin state restore");
    else
      DisableActionHeat("target bind");
    return false;
  }
  saved.valid = true;
  s_action_heat_saved_state = saved;
  s_action_heat_engaged = true;
  return true;
}

static SDL_Rect ActionHeatSceneViewport(SDL_Rect output_viewport) {
  if (!s_action_heat_engaged) return output_viewport;
  return (SDL_Rect){0, 0, output_viewport.w, output_viewport.h};
}

static bool ResolveFrameOutputViewport(
    const FrameSlot *slot, SDL_Rect *viewport) {
  if (!slot || !viewport) return false;
  const int aspect_width = slot->visible_width *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  const int aspect_height = slot->snes_height *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  ArRenderRectI resolved;
  if (!ArRenderOutput_ResolveAspectFit(
          &g_render_device, slot->ignore_aspect_ratio,
          aspect_width, aspect_height, &resolved, NULL, NULL))
    return false;
  *viewport = (SDL_Rect){
    resolved.x, resolved.y, resolved.w, resolved.h,
  };
  return true;
}

static void CancelActionHeat(void) {
  if (!s_action_heat_engaged) return;
  const ActionHeatPassState saved = s_action_heat_saved_state;
  const bool target_restored =
      saved.valid && ArRenderDevice_EndTarget(
          &g_render_device, &saved.target_state);
  ClearActionHeatSavedState();
  s_action_heat_engaged = false;
  if (!target_restored)
    FailActionHeatTargetState("cancel state restore");
}

static void EndActionHeat(const FrameSlot *slot, SDL_Rect viewport) {
  if (!s_action_heat_engaged) return;
  const ActionHeatPassState saved = s_action_heat_saved_state;
  ClearActionHeatSavedState();
  s_action_heat_engaged = false;
  if (!saved.valid || !ArRenderDevice_EndTarget(
          &g_render_device, &saved.target_state)) {
    FailActionHeatTargetState("target restore");
    return;
  }

  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(
          &g_render_device,
          (ArRenderRectI){viewport.x, viewport.y, viewport.w, viewport.h},
          black, black, &output_frame)) {
    DisableActionHeat("scene resolve scope");
    return;
  }
  const SDL_Rect local_viewport = {0, 0, viewport.w, viewport.h};
  const ActionHeatRenderMesh *mesh = ActionHeatMeshFor(
      slot->action_scene_effects.game_frame, local_viewport,
      s_action_heat_w, s_action_heat_h, slot->visible_width);
  const bool warped = mesh && ArRenderDevice_DrawGeometry(
      &g_render_device, s_action_heat_target,
      mesh->vertices, mesh->vertex_count,
      mesh->indices, mesh->index_count);
  bool fallback = false;
  if (!warped) {
    /* A runtime geometry rejection must drop only the enhancement, not the
     * already-rendered world. Resolve the captured scene without refraction
     * for this frame, then disable future heat attempts. */
    const ArRenderRectF destination = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    fallback = ArRenderDevice_DrawTexture(
        &g_render_device, s_action_heat_target, NULL, &destination);
  }
  if (!ArRenderOutputFrame_Finish(&output_frame)) {
    DisableActionHeat("output-state restore");
    SessionFatal_Request(
        "The action heat-refraction pass could not restore the output "
        "viewport and clip state (%s). Restart the game; if this repeats, "
        "update your graphics driver or disable action particles.",
        ArRenderDevice_LastError(&g_render_device));
  } else if (!warped)
    DisableActionHeat(fallback ? "refraction mesh" : "fallback resolve");
}

bool SubmitEffectBatch(EffectBatch *batch, ArRenderBlendMode blend) {
  if (!batch || batch->overflow) {
    static bool logged;
    if (!logged) {
      logged = true;
      fprintf(stderr,
              "[host-effects] internal geometry batch capacity exceeded — "
              "effect pass skipped\n");
    }
    return false;
  }
  if (!batch->index_count) return true;
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = blend,
  };
  if (ArRenderDevice_DrawGeometryWithState(
          &g_render_device, ArRenderTexture_Invalid(), batch->vertices,
          batch->vertex_count, batch->indices, batch->index_count, &state))
    return true;
  DisableEffectGeometry("geometry submit");
  return false;
}

/* ── Action-stage presentation effects ────────────────────────────────── */

_Static_assert(kActionEffectObjPriorityCount ==
                   kDioramaObjectPriorityCount,
               "action effects and diorama must agree on OBJ bands");

static void DrawActionEffects(const FrameSlot *slot, SDL_Rect viewport,
                              const DioramaProjection *diorama_projection) {
  if (!slot || (!slot->action_effects.visible_count &&
                !slot->action_scene_effects.visible_count &&
                !slot->action_scene_effects.decoration_visible_count) ||
      (!slot->action_effect_lighting && !slot->action_effect_particles) ||
      !EffectRendererAvailable())
    return;

  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .ws_extra_top = slot->ws_extra_top,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .diorama_projection = diorama_projection,
    .viewport = {viewport.x, viewport.y, viewport.w, viewport.h},
  };
  ActionEffectRenderBatch *geometry = &s_action_effect_render_scratch.spell;
  ActionSceneEffectRenderBatch *scene_geometry =
      &s_action_effect_render_scratch.scene;
  geometry->vertex_count = geometry->index_count = 0;
  scene_geometry->vertex_count = scene_geometry->index_count = 0;
  if ((slot->action_effects.visible_count &&
       !ActionEffectRender_Build(
           &slot->action_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection, geometry)) ||
      (slot->action_scene_effects.visible_count &&
       !ActionSceneEffectRender_Build(
           &slot->action_scene_effects, slot->action_effect_lighting,
           slot->action_effect_particles,
           ActionEffectProjection_ProjectPoint, &projection,
           scene_geometry)))
    return;
  const int actor_vertex_count = scene_geometry->vertex_count;
  const int actor_index_count = scene_geometry->index_count;

  EffectBatch spell_batch = {
    .vertices = geometry->vertices,
    .indices = geometry->indices,
    .vertex_count = geometry->vertex_count,
    .index_count = geometry->index_count,
    .vertex_capacity = kActionEffectRenderMaxVertices,
    .index_capacity = kActionEffectRenderMaxIndices,
  };
  EffectBatch scene_batch = {
    .vertices = scene_geometry->vertices,
    .indices = scene_geometry->indices,
    .vertex_count = scene_geometry->vertex_count,
    .index_count = scene_geometry->index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  bool spell_submitted = true;
  bool scene_submitted = true;
  if (spell_batch.index_count || scene_batch.index_count) {
    spell_submitted = SubmitEffectBatch(
        &spell_batch, kArRenderBlendMode_Add);
    scene_submitted = SubmitEffectBatch(
        &scene_batch, kArRenderBlendMode_Add);
  }

  /* Map-derived world decorations own a separate captured list and reuse the
   * same scratch batch after actor submission. This preserves the actor
   * budget without allocating another workspace. BG2 decorations and bottom
   * atmosphere are submitted by their dedicated depth-ordered passes. */
  bool decoration_submitted = false;
  if (slot->action_scene_effects.decoration_visible_count &&
      ActionSceneDecorationRender_Build(
          &slot->action_scene_effects,
          kActionEffectRenderLayer_WorldOverlay,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection,
          scene_geometry) && scene_geometry->index_count) {
    scene_batch.vertex_count = scene_geometry->vertex_count;
    scene_batch.index_count = scene_geometry->index_count;
    decoration_submitted = SubmitEffectBatch(
        &scene_batch, kArRenderBlendMode_Add);
  }
  /* One line, once per process: the whole path (WRAM identity -> capture ->
   * projection -> geometry submit) either produced pixels or it did not, and
   * a run's console.log should say which without anyone re-deriving it. The
   * silent version of this is what let a 16-bit read of the animation-bank
   * BYTE reject every spell with no visible symptom but "nothing happens". */
  static bool announced;
  if (!announced && geometry->index_count && spell_submitted) {
    announced = true;
    fprintf(stderr, "[action-fx] first spell geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_effects.visible_count, geometry->vertex_count,
            geometry->index_count, slot->action_effect_lighting,
            slot->action_effect_particles);
  }
  static bool announced_scene;
  if (!announced_scene && actor_index_count && scene_submitted) {
    announced_scene = true;
    fprintf(stderr,
            "[action-fx] first scene accent geometry submitted: %u effect(s), "
            "%d vertices / %d indices (lighting=%d particles=%d)\n",
            slot->action_scene_effects.visible_count,
            actor_vertex_count, actor_index_count,
            slot->action_effect_lighting, slot->action_effect_particles);
  }
  static bool announced_decorations;
  if (!announced_decorations && decoration_submitted) {
    announced_decorations = true;
    fprintf(stderr,
            "[action-fx] first map decoration geometry submitted\n");
  }
}

typedef struct ActionDioramaPlaneEffectContext {
  const FrameSlot *slot;
  SDL_Rect viewport;
} ActionDioramaPlaneEffectContext;

static void DrawActionDioramaPlaneEffect(
    void *userdata, int plane, const DioramaProjection *diorama_projection) {
  ActionDioramaPlaneEffectContext *context =
      (ActionDioramaPlaneEffectContext *)userdata;
  if (!context || !context->slot ||
      !context->slot->action_scene_effects.decoration_visible_count ||
      !diorama_projection || !EffectRendererAvailable())
    return;
  uint8_t render_layer;
  if (plane == SR_PPU_OVERLAY_BG1 &&
      diorama_projection->bg1_plane.valid) {
    render_layer = kActionEffectRenderLayer_Bg1Plane;
  } else if (plane == SR_PPU_OVERLAY_BG2 &&
             diorama_projection->bg2_plane.valid) {
    render_layer = kActionEffectRenderLayer_Bg2Plane;
  } else if (plane == kDioramaPlane_Bg1Hi &&
             diorama_projection->bg1_high_plane.valid) {
    render_layer = kActionEffectRenderLayer_Bg1HighPlane;
  } else {
    return;
  }
  const FrameSlot *slot = context->slot;
  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .ws_extra_top = slot->ws_extra_top,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    .diorama_projection = diorama_projection,
    .viewport = {
      context->viewport.x, context->viewport.y,
      context->viewport.w, context->viewport.h,
    },
  };
  ActionSceneEffectRenderBatch *geometry =
      &s_action_effect_render_scratch.scene;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, render_layer,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, geometry))
    return;
  EffectBatch batch = {
    .vertices = geometry->vertices,
    .indices = geometry->indices,
    .vertex_count = geometry->vertex_count,
    .index_count = geometry->index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  const bool submitted = geometry->index_count && SubmitEffectBatch(
      &batch, kArRenderBlendMode_Add);
  static bool announced_bg1;
  if (!announced_bg1 && submitted &&
      render_layer == kActionEffectRenderLayer_Bg1Plane) {
    announced_bg1 = true;
    fprintf(stderr,
            "[action-fx] first BG1-local decoration geometry submitted "
            "(Diorama, depth-ordered)\n");
  }
  static bool announced_bg2;
  if (!announced_bg2 && submitted &&
      render_layer == kActionEffectRenderLayer_Bg2Plane) {
    announced_bg2 = true;
    fprintf(stderr,
            "[action-fx] first BG2-local waterfall geometry submitted "
            "(Diorama)\n");
  }
  static bool announced_bg1_high;
  if (!announced_bg1_high && submitted &&
      render_layer == kActionEffectRenderLayer_Bg1HighPlane) {
    announced_bg1_high = true;
    fprintf(stderr,
            "[action-fx] first BG1-high lava geometry submitted "
            "(Diorama, depth-ordered)\n");
  }

  /* The finite-backdrop gap exists only in Diorama's vertical extension.
   * Submit its unmasked atmosphere from the same after-BG2 callback, before
   * later BG1 and OBJ planes, so source sprites remain in front. */
  if (render_layer != kActionEffectRenderLayer_Bg2Plane) return;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects,
          kActionEffectRenderLayer_Atmosphere,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, geometry) ||
      !geometry->index_count)
    return;
  batch.vertex_count = geometry->vertex_count;
  batch.index_count = geometry->index_count;
  /* Mist needs to obscure the finite BG2/skybox discontinuity, not merely
   * brighten both sides of it. Standard source-alpha blending lets the
   * staggered zero-alpha rims feather that boundary; the ordinary waterfall
   * veil and all luminous effects remain additive. */
  const bool atmosphere_submitted = SubmitEffectBatch(
      &batch, kArRenderBlendMode_Alpha);
  static bool announced_atmosphere;
  if (!announced_atmosphere && atmosphere_submitted) {
    announced_atmosphere = true;
    fprintf(stderr,
            "[action-fx] first waterfall bottom atmosphere submitted "
            "(Diorama)\n");
  }
}

static ArRenderTexture EnsureActionPlaneEffectTarget(int w, int h) {
  if (!ArRenderDevice_IsReady(&g_render_device) || w <= 0 || h <= 0)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_action_plane_effect_target) &&
      s_action_plane_effect_w == w &&
      s_action_plane_effect_h == h)
    return s_action_plane_effect_target;
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_action_plane_effect_target);
  s_action_plane_effect_target = ArRenderTexture_Invalid();
  s_action_plane_effect_w = w;
  s_action_plane_effect_h = h;
  const ArRenderTextureDesc desc = {
    .width = w,
    .height = h,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_AddPremultiplied,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &desc, &s_action_plane_effect_target)) {
    DisableActionPlaneEffect("premultiplied target creation");
  }
  return s_action_plane_effect_target;
}

static void DrawActionPlaneEffectFlat(
    const FrameSlot *slot, SDL_Rect viewport, uint8_t render_layer,
    bool mask_valid, ArRenderTexture mask_texture, const char *label) {
  if (!slot || !mask_valid ||
      !slot->action_scene_effects.decoration_visible_count ||
      !ArRenderTexture_IsValid(mask_texture) ||
      !s_action_plane_blend_supported ||
      !EffectRendererAvailable())
    return;
  ActionEffectProjectionContext projection = {
    .bg1_camera_x = slot->bg1_camera_x,
    .bg1_camera_y = slot->bg1_camera_y,
    .bg2_camera_x = slot->bg2_camera_x,
    .bg2_camera_y = slot->bg2_camera_y,
    .ws_extra = slot->ws_extra,
    .visible_x0 = slot->visible_x0,
    .visible_width = slot->visible_width,
    .snes_height = slot->snes_height,
    /* Geometry is rendered into a viewport-sized intermediate target. Keep its
     * coordinates target-local; the final composite restores the output-space
     * viewport offset below. */
    .viewport = {0, 0, viewport.w, viewport.h},
  };
  ActionSceneEffectRenderBatch *geometry =
      &s_action_effect_render_scratch.scene;
  if (!ActionSceneDecorationRender_Build(
          &slot->action_scene_effects, render_layer,
          slot->action_effect_lighting, slot->action_effect_particles,
          ActionEffectProjection_ProjectPoint, &projection, geometry) ||
      !geometry->index_count)
    return;
  const ArRenderTexture target =
      EnsureActionPlaneEffectTarget(viewport.w, viewport.h);
  if (!ArRenderTexture_IsValid(target)) return;

  ArRenderTargetState target_state = {0};
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      &g_render_device, target, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    DisableActionPlaneEffect("effect-target bind");
    return;
  }
  const bool target_ready = ArRenderDevice_Clear(
      &g_render_device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  if (!target_ready)
    DisableActionPlaneEffect("effect-target clear");
  EffectBatch batch = {
    .vertices = geometry->vertices,
    .indices = geometry->indices,
    .vertex_count = geometry->vertex_count,
    .index_count = geometry->index_count,
    .vertex_capacity = kActionSceneEffectRenderMaxVertices,
    .index_capacity = kActionSceneEffectRenderMaxIndices,
  };
  bool submitted = false;
  bool masked = false;
  if (target_ready)
    submitted = SubmitEffectBatch(&batch, kArRenderBlendMode_Add);
  if (submitted && s_action_plane_blend_supported) {
    const ArRenderRectF src = {
      (float)slot->visible_x0, 0.0f,
      (float)slot->visible_width, (float)slot->snes_height,
    };
    const ArRenderRectF dst = {
      0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
    };
    const ArRenderDrawState mask_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Multiply,
    };
    masked = ArRenderDevice_DrawTextureWithState(
        &g_render_device, mask_texture, &src, &dst, &mask_state);
    if (!masked) DisableActionPlaneEffect("winner-mask draw");
  }
  if (!ArRenderDevice_EndTarget(&g_render_device, &target_state)) {
    DisableActionPlaneEffect("render-state restore");
    return;
  }
  bool composited = false;
  if (masked && s_action_plane_blend_supported) {
    const ArRenderRectF dst = ToRenderRectF(viewport);
    composited = ArRenderDevice_DrawTexture(
        &g_render_device, target, NULL, &dst);
    if (!composited) DisableActionPlaneEffect("masked-target composite");
  }
  static bool announced[kActionEffectRenderLayer_Count];
  if (render_layer < kActionEffectRenderLayer_Count &&
      !announced[render_layer] && composited) {
    announced[render_layer] = true;
    fprintf(stderr,
            "[action-fx] first %s geometry submitted "
            "(flat, winner-masked)\n",
            label ? label : "BG-local decoration");
  }
}

/* ── Cheat visibility badge ────────────────────────────────────────────── */

/* An armed spell-cycle silently makes the game behave in a way no cartridge
 * can, which is exactly the state a screenshot or a bug report must not be
 * able to hide. Drawn last-but-one — above the game and the HUD, below the
 * settings overlay, in every presentation path — so it cannot be scrolled,
 * masked, or projected out of frame. */
static void PresentCheatBadge(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot || !slot->magic_cycle_armed) return;

  static const char *const kSpells[] = {
    "NONE", "FIRE", "STARDUST", "AURA", "LIGHT",
  };
  char text[64];
  uint8_t selected = slot->magic_cycle_selected;
  SDL_snprintf(text, sizeof(text), "CHEAT: SPELL CYCLE %s",
               selected <= 4 ? kSpells[selected] : "NONE");

  /* One glyph of inset from the viewport's top-left, at whatever scale keeps
   * the run legible on this output without ever exceeding the viewport. */
  int scale = viewport.h >= 720 ? 2 : 1;
  if (SettingsOverlay_GameTextWidth(text, scale) + 2 * kSettingsOverlayGlyphSize
      > viewport.w && scale > 1)
    scale = 1;
  int x = viewport.x + kSettingsOverlayGlyphSize;
  int y = viewport.y + kSettingsOverlayGlyphSize;
  SettingsOverlay_DrawGameText(x, y, scale, 255, text);
}

/* Developer authoring overlay for Settings > Layers > BG Extents. Segments
 * arrive in authentic-screen coordinates from the immutable plan in FrameSlot;
 * mapping them here keeps the pure row/guide model independent of SDL and keeps
 * present.c isolated from the live tuner singleton. BG1 is cyan, BG2 orange. */
static void PresentActionBgExtentGuides(const FrameSlot *slot,
                                        SDL_Rect viewport) {
  if (!slot || !slot->action_bg_extent_guides || viewport.w <= 0 ||
      viewport.h <= 0 || slot->visible_width <= 0)
    return;
  ActionBgTunerGuide guides[kActionBgTunerGuideMax];
  int count = ActionBgTuner_BuildGuides(
      &slot->action_bg_plan, guides, kActionBgTunerGuideMax);
  if (!count) return;

  const float scale_x = (float)viewport.w / (float)slot->visible_width;
  const float scale_y =
      (float)viewport.h / (float)kFrameSlotAuthenticHeight;
  const float authentic_x0 =
      ((float)slot->visible_width - (float)kFrameSlotAuthenticWidth) * 0.5f;
  for (int i = 0; i < count; i++) {
    const ActionBgTunerGuide *guide = &guides[i];
    const ArRenderColorF color = guide->layer == 0
        ? (ArRenderColorF){48.0f / 255.0f, 220.0f / 255.0f,
                           1.0f, 220.0f / 255.0f}
        : (ArRenderColorF){1.0f, 96.0f / 255.0f,
                           48.0f / 255.0f, 220.0f / 255.0f};
    float x0 = viewport.x + (authentic_x0 + guide->x0) * scale_x;
    float x1 = viewport.x + (authentic_x0 + guide->x1) * scale_x;
    float y0 = viewport.y + guide->y0 * scale_y;
    float y1 = viewport.y + guide->y1 * scale_y;
    (void)ArRenderDevice_DrawLine(
        &g_render_device, (ArRenderPointF){x0, y0},
        (ArRenderPointF){x1, y1}, 1.0f, color,
        kArRenderBlendMode_Alpha);
    /* One adjacent line remains legible over both bright and dark pixel art. */
    if (guide->x0 == guide->x1)
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0 + 1.0f, y0},
          (ArRenderPointF){x1 + 1.0f, y1}, 1.0f, color,
          kArRenderBlendMode_Alpha);
    else
      (void)ArRenderDevice_DrawLine(
          &g_render_device, (ArRenderPointF){x0, y0 + 1.0f},
          (ArRenderPointF){x1, y1 + 1.0f}, 1.0f, color,
          kArRenderBlendMode_Alpha);
  }
}

static void PresentFpsCounter(const FrameSlot *slot, SDL_Point output_size,
                              double presentation_fps) {
  enum {
    kFpsTextCapacity = 32,
    kFpsScale2OutputHeight = 720,
    kFpsScale3OutputHeight = 1440,
  };
  typedef struct FpsOverlayCache {
    char text[kFpsTextCapacity];
    double frames_per_second;
    int output_width;
    int output_height;
    int x;
    int y;
    int scale;
    bool initialized;
  } FpsOverlayCache;
  static FpsOverlayCache cache;
  if (!slot || !slot->show_fps) return;
  if (output_size.x <= 0 || output_size.y <= 0)
    return;

  const int scale = output_size.y >= kFpsScale3OutputHeight ? 3
      : output_size.y >= kFpsScale2OutputHeight ? 2
      : 1;
  const bool text_changed = !cache.initialized ||
      cache.frames_per_second != presentation_fps;
  if (text_changed) {
    if (presentation_fps > 0.0)
      SDL_snprintf(
          cache.text, sizeof(cache.text), "FPS %.1f", presentation_fps);
    else
      SDL_snprintf(cache.text, sizeof(cache.text), "FPS --.-");
    cache.frames_per_second = presentation_fps;
  }
  if (!cache.initialized || text_changed ||
      cache.output_width != output_size.x ||
      cache.output_height != output_size.y || cache.scale != scale) {
    const int margin = kSettingsOverlayGlyphSize * scale;
    cache.output_width = output_size.x;
    cache.output_height = output_size.y;
    cache.scale = scale;
    cache.x = output_size.x - margin -
        SettingsOverlay_GameTextWidth(cache.text, scale);
    cache.y = margin;
    cache.initialized = true;
  }
  SettingsOverlay_DrawGameText(
      cache.x, cache.y, cache.scale, 255, cache.text);
}

/* Terminal host UI is deliberately outside the CRT scene. One physical-output
 * coordinate space covers the inspector marker/panel, cheat disclosure,
 * manual, settings menu, and FPS counter. The next frame establishes its own
 * scene coordinates, so terminal UI deliberately leaves this scope active. */
void PresentHostUi(const FrameSlot *slot, SDL_Rect viewport,
                   SDL_Point output_size,
                   double presentation_fps) {
  if (!slot || !g_renderer) return;
  if (!ArRenderOutput_UseFull(&g_render_device, NULL, NULL)) return;
  PresentActionBgExtentGuides(slot, viewport);
  PresentSceneInspector(slot, viewport);
  PresentCheatBadge(slot, viewport);
  SettingsOverlay_Render(viewport);
  PresentFpsCounter(slot, output_size, presentation_fps);
}

bool Present_SimRimMaskSupported(void) {
  return SDL_GetAtomicInt(&s_sim_rim_mask_supported) != 0;
}

/* Called from the SDL_EVENT_RENDER_TARGETS_RESET / _DEVICE_RESET arm and once
 * during orderly shutdown.
 *
 * SDL_events.h documents _DEVICE_RESET as "The device has been reset and all
 * textures need to be recreated". This includes the size-keyed render targets
 * above as well as resources written only when a game-side serial changes
 * (underlay/canvas) or exactly once at creation (cloud noise). None of those
 * cache keys has any dependence on GPU device state, so without this call the
 * caches short-circuit forever and keep handing back textures whose contents
 * the driver discarded. In a settled town the underlay serial can stay fixed
 * indefinitely, so the damage does not self-heal; only changing town would
 * clear it.
 *
 * The symptom is already documented for this exact texture class in
 * UploadSimTownCanvas below ("it showed as magenta"): freshly reallocated
 * STREAMING storage is uninitialized. Never reproducible on macOS/Metal, which
 * does not emit _DEVICE_RESET at all — this is a Windows-D3D and
 * Vulkan/SDL_GPU (Steam Deck) bug. */
void PresentRendererResources_Reset(void) {
  ResetSim3DUploadMirrors();
  ResetActionUploadMirrors();
  ArRenderDevice_DestroyTexture(&g_render_device, s_hud_composite_texture);
  s_hud_composite_texture = ArRenderTexture_Invalid();
  s_hud_composite_w = s_hud_composite_h = 0;
  ArRenderDevice_DestroyTexture(&g_render_device, s_action_bg1_mask_texture);
  ArRenderDevice_DestroyTexture(&g_render_device, s_action_bg2_mask_texture);
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_action_plane_effect_target);
  ArRenderDevice_DestroyTexture(&g_render_device, s_action_heat_target);
  s_action_bg1_mask_texture = ArRenderTexture_Invalid();
  s_action_bg2_mask_texture = ArRenderTexture_Invalid();
  s_action_plane_effect_target = ArRenderTexture_Invalid();
  s_action_plane_effect_w = s_action_plane_effect_h = 0;
  s_action_plane_blend_supported = true;
  s_action_heat_target = ArRenderTexture_Invalid();
  ClearActionHeatSavedState();
  s_action_heat_mesh_cache = (ActionHeatMeshCache){0};
  s_action_heat_w = s_action_heat_h = 0;
  s_action_heat_supported = true;
  s_action_heat_engaged = false;
  SDL_SetAtomicInt(&s_effect_blend_supported, 1);
  SDL_SetAtomicInt(&s_effect_geometry_supported, 1);
  DioramaFrameGeneration_Reset();
  PresentSim3D_ResetResources();
}

void PresentCompositeScene(const FrameSlot *slot, float alpha) {
  if (!g_renderer || !ArRenderTexture_IsValid(g_texture)) return;

  /* The action map group becomes live while the world-to-action transition
   * is still holding the SNES in hardware forced blank. That makes Diorama's
   * host-side gate true before the first action frame is actually visible.
   * Unlike the ordinary PPU scanout, Diorama does not pass through INIDISP:
   * its navy clear, shoebox, skybox, HUD, and host overlays would therefore
   * leak through an otherwise fully blank transition (the gf=976 snapshot is
   * the captured example). Treat forced blank as the master output gate it is
   * on hardware and return before drawing any host-owned layer or overlay. */
  if (slot->diorama_active && (slot->inidisp & 0x80)) {
    const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!ArRenderDevice_SetRenderTarget(
            &g_render_device, CrtPost_BaseTarget()) ||
        !ArRenderDevice_UseOutputCoordinates(&g_render_device) ||
        !ArRenderDevice_SetViewport(&g_render_device, NULL) ||
        !ArRenderDevice_SetClipRect(&g_render_device, NULL) ||
        !ArRenderDevice_Clear(&g_render_device, black)) {
      SessionFatal_Request(
          "The renderer could not clear its scene target for forced blank "
          "(%s). Restart the game; if this repeats, update your graphics "
          "driver.", ArRenderDevice_LastError(&g_render_device));
      return;
    }
    return;
  }

  if (slot->sim.view == kSimView_Enhanced && slot->sim.separated_valid) {
    SDL_ClearError();
    const PresentationOutcome sim = PresentSim3D(slot);
    if (!PresentationOutcome_IsUsable(sim)) {
      const char *sdl_error = SDL_GetError();
      SessionFatal_Request(
          "The enhanced SIM renderer lost its active frame state (%s). "
          "Restart the game. If this happens again, update your graphics "
          "driver or select a different SDL renderer.",
          sdl_error[0] ? sdl_error : "renderer target/state restore failed");
    }
    return;
  }
  if (slot->sim.view == kSimView_WorldNavigation) {
    SDL_ClearError();
    const PresentationOutcome navigation = PresentWorldNavigation3D(slot);
    if (PresentationOutcome_IsUsable(navigation)) return;
    const char *sdl_error = SDL_GetError();
    SessionFatal_Request(
        "The enhanced world-navigation renderer failed while it was active "
        "(%s). Restart the game. If this happens again, update your graphics "
        "driver or disable enhanced world navigation before entering the map.",
        sdl_error[0] ? sdl_error : "invalid renderer frame state");
    return;
  }

  if (slot->diorama_active) {
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
      current_textures[plane] = g_diorama_textures[plane];
    DioramaPerformanceScope frame_synthesis =
        DioramaPerformance_Begin(kDioramaPerformance_FrameSynthesis);
    (void)DioramaFrameGeneration_Prepare(
        &g_render_device, slot, alpha, current_textures,
        s_diorama_uploaded_plane_mask, scene_textures);
    DioramaPerformance_End(frame_synthesis);
    /* The existing graphics setting now selects frame-space generation.
     * Prepare fails individual planes closed when either endpoint or pair
     * continuity is unavailable, leaving their current raw textures intact. */
    /* B4-split (followup doc): resolve which authored pose is active this
     * frame. Free Cam: the live authored pose, unchanged from B4-split.
     * Dynamic Cam (B4-vellean): baseline + a small velocity-driven lean —
     * yaw toward horizontal run direction, pitch with vertical velocity —
     * scaled by reactive_strength/kPercentScale (0 disables sway, reproducing
     * B4-baseline's "snaps to the fixed pose" test). */
    bool dynamic = slot->diorama_camera_mode == kDioramaCam_Dynamic;
    DioramaCameraPose target;
    if (dynamic) {
      float gain =
          (float)slot->diorama_reactive_strength / (float)kPercentScale;
      target = slot->diorama_dyncam_baseline;
      target.tilt_y += kDioramaLeanYaw * gain * slot->diorama_dyncam_lean_yaw;
      target.tilt_x += kDioramaLeanPitch * gain * slot->diorama_dyncam_lean_pitch;
    } else {
      target = slot->diorama_free_pose;
    }

    /* B4-damp: Free Cam stays a direct snap (manual orbit must feel
     * immediate, and this preserves B4-split's byte-identical regression
     * test). Dynamic Cam eases toward the target with a wall-clock
     * exponential — NOT a fixed per-frame factor, since B1a makes the
     * present rate monitor-dependent and a fixed factor would be twice as
     * stiff at 120Hz as at 60Hz. The one exception: the frame a mode change
     * lands on (or the very first composited frame) snaps immediately —
     * that's what makes switching TO Dynamic Cam snap straight to the
     * baseline pose (already verified in B4-baseline) rather than easing in
     * from wherever Free Cam was left. */
    bool mode_changed = g_diorama_render_cam_mode != slot->diorama_camera_mode;
    g_diorama_render_cam_mode = slot->diorama_camera_mode;
    uint64_t now_ns = SDL_GetTicksNS();
    float dt = 0.0f;
    if (g_diorama_render_cam_last_ns != 0) {
      dt = (float)(now_ns - g_diorama_render_cam_last_ns) / 1e9f;
      if (dt < 0.0f) dt = 0.0f;
      if (dt > 1.0f) dt = 1.0f;   /* sanity clamp (e.g. resuming after a pause) */
    }
    if (!dynamic || mode_changed || g_diorama_render_cam_last_ns == 0) {
      g_diorama_render_cam = target;
    } else {
      float damping_alpha = 1.0f - expf(-dt / kDioramaDampTau);
      g_diorama_render_cam.tilt_x +=
          (target.tilt_x - g_diorama_render_cam.tilt_x) * damping_alpha;
      g_diorama_render_cam.tilt_y +=
          (target.tilt_y - g_diorama_render_cam.tilt_y) * damping_alpha;
      g_diorama_render_cam.distance +=
          (target.distance - g_diorama_render_cam.distance) * damping_alpha;
    }
    g_diorama_render_cam_last_ns = now_ns;

    /* B4-kick: trigger a fresh impulse only on a genuinely NEW FrameSlot
     * capture (not a re-presentation of one already processed—see
     * the FrameSlot field comment, present.h), and only in Dynamic Cam
     * (event kicks are part of the reactive system, same scoping as
     * vellean/pan). Impulses stack additively (a hit while already mid-jolt
     * gets stronger, not replaced) so back-to-back events still read. Decay
     * runs every present frame regardless, on the same wall-clock exponential
     * basis as the position damping above. */
    bool new_slot = dynamic && slot->timestamp_ns != g_diorama_last_slot_ns;
    g_diorama_last_slot_ns = slot->timestamp_ns;
    if (new_slot) {
      float gain =
          (float)slot->diorama_reactive_strength / (float)kPercentScale;
      if (slot->diorama_dyncam_event_hit || slot->diorama_dyncam_event_land)
        g_diorama_kick_pitch += kDioramaKickPitch * gain;
      /* Hit gets the zoom-punch too (see the section comment above) — a
       * discrete, reliable edge, unlike PlayerBoost. */
      if (slot->diorama_dyncam_event_hit)
        g_diorama_kick_zoom += kDioramaKickZoom * gain;
      /* DISABLED (2026-07-21, live report): PlayerBoost ($08C4) fired
       * constantly while just holding a direction — it isn't a clean
       * "boost activated" edge the way the invuln bit is for hits; more
       * likely a counter/cycling value that's nonzero (or repeatedly
       * revisits zero) during ordinary movement, not a discrete ability
       * trigger. slot->diorama_dyncam_event_boost is still captured
       * (FrameSlot/AR_DYNCAM_LOG's evt(boost=...) field) for whenever this
       * gets revisited with real investigation into what the byte means. */
    }
    if (!dynamic) {
      g_diorama_kick_pitch = 0.0f;
      g_diorama_kick_zoom = 0.0f;
    } else if (dt > 0.0f) {
      float kick_decay = expf(-dt / kDioramaKickTau);
      g_diorama_kick_pitch *= kick_decay;
      g_diorama_kick_zoom *= kick_decay;
    }
    DioramaCameraPose final_cam = g_diorama_render_cam;
    float distance_scale = 1.0f;
    if (dynamic) {
      final_cam.tilt_x += g_diorama_kick_pitch +
          slot->diorama_manual_orbit_pitch;
      final_cam.tilt_y += slot->diorama_manual_orbit_yaw;
      distance_scale = 1.0f + g_diorama_kick_zoom;
    }

    /* AR_DYNCAM_LOG=1: diagnose "no visible sway" reports — prints the raw
     * self-calibrated lean signal, the gain, the computed target, and the
     * actual (possibly still-damping) render camera every present, same
     * pattern as AR_INTERP_LOG above. */
    static int dyncam_log_on = -1;
    if (dyncam_log_on < 0) {
      const char *e = getenv("AR_DYNCAM_LOG");
      dyncam_log_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (dyncam_log_on && dynamic) {
      fprintf(stderr,
        "[dyncam] mode=%d gain=%.3f lean_yaw=%.3f lean_pitch=%.3f "
        "target(x=%.4f y=%.4f d=%.3f) render(x=%.4f y=%.4f d=%.3f) "
        "kick(pitch=%.4f zoom=%.4f) evt(hit=%d land=%d boost=%d)\n",
        slot->diorama_camera_mode,
        (double)slot->diorama_reactive_strength / (double)kPercentScale,
        (double)slot->diorama_dyncam_lean_yaw,
        (double)slot->diorama_dyncam_lean_pitch,
        (double)target.tilt_x, (double)target.tilt_y,
        (double)target.distance,
        (double)g_diorama_render_cam.tilt_x,
        (double)g_diorama_render_cam.tilt_y,
        (double)g_diorama_render_cam.distance,
        (double)g_diorama_kick_pitch, (double)g_diorama_kick_zoom,
        slot->diorama_dyncam_event_hit, slot->diorama_dyncam_event_land,
        slot->diorama_dyncam_event_boost);
    }

    /* Fix B/BH6: resolve BG2's row-banded valid capture spans from the slot
     * alone (D6 — this file never reads live g_ppu). ws_extra, not
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
    SDL_Rect output_viewport;
    if (!ResolveFrameOutputViewport(slot, &output_viewport)) {
      DioramaPerformance_End(presentation_performance);
      DioramaPerformance_PresentCompleted();
      SessionFatal_Request(
          "The renderer could not resolve the Diorama output viewport (%s). "
          "Restart the game; if this repeats, update your graphics driver.",
          ArRenderDevice_LastError(&g_render_device));
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
        (slot->action_effect_lighting || slot->action_effect_particles)
            ? ActionEffectProjection_RequiredBgPlaneMask(
                  &slot->action_effects, &slot->action_scene_effects)
            : 0;
    const uint32_t effect_bg_plane_mask =
        Diorama_FilterBgEffectProjectionMask(
            required_effect_bg_planes,
            slot->diorama_plane_request_mask,
            slot->diorama_plane_content_mask,
            s_diorama_uploaded_plane_mask);
    (void)BeginActionHeat(slot, output_viewport);
    const SDL_Rect viewport = ActionHeatSceneViewport(output_viewport);
    const ArRenderRectI diorama_viewport = {
      viewport.x, viewport.y, viewport.w, viewport.h,
    };
    ActionDioramaPlaneEffectContext plane_effect = {slot, viewport};
    SDL_ClearError();
    const PresentationOutcome diorama = Diorama_Composite(
        &g_render_device, slot->snes_width,
        slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom,
        slot->ws_extra_top, slot->obj_apron,
        slot->pixel_aspect, slot->ignore_aspect_ratio,
        slot->visible_width, diorama_viewport, scene_textures, pixels,
        slot->diorama_bg_transparent_fill_configured,
        slot->diorama_bg_transparent_fill_argb,
        &final_cam, distance_scale,
        slot->diorama_plane_additive_mask & s_diorama_uploaded_plane_mask,
        effect_obj_priority_mask, effect_bg_plane_mask,
        slot->diorama_map_group, slot->diorama_map_number,
        slot->diorama_layer_section, &bg2_valid_spans,
        DrawActionDioramaPlaneEffect, &plane_effect, &action_projection);
    if (!PresentationOutcome_IsUsable(diorama)) {
      char renderer_error[256];
      snprintf(renderer_error, sizeof(renderer_error), "%s", SDL_GetError());
      CancelActionHeat();
      DioramaPerformance_End(presentation_performance);
      DioramaPerformance_PresentCompleted();
      SessionFatal_Request(
          "The selected Diorama renderer could not complete its core scene "
          "(%s). Restart the game. If this happens again, update your "
          "graphics driver or disable Diorama mode before entering the room.",
          renderer_error[0] ? renderer_error
                            : "invalid renderer frame state");
      return;
    }
    DioramaPerformanceScope callback_performance =
        DioramaPerformance_Begin(kDioramaPerformance_Callback);
    DrawActionEffects(slot, viewport, &action_projection);
    DioramaPerformance_End(callback_performance);
    EndActionHeat(slot, output_viewport);
    /* Flat HUD mode leaves BG3 in the same RemoveFromGame capture used by flat
     * presentation. Reconstruct its split pieces into one texture before
     * drawing the screen-space overlay; drawing them directly creates seams
     * (see PresentHudOverlayComposited).
     *
     * With diorama_hud_flat off, capture rebinds BG3 into the diorama layer
     * buffer so it renders as the ordinary tilted BG3 plane in
     * Diorama_Composite's own
     * per-layer loop above — skip the anchored overlay entirely here so the
     * two don't both draw a HUD. */
    if (slot->diorama_hud_flat)
      PresentHudOverlayComposited(slot, output_viewport);
    DioramaPerformance_End(presentation_performance);
    DioramaPerformance_PresentCompleted();
    return;
  }

  SDL_Rect output_viewport;
  if (!ResolveFrameOutputViewport(slot, &output_viewport)) {
    SessionFatal_Request(
        "The renderer could not resolve the game output viewport (%s). "
        "Restart the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }
  (void)BeginActionHeat(slot, output_viewport);
  const SDL_Rect viewport = ActionHeatSceneViewport(output_viewport);
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(
          &g_render_device,
          (ArRenderRectI){viewport.x, viewport.y, viewport.w, viewport.h},
          black, black, &output_frame)) {
    CancelActionHeat();
    SessionFatal_Request(
        "The renderer could not begin the game scene output (%s). Restart "
        "the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }
  const SDL_Rect local_viewport = {0, 0, viewport.w, viewport.h};
  SDL_Rect src = { slot->visible_x0, 0, slot->visible_width, slot->snes_height };
  ArRenderRectF source = {
    (float)src.x, (float)src.y, (float)src.w, (float)src.h,
  };
  const ArRenderRectF destination = {
    0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
  };
  if (!ArRenderDevice_DrawTexture(
          &g_render_device, g_texture, &source, &destination)) {
    ArRenderOutputFrame_Abort(&output_frame);
    CancelActionHeat();
    SessionFatal_Request(
        "The renderer rejected the base game framebuffer (%s). Restart the "
        "game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }

  PresentMode7Composite(slot, local_viewport);
  DrawActionPlaneEffectFlat(
      slot, local_viewport, kActionEffectRenderLayer_Bg1Plane,
      slot->action_bg1_mask_valid, s_action_bg1_mask_texture,
      "BG1-local decoration");
  DrawActionPlaneEffectFlat(
      slot, local_viewport, kActionEffectRenderLayer_Bg1HighPlane,
      slot->action_bg1_mask_valid, s_action_bg1_mask_texture,
      "BG1-high lava decoration");
  DrawActionPlaneEffectFlat(
      slot, local_viewport, kActionEffectRenderLayer_Bg2Plane,
      slot->action_bg2_mask_valid, s_action_bg2_mask_texture,
      "BG2-local waterfall");
  DrawActionEffects(slot, local_viewport, NULL);
  if (!ArRenderOutputFrame_Finish(&output_frame)) {
    CancelActionHeat();
    SessionFatal_Request(
        "The renderer could not restore the output after drawing the game "
        "scene (%s). Restart the game; if this repeats, update your graphics "
        "driver.", ArRenderDevice_LastError(&g_render_device));
    return;
  }
  EndActionHeat(slot, output_viewport);
  if (SessionFatal_Requested()) return;
  PresentHudOverlay(slot, output_viewport);
  PresentHdReplacements(slot, output_viewport);
}

bool PresentAuthenticScene(const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot || !ArRenderDevice_IsReady(&g_render_device) ||
      !ArRenderTexture_IsValid(g_authentic_texture) ||
      viewport.w <= 0 || viewport.h <= 0)
    return false;
  /* The orchestrator owns the current target: this may be either the window
   * backbuffer or the CRT scene target. Authentic comparison changes the game
   * image, not the player's independent display treatment. */
  if (!ArRenderDevice_SetClipRect(&g_render_device, NULL))
    return false;
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(
          &g_render_device,
          (ArRenderRectI){viewport.x, viewport.y, viewport.w, viewport.h},
          black, black, &output_frame))
    return false;
  const ArRenderRectF source = {
    (float)slot->authentic_x0, (float)slot->authentic_y0,
    (float)kFrameSlotAuthenticWidth, (float)kFrameSlotAuthenticHeight,
  };
  const ArRenderRectF destination = {
    0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
  };
  if (!ArRenderDevice_DrawTexture(
          &g_render_device, g_authentic_texture, &source, &destination)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return false;
  }
  return ArRenderOutputFrame_Finish(&output_frame);
}

bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      SDL_Rect priority_viewport) {
  if (!slot || !g_renderer ||
      !ArRenderTexture_IsValid(g_authentic_texture) ||
      priority_viewport.w <= 0 || priority_viewport.h <= 0)
    return false;
  if (!ArRenderOutput_UseFull(&g_render_device, NULL, NULL))
    return false;

  enum { kPipWidthPercent = 31, kPipMarginPercent = 3 };
  const int frame_scale = priority_viewport.h >= 1080 ? 3
      : priority_viewport.h >= 600 ? 2 : 1;
  int ratio_unit =
      priority_viewport.w * kPipWidthPercent / 100 / (4 * frame_scale);
  const int maximum_height = priority_viewport.h * 38 / 100;
  const int maximum_ratio_unit = maximum_height / (3 * frame_scale);
  if (ratio_unit > maximum_ratio_unit) ratio_unit = maximum_ratio_unit;
  /* The dialog frame repeats exact 8x8 ROM tiles. Keeping the 4:3 ratio unit
   * on that grid prevents a partial edge tile from being stretched or hidden
   * beneath a corner. */
  ratio_unit -= ratio_unit % 8;
  if (ratio_unit < 8) ratio_unit = 8;
  const int width = ratio_unit * 4 * frame_scale;
  const int height = ratio_unit * 3 * frame_scale;
  const int frame_size = kSettingsOverlayGlyphSize * frame_scale;
  int margin = priority_viewport.h * kPipMarginPercent / 100;
  if (margin < frame_size + 8) margin = frame_size + 8;
  const ArRenderRectF destination = {
    (float)(priority_viewport.x + priority_viewport.w - margin - width),
    (float)(priority_viewport.y + priority_viewport.h - margin - height),
    (float)width, (float)height,
  };
  const SDL_Rect frame = {
    (int)destination.x - frame_size,
    (int)destination.y - frame_size,
    width + frame_size * 2,
    height + frame_size * 2,
  };
  const int shadow_offset = frame_scale * 4;
  const ArRenderRectF shadow = {
    (float)(frame.x + shadow_offset),
    (float)(frame.y + shadow_offset),
    (float)frame.w, (float)frame.h,
  };
  bool rendered =
      ArRenderDevice_DrawSolidRect(
          &g_render_device, &shadow,
          (ArRenderColorF){0.0f, 0.0f, 0.0f, 150.0f / 255.0f},
          kArRenderBlendMode_Alpha) &&
      SettingsOverlay_DrawGameFrame(frame, frame_scale) &&
      ArRenderDevice_DrawSolidRect(
          &g_render_device, &destination,
          (ArRenderColorF){0.0f, 0.0f, 0.0f, 1.0f},
          kArRenderBlendMode_Opaque);
  const ArRenderRectF source = {
    (float)slot->authentic_x0, (float)slot->authentic_y0,
    (float)kFrameSlotAuthenticWidth, (float)kFrameSlotAuthenticHeight,
  };
  if (rendered)
    rendered = ArRenderDevice_DrawTexture(
        &g_render_device, g_authentic_texture, &source, &destination);
  return rendered;
}

bool PresentComparisonTransitionOverlay(uint8_t alpha, const char *label) {
  if (!alpha) return true;
  if (!g_renderer) return false;
  int width = 0, height = 0;
  bool rendered = ArRenderOutput_UseFull(
      &g_render_device, &width, &height);
  const ArRenderRectF full_output = {
    0.0f, 0.0f, (float)width, (float)height,
  };
  if (rendered)
    rendered = ArRenderDevice_DrawSolidRect(
        &g_render_device, &full_output,
        (ArRenderColorF){0.0f, 0.0f, 0.0f, (float)alpha / 255.0f},
        kArRenderBlendMode_Alpha);
  if (rendered && label && alpha >= 240 && width > 0 && height > 0) {
    int scale = height >= 1080 ? 6 : height >= 720 ? 5
        : height >= 480 ? 4 : height >= 240 ? 3 : 2;
    int text_width = SettingsOverlay_GameTextWidth(label, scale);
    while (scale > 1 && text_width > width * 3 / 4) {
      scale--;
      text_width = SettingsOverlay_GameTextWidth(label, scale);
    }
    SettingsOverlay_DrawGameText(
        (width - text_width) / 2,
        (height - kSettingsOverlayGlyphSize * scale) / 2,
        scale, 255, label);
  }
  return rendered;
}
