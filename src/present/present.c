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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "action/action_bg_tuner.h"
#include "present/present.h"
#include "sim/menu/present_sim_menu.h"
#include "sim/world_nav/present_sky_palace.h"
#include "sim/sim3d/sim3d_textures.h"
#include "action/present_action_effects.h"
#include "render/effect_batch.h"
#include "constants.h"
#include "render/crt_post.h"
#include "snesrecomp/game/types.h"
#include "diorama/present_diorama.h"
#include "present/presentation_surface.h"
#include "replacements/hd_replacement_host.h"
#include "replacements/present_hd_replacements.h"
#include "settings_overlay/settings_overlay_render.h"
#include "dev/present_scene_inspector.h"
#include "render/render_capabilities.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim3d/sim3d_performance.h"
#include "app/performance_metrics.h"
#include "app/performance_overlay.h"

/* Pixel-aspect enum constants only; never live settings. */
#include "app/settings.h"
#include "present/present_internal.h"
#include "sim/sim3d/present_sim3d.h"
#include "sim/world_nav/present_world_nav.h"
#include "sim/world_nav/present_world_nav_composition.h"
#include "present/render_comparison.h"
#include "app/session_fatal.h"
#include "present/presentation_upload_mirror.h"
#include "render/presentation_layout.h"
#include "render/render_output.h"
#include "render/present_hud.h"
#include "host/host_video.h"
#include "present/presentation_textures.h"

/* Streaming textures retain their last successfully uploaded pixels. Exact CPU
 * mirrors let static presentation surfaces cost no bus upload and locally
 * animated surfaces update only their changed bounding rectangle. Byte-exact
 * comparison avoids making rendering correctness depend on a hash. */
enum {
  kActionUploadSurface_Frame,
  kActionUploadSurface_Authentic,
  kActionUploadSurface_Count,
};

static PresentationUploadMirror
    s_action_upload_mirrors[kActionUploadSurface_Count];
static uint64_t s_authentic_uploaded_frame_serial;

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

/* The separated SIM profile consumes its published planes (or their flat
 * separated composite), never the ordinary PPU composite texture.  Keep this
 * decision at the presentation layer: the PPU still produces the authentic
 * frame and capture planes it owns, while the backend only sees uploads for
 * resources the selected compositor can actually sample.  The upload mirror
 * remains untouched while skipped, so returning to flat presentation detects
 * and synchronizes every intervening change on the next frame. */
static bool PresentationConsumesMainPpuTexture(const FrameSlot *slot) {
  if (!slot) return false;
  return !(slot->sim.view == kSimView_Enhanced &&
           slot->sim.separated_valid &&
           (slot->sim.effective_features &
            kSimFeature_SeparatedComposite) != 0);
}


ArRenderRectI ComputePresentationViewport(
    ArRenderDevice *device, bool ignore_aspect_ratio,
    int pixel_aspect, int visible_width, int snes_height) {
  return ComputePresentationViewportWithOutput(
      device, ignore_aspect_ratio, pixel_aspect, visible_width,
      snes_height, NULL);
}

ArRenderRectI ComputePresentationViewportWithOutput(
    ArRenderDevice *device, bool ignore_aspect_ratio,
    int pixel_aspect, int visible_width, int snes_height,
    ArRenderExtentI *output_size) {
  int out_w = 0, out_h = 0;
  if (!ArRenderDevice_GetOutputSize(device, &out_w, &out_h)) {
    if (output_size) *output_size = (ArRenderExtentI){0};
    return (ArRenderRectI){0};
  }
  if (output_size)
    *output_size = (ArRenderExtentI){out_w, out_h};
  return ArPresentationLayout_ResolveViewport(
      out_w, out_h, ignore_aspect_ratio,
      pixel_aspect == kPixelAspect_Crt43, visible_width, snes_height);
}

