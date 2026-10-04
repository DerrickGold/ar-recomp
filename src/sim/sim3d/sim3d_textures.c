#include "sim/sim3d/sim3d_textures.h"

#include <limits.h>
#include <stdio.h>
#include "app/settings.h"
#include "app/session_fatal.h"
#include "snesrecomp/game/types.h"
#include "present/present.h"
#include "present/presentation_surface.h"
#include "present/presentation_upload_mirror.h"
#include "sim/menu/present_sim_menu.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim3d/sim3d_performance.h"
#include "sim/sim3d/present_sim3d_canvas.h"
#include "sim/sim_render_atlas.h"
#include "sim/voxels/sim_background_voxel_renderer.h"

static ArRenderTexture s_atlas;
static ArRenderTexture s_layers[kSim3DPlane_Count];
static ArRenderTexture s_flat;
static bool s_ready;

ArRenderTexture Sim3DTextures_Atlas(void) { return s_atlas; }
ArRenderTexture Sim3DTextures_Flat(void) { return s_flat; }
ArRenderTexture Sim3DTextures_Layer(int plane) {
  return plane >= 0 && plane < kSim3DPlane_Count ? s_layers[plane] : ArRenderTexture_Invalid();
}
bool Sim3DTextures_Ready(void) { return s_ready; }
bool Sim3DTextures_BillboardsReady(void) { return ArRenderTexture_IsValid(s_atlas); }

enum {
  kSim3DUploadSurface_Flat = kSim3DPlane_Count,
  kSim3DUploadSurface_Atlas,
  kSim3DUploadSurface_Count,
};

static PresentationUploadMirror s_sim3d_upload_mirrors[kSim3DUploadSurface_Count];

void Sim3DTextures_ResetUploads(void) {
  for (int surface = 0; surface < kSim3DUploadSurface_Count; surface++)
    PresentationUploadMirror_Reset(&s_sim3d_upload_mirrors[surface]);
}

void Sim3DTextures_Create(ArRenderDevice *device) {
  /* Billboards can fail independently of the core layer/flat capture family. */
  const ArRenderTextureDesc sim_atlas_texture = {
    .width = kSimObjAtlasWidth,
    .height = kSimObjAtlasHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (ArRenderDevice_CreateTexture(
          device, &sim_atlas_texture,
          &s_atlas)) {
    /* Static storage is zero-initialized before the game thread starts. */
    ArRenderDevice_UpdateTexture(
        device, s_atlas, NULL,
        g_sim_obj_atlas_pixels, kSimObjAtlasPitch);
  } else {
    fprintf(stderr,
            "[sim3d-d1] semantic atlas texture unavailable: %s\n",
            ArRenderDevice_LastError(device));
  }

  /* The core renderer requires the complete layer family and flat fallback. */
  s_ready = true;
  const ArRenderTextureDesc sim_layer_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!ArRenderDevice_CreateTexture(
            device, &sim_layer_texture,
            &s_layers[plane])) {
      s_ready = false;
      break;
    }
  }
  const ArRenderTextureDesc sim_flat_texture = {
    .width = kSim3DMaxWidth,
    .height = kSim3DMaxHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  if (!ArRenderDevice_CreateTexture(
          device, &sim_flat_texture, &s_flat))
    s_ready = false;
  if (!s_ready) {
    fprintf(stderr,
            "[sim3d-d2] capture textures unavailable: %s\n",
            ArRenderDevice_LastError(device));
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      ArRenderDevice_DestroyTexture(
          device, s_layers[plane]);
      s_layers[plane] = ArRenderTexture_Invalid();
    }
    ArRenderDevice_DestroyTexture(device, s_flat);
    s_flat = ArRenderTexture_Invalid();
  }
  if (g_settings.sim3d_mode && !s_ready) {
    Die("Simulation town 3D is enabled, but its core capture textures could "
        "not be created. Restart after checking graphics memory and driver "
        "stability, or disable Simulation town 3D in settings.ini.");
  }
  if (g_settings.sim3d_mode &&
      (Settings_Sim3DRequestedFeatures() & kSimFeature_ObjectBillboards) &&
      !Sim3DTextures_BillboardsReady()) {
    Die("Simulation object billboards are enabled, but their renderer atlas "
        "could not be created. Restart after checking graphics memory and "
        "driver stability, or disable object billboards in settings.ini.");
  }
}

void Sim3DTextures_Destroy(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_atlas);
  s_atlas = ArRenderTexture_Invalid();
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    ArRenderDevice_DestroyTexture(
        device, s_layers[plane]);
    s_layers[plane] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(device, s_flat);
  s_flat = ArRenderTexture_Invalid();
  s_ready = false;
  Sim3DTextures_ResetUploads();
}