void PresentUpload(const FrameSlot *slot) {
  if (!ArRenderDevice_IsReady(&g_render_device) ||
      !ArRenderTexture_IsValid(g_texture)) {
    PresentSkyPalace_Upload(NULL, slot);
    PresentDiorama_Upload(NULL, slot);
    return;
  }
  Sim3DPerformanceScope performance = {0};
  if (slot->sim.view == kSimView_Enhanced)
    performance = Sim3DPerformance_Begin(kSim3DPerformance_Upload);

  if (ArRenderTexture_IsValid(g_authentic_texture) &&
      slot->authentic_frame_serial) {
    const int authentic_height = slot->snes_height + slot->ws_extra_top +
                                 slot->ws_extra_bottom;
    const SrPpuSurfaceView *surface =
        PresentationSurface_Bound(&slot->ppu_surfaces.authentic);
    const uint8_t *pixels = PresentationSurface_Region(
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
        SessionFatal_Request(
            "Authentic comparison could not upload its current native frame "
            "(%s). Restart the game; if this repeats, update your graphics "
            "driver or select a different renderer.",
            ArRenderDevice_LastError(&g_render_device));
      }
    }
  }

  PresentDiorama_Upload(&g_render_device, slot);
  if (!slot->diorama_active && PresentationConsumesMainPpuTexture(slot)) {
    ArRenderRectI upload = {
      0, 0, slot->snes_width, FrameSlot_CaptureHeight(slot),
    };
    const SrPpuSurfaceView *surface =
        PresentationSurface_Bound(&slot->ppu_surfaces.main);
    /* The main view reports the physical column for screen x=0. Upload starts
     * at screen x=-ws_extra, leaving any resolve apron outside the texture. */
    const int source_x = surface ? surface->origin_x - slot->ws_extra : -1;
    const int source_y = surface ? surface->origin_y - slot->ws_extra_top : -1;
    const uint8_t *pixels = PresentationSurface_Region(
        surface, source_x, source_y, upload.w, upload.h);
    if (pixels)
      UploadChangedSurface(
          g_texture,
          &s_action_upload_mirrors[kActionUploadSurface_Frame],
          pixels, upload.w, upload.h, (int)surface->pitch_bytes,
          upload.x, upload.y);
  }

  PresentSkyPalace_Upload(&g_render_device, slot);

  const SrPpuSurfaceView *bg1_surface =
      PresentationSurface_Bound(
          &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG1][0]);
  const SrPpuSurfaceView *bg2_surface =
      PresentationSurface_Bound(
          &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0]);
  if (!slot->diorama_active && slot->action_bg1_mask_valid &&
      PresentationSurface_Holds(bg1_surface, slot->snes_width, FrameSlot_CaptureHeight(slot))) {
    const uint64_t bytes = PresentActionEffects_UploadMask(
        &g_render_device, SR_PPU_OVERLAY_BG1, slot,
        bg1_surface->data, (int)bg1_surface->pitch_bytes);
    if (bytes) Sim3DPerformance_AddUpload(bytes);
  }
  if (!slot->diorama_active && slot->action_bg2_mask_valid &&
      PresentationSurface_Holds(bg2_surface, slot->snes_width, FrameSlot_CaptureHeight(slot))) {
    const uint64_t bytes = PresentActionEffects_UploadMask(
        &g_render_device, SR_PPU_OVERLAY_BG2, slot,
        bg2_surface->data, (int)bg2_surface->pitch_bytes);
    if (bytes) Sim3DPerformance_AddUpload(bytes);
  }

  const PresentHudUploadResult hud = PresentHud_Upload(&g_render_device, slot);
  if (hud.background_bytes) Sim3DPerformance_AddUpload(hud.background_bytes);
  if (hud.object_bytes) Sim3DPerformance_AddUpload(hud.object_bytes);

  const uint64_t mode7_bytes = PresentHdReplacements_UploadMode7(
      &g_render_device, HdReplacementHost_Mode7Texture(), slot);
  if (mode7_bytes) Sim3DPerformance_AddUpload(mode7_bytes);

  Sim3DTextures_Upload(&g_render_device, slot);
  UploadWorldNavigationComposition(slot);
  Sim3DPerformance_End(performance);
}

bool Present_ResolveOutputViewport(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI *viewport) {
  if (!slot || !viewport) return false;
  const int aspect_width = slot->visible_width *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  const int aspect_height = FrameSlot_VisibleHeight(slot) *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  ArRenderRectI resolved;
  if (!ArRenderOutput_ResolveAspectFit(
          device, slot->ignore_aspect_ratio,
          aspect_width, aspect_height, &resolved, NULL, NULL))
    return false;
  *viewport = resolved;
  return true;
}

/* ── Cheat visibility badge ────────────────────────────────────────────── */

/* An armed spell-cycle silently makes the game behave in a way no cartridge
 * can, which is exactly the state a screenshot or a bug report must not be
 * able to hide. Drawn last-but-one — above the game and the HUD, below the
 * settings overlay, in every presentation path — so it cannot be scrolled,
 * masked, or projected out of frame. */