static void UploadSurface(
    ArRenderDevice *device, ArRenderTexture texture, int surface, const uint32_t *pixels,
    int width, int height, int pitch_pixels) {
  if (surface < 0 || surface >= kSim3DUploadSurface_Count ||
      pitch_pixels > INT_MAX / (int)sizeof(uint32_t))
    return;
  PresentationUploadResult result;
  if (PresentationUploadMirror_UploadArgb8888(
          &s_sim3d_upload_mirrors[surface], device, texture,
          (const uint8_t *)pixels, width, height,
          pitch_pixels * (int)sizeof(uint32_t), 0, 0, &result) && result.uploaded_bytes)
    Sim3DPerformance_AddUpload(result.uploaded_bytes);
}

void Sim3DTextures_Upload(ArRenderDevice *device, const FrameSlot *slot) {
  /* Resolve this frame's replacement before suppressing any source upload.
   * Captured BG1 stays available to HUD reconstruction and diagnostics. */
  SimBackgroundVoxelRenderer_Upload(device);
  const Sim3DGroundSource ground_source = Sim3D_ResolveGroundSource(
      slot->sim.effective_features, slot->sim.background_voxel_enabled,
      SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial));
  /* D1b: the raw atlas follows the same upload-before-release ownership as
   * every other frame pixel buffer. Only the packed used rectangle is copied;
   * all descriptors in this immutable slot are bounded by that rectangle. */
  if (ArRenderTexture_IsValid(s_atlas) &&
      slot->sim.town && slot->sim.atlas_valid &&
      slot->sim.atlas_used_width && slot->sim.atlas_used_height) {
    const ArRenderRectI atlas = {
      0, 0, slot->sim.atlas_used_width, slot->sim.atlas_used_height,
    };
    UploadSurface(
        device, s_atlas, kSim3DUploadSurface_Atlas,
        g_sim_obj_atlas_pixels, atlas.w, atlas.h,
        kSimObjAtlasWidth);
  }

  if (slot->sim.separated_valid) {
    const ArRenderRectI frame = {
      0, 0, slot->snes_width, slot->snes_height,
    };
    uint32_t plane_upload_mask =
        Sim3D_PlaneTextureUploadMask(
            slot->sim.effective_features,
            slot->sim.separated_plane_mask);
    if (PresentSimMenu_Active(slot))
      plane_upload_mask |= slot->sim.separated_plane_mask;
    else if (ground_source == kSim3DGround_Voxels && !g_settings.scene_inspector)
      plane_upload_mask &= ~((1u << kSim3DPlane_Bg1Low) | (1u << kSim3DPlane_Bg1High));
    for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
      const SrPpuSurfaceView *surface =
          PresentationSurface_Bound(&slot->sim3d_output_surfaces.planes[plane]);
      if ((plane_upload_mask & (1u << plane)) &&
          ArRenderTexture_IsValid(s_layers[plane]) &&
          PresentationSurface_Holds(surface, frame.w, frame.h)) {
        UploadSurface(
            device, s_layers[plane], plane,
            (const uint32_t *)surface->data,
            frame.w, frame.h,
            (int)(surface->pitch_bytes / sizeof(uint32_t)));
      }
    }
    /* Ground projection samples the separated planes directly. Upload the
     * CPU flat composite only for the fallback stage that actually draws it. */
    if (ArRenderTexture_IsValid(s_flat) &&
        !(slot->sim.effective_features & kSimFeature_GroundProjection)) {
      UploadSurface(
          device, s_flat, kSim3DUploadSurface_Flat,
          g_sim3d_flat_pixels,
          frame.w, frame.h, frame.w);
    }
  }
  PresentSim3DCanvas_Upload(device,
      slot->sim.view == kSimView_Enhanced && slot->sim.separated_valid &&
      ground_source == kSim3DGround_Canvas);
}

bool Sim3DTextures_ValidateSetting(const SettingDesc *desc) {
  if (g_settings.sim3d_mode &&
      (desc->field == &g_settings.sim3d_mode ||
       desc->field == &g_settings.sim3d_object_billboards)) {
    if (!s_ready) {
      SessionFatal_Request(
          "Simulation town 3D was selected, but its core renderer textures "
          "are unavailable. Restart after checking graphics memory and driver "
          "stability, or leave Simulation town 3D disabled.");
      return false;
    } else if (g_settings.sim3d_object_billboards &&
               !Sim3DTextures_BillboardsReady()) {
      SessionFatal_Request(
          "Simulation object billboards were selected, but their renderer "
          "atlas is unavailable. Restart after checking graphics memory and "
          "driver stability, or leave object billboards disabled.");
      return false;
    }
  }
  return true;
}