static void PresentCheatBadge(const FrameSlot *slot,
                              ArRenderRectI viewport) {
  if (!slot || !slot->magic_cycle_armed) return;

  static const char *const kSpells[] = {
    "NONE", "FIRE", "STARDUST", "AURA", "LIGHT",
  };
  char text[64];
  uint8_t selected = slot->magic_cycle_selected;
  snprintf(text, sizeof(text), "CHEAT: SPELL CYCLE %s",
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
 * mapping them here keeps the pure row/guide model backend-neutral and keeps
 * present.c isolated from the live tuner singleton. BG1 is cyan, BG2 orange. */
static void PresentActionBgExtentGuides(const FrameSlot *slot,
                                        ArRenderRectI viewport) {
  if (!slot || !slot->action_bg_extent_guides || viewport.w <= 0 ||
      viewport.h <= 0 || slot->visible_width <= 0)
    return;
  ActionBgTunerGuide guides[kActionBgTunerGuideMax];
  int count = ActionBgTuner_BuildGuides(
      &slot->action_bg_plan, guides, kActionBgTunerGuideMax);
  if (!count) return;

  const float scale_x = (float)viewport.w / (float)slot->visible_width;
  const float scale_y =
      (float)viewport.h / (float)FrameSlot_VisibleHeight(slot);
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
    float y0 = viewport.y + (slot->visible_top + guide->y0) * scale_y;
    float y1 = viewport.y + (slot->visible_top + guide->y1) * scale_y;
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

static void PresentFpsCounter(const FrameSlot *slot, ArRenderExtentI output_size,
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
  if (output_size.width <= 0 || output_size.height <= 0)
    return;

  const int scale = output_size.height >= kFpsScale3OutputHeight ? 3
      : output_size.height >= kFpsScale2OutputHeight ? 2
      : 1;
  const bool text_changed = !cache.initialized ||
      cache.frames_per_second != presentation_fps;
  if (text_changed) {
    if (presentation_fps > 0.0)
      snprintf(
          cache.text, sizeof(cache.text), "FPS %.1f", presentation_fps);
    else
      snprintf(cache.text, sizeof(cache.text), "FPS --.-");
    cache.frames_per_second = presentation_fps;
  }
  if (!cache.initialized || text_changed ||
      cache.output_width != output_size.width ||
      cache.output_height != output_size.height || cache.scale != scale) {
    const int margin = kSettingsOverlayGlyphSize * scale;
    cache.output_width = output_size.width;
    cache.output_height = output_size.height;
    cache.scale = scale;
    cache.x = output_size.width - margin -
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
void PresentHostUi(const FrameSlot *slot, ArRenderRectI viewport,
                   ArRenderExtentI output_size,
                   double presentation_fps) {
  if (!slot || !ArRenderDevice_IsReady(&g_render_device)) return;
  if (!ArRenderOutput_UseFull(&g_render_device, NULL, NULL)) return;
  PresentActionBgExtentGuides(slot, viewport);
  PresentSceneInspector_Draw(slot, viewport);
  PresentCheatBadge(slot, viewport);
  const PerformanceScope settings = PerformanceMetrics_Begin(kPerformance_SettingsUi);
  SettingsOverlay_Render(viewport);
  PerformanceMetrics_End(settings);
  PresentFpsCounter(slot, output_size, presentation_fps);
  if (slot->performance_overlay) {
    const PerformanceScope overlay = PerformanceMetrics_Begin(kPerformance_Overlay);
    PerformanceSnapshot snapshot;
    PerformanceMetrics_Snapshot(&snapshot);
    if (!PerformanceOverlay_Render(&g_render_device, &snapshot,
            slot->performance_overlay, output_size))
      SessionFatal_Request("Performance overlay could not restore the render target.");
    PerformanceMetrics_End(overlay);
  } else {
    PerformanceOverlay_Reset(&g_render_device);
  }
}

bool Present_EffectRendererSupported(void) {
  return EffectRenderer_Available();
}

/* Called from the host render-target/device-reset event handlers and once
 * during orderly shutdown. A device reset invalidates every texture, including
 * the size-keyed render targets
 * above as well as resources written only when a game-side serial changes
 * (underlay/canvas) or exactly once at creation (cloud noise). None of those
 * cache keys has any dependence on GPU device state, so without this call the
 * caches short-circuit forever and keep handing back textures whose contents
 * the driver discarded. In a settled town the underlay serial can stay fixed
 * indefinitely, so the damage does not self-heal; only changing town would
 * clear it.
 *
 * Freshly reallocated STREAMING storage is uninitialized and needs a complete
 * upload before any partial updates. Never reproducible on macOS/Metal, which
 * does not emit _DEVICE_RESET at all — this is a Windows-D3D and
 * Vulkan-backed (Steam Deck) bug. */
void PresentRendererResources_Reset(void) {
  PresentDiorama_Reset(&g_render_device);
  PerformanceOverlay_Reset(&g_render_device);
  Sim3DTextures_ResetUploads();
  ResetActionUploadMirrors();
  PresentSkyPalace_Reset(&g_render_device);
  PresentHud_Reset(&g_render_device);
  PresentActionEffects_Reset(&g_render_device);
  EffectRenderer_Reset();
  PresentSim3D_ResetResources();
  PresentWorldNav_ResetResources();
  PresentSimMenu_Reset();
}

bool PresentBlankScene(void) {
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  if (!ArRenderDevice_SetRenderTarget(
          &g_render_device, CrtPost_BaseTarget()) ||
      !ArRenderDevice_UseOutputCoordinates(&g_render_device) ||
      !ArRenderDevice_SetViewport(&g_render_device, NULL) ||
      !ArRenderDevice_SetClipRect(&g_render_device, NULL) ||
      !ArRenderDevice_Clear(&g_render_device, black)) {
    SessionFatal_Request(
        "The renderer could not clear its scene target for a blank display "
        "(%s). Restart the game; if this repeats, update your graphics "
        "driver.", ArRenderDevice_LastError(&g_render_device));
    return false;
  }
  return true;
}

void PresentCompositeScene(const FrameSlot *slot, float alpha) {
  if (!ArRenderDevice_IsReady(&g_render_device) ||
      !ArRenderTexture_IsValid(g_texture)) return;

  /* Forced blank and zero brightness gate every host-owned game layer,
   * including effects and room shells that bypass native PPU scanout. */
  if ((slot->inidisp & 0x80) || !(slot->inidisp & 0x0f)) {
    (void)PresentBlankScene();
    return;
  }

  const SimPresentationDecision sim_view = Sim3D_PresentationDecision(&slot->sim);
  if (sim_view.view == kSimView_Enhanced) {
    const PresentationOutcome sim = PresentSim3D(slot);
    if (!PresentationOutcome_IsUsable(sim)) {
      SessionFatal_Request(
          "The enhanced SIM renderer lost its active frame state (%s). "
          "Restart the game. If this happens again, update your graphics "
          "driver or select a different renderer.",
          ArRenderDevice_LastError(&g_render_device));
    }
    return;
  }
  if (sim_view.view == kSimView_WorldNavigation) {
    const PresentationOutcome navigation = PresentWorldNavigation3D(slot);
    if (PresentationOutcome_IsUsable(navigation)) return;
    SessionFatal_Request(
        "The enhanced world-navigation renderer failed while it was active "
        "(%s). Restart the game. If this happens again, update your graphics "
        "driver or disable enhanced world navigation before entering the map.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }

  if (slot->diorama_active) {
    PresentDiorama_Draw(&g_render_device, slot, alpha);
    return;
  }

  ArRenderRectI output_viewport;
  if (!Present_ResolveOutputViewport(&g_render_device, slot, &output_viewport)) {
    SessionFatal_Request(
        "The renderer could not resolve the game output viewport (%s). "
        "Restart the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }
  (void)PresentActionHeat_Begin(&g_render_device, slot, output_viewport);
  if (SessionFatal_Requested()) return;
  const ArRenderRectI viewport = PresentActionHeat_SceneViewport(output_viewport);
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(
          &g_render_device,
          viewport,
          black, black, &output_frame)) {
    PresentActionHeat_Cancel(&g_render_device);
    SessionFatal_Request(
        "The renderer could not begin the game scene output (%s). Restart "
        "the game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }
  const ArRenderRectI local_viewport = {0, 0, viewport.w, viewport.h};
  const ArRenderRectI src = {
    slot->visible_x0, 0, slot->visible_width, FrameSlot_CaptureHeight(slot),
  };
  ArRenderRectF source = {
    (float)src.x, (float)src.y, (float)src.w, (float)src.h,
  };
  const ArRenderRectF destination = ArPresentationLayout_CaptureDestination(
      local_viewport, FrameSlot_VisibleHeight(slot), slot->visible_top,
      FrameSlot_CaptureHeight(slot), slot->ws_extra_top);
  if (slot->sim.view == kSimView_SkyPalace) {
    const PresentationOutcome palace = PresentSkyPalace_Draw(
        &g_render_device, slot, local_viewport,
        &source, &destination);
    if (!PresentationOutcome_IsUsable(palace)) {
      ArRenderOutputFrame_Abort(&output_frame);
      PresentActionHeat_Cancel(&g_render_device);
      SessionFatal_Request(
          "The selected Sky Palace renderer could not complete its %s (%s). "
          "Restart the game; if this repeats, report the graphics settings "
          "and update your graphics driver.",
          PresentSkyPalace_ForegroundReady() ? "scene" : "foreground capture/upload",
          ArRenderDevice_LastError(&g_render_device));
      return;
    }
  } else if (!ArRenderDevice_DrawTexture(
          &g_render_device, g_texture, &source, &destination)) {
    ArRenderOutputFrame_Abort(&output_frame);
    PresentActionHeat_Cancel(&g_render_device);
    SessionFatal_Request(
        "The renderer rejected the base game framebuffer (%s). Restart the "
        "game; if this repeats, update your graphics driver.",
        ArRenderDevice_LastError(&g_render_device));
    return;
  }

  PresentHdReplacements_DrawMode7(
      &g_render_device, HdReplacementHost_Mode7Texture(), slot, local_viewport);
  if (!PresentActionEffects_DrawFlatPlanes(
          &g_render_device, slot, local_viewport)) {
    ArRenderOutputFrame_Abort(&output_frame);
    PresentActionHeat_Cancel(&g_render_device);
    return;
  }
  PresentActionEffects_Draw(&g_render_device, slot, local_viewport, NULL);
  if (!ArRenderOutputFrame_Finish(&output_frame)) {
    PresentActionHeat_Cancel(&g_render_device);
    SessionFatal_Request(
        "The renderer could not restore the output after drawing the game "
        "scene (%s). Restart the game; if this repeats, update your graphics "
        "driver.", ArRenderDevice_LastError(&g_render_device));
    return;
  }
  PresentActionHeat_End(&g_render_device, slot, output_viewport);
  if (SessionFatal_Requested()) return;
  PresentHdReplacements_DrawScreen(&g_render_device, slot, output_viewport);
  /* The captured BG3 surface is transparent outside its visible cells, so it
   * can be the final single unit without covering a BG1/BG2 HD replacement.
   * Drawing replacements first also makes the ordering explicit for future
   * enhanced glyph claims inside this same composite. */
  PresentHud_Draw(&g_render_device, slot, output_viewport);
}

bool PresentAuthenticScene(const FrameSlot *slot, ArRenderRectI viewport) {
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
          viewport,
          black, black, &output_frame))
    return false;
  const ArRenderRectF source = {
    (float)slot->authentic_x0, (float)slot->authentic_y0,
    (float)kFrameSlotAuthenticWidth, (float)kFrameSlotAuthenticHeight,
  };
  const ArRenderRectF destination = {
    0.0f, 0.0f, (float)viewport.w, (float)viewport.h,
  };
  bool drawn;
  if (PresentSimMenu_Active(slot)) {
    const ArRenderRectI local = {0, 0, viewport.w, viewport.h};
    if (slot->sim.separated_valid)
      drawn = PresentSimMenuFlatTown(slot,
          (ArRenderRectI){slot->ws_extra, 0, 256, 224}, local);
    else {
      const ArRenderRectF clean_source = {slot->ws_extra, 0, 256, 224};
      drawn = ArRenderDevice_DrawTexture(&g_render_device, g_texture,
                                         &clean_source, &destination);
    }
    if (drawn) PresentHud_DrawComposited(&g_render_device, slot, local);
  } else drawn = ArRenderDevice_DrawTexture(
      &g_render_device, g_authentic_texture, &source, &destination);
  if (!drawn) {
    ArRenderOutputFrame_Abort(&output_frame);
    return false;
  }
  return ArRenderOutputFrame_Finish(&output_frame);
}

bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      ArRenderRectI priority_viewport) {
  if (!slot || !ArRenderDevice_IsReady(&g_render_device) ||
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
  /* Menu phases do not move or resize the comparison inset. */
  const ArRenderRectF destination = {
    (float)(priority_viewport.x + priority_viewport.w - margin - width),
    (float)(priority_viewport.y + priority_viewport.h - margin - height),
    (float)width, (float)height,
  };
  const ArRenderRectI frame = {
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
  if (rendered)
    PresentSimMenu_DrawNativeHelp(slot, (ArRenderRectI){
        destination.x, destination.y, destination.w, destination.h});
  return rendered;
}

bool PresentComparisonTransitionOverlay(uint8_t alpha, const char *label) {
  if (!alpha) return true;
  if (!ArRenderDevice_IsReady(&g_render_device)) return false;
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
    const int label_height = kSettingsOverlayGlyphSize * scale;
    const int label_width = width * 3 / 4;
    if (SettingsOverlay_DrawEnhancedLabel((ArRenderRectI){
            (width - label_width) / 2, (height - label_height) / 2,
            label_width, label_height}, 255, label))
      return rendered;
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
