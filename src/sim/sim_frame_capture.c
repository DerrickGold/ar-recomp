#include "sim/sim_frame_capture.h"

#include <limits.h>
#include <stddef.h>

#include "action/action_obj_apron.h"
#include "actraiser_game.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "regional/presentation/regional_artwork.h"
#include "app/performance_metrics.h"
#include "app/settings.h"
#include "dev/scene_inspector.h"
#include "host/host_display.h"
#include "host/host_frame_surfaces.h"
#include "host/parallel_work.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_world_map_build.h"
#include "sim/town/sim_town_canvas.h"
#include "sim/voxels/sim_background_voxels.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/runner.h"

/* Both produced frames and paused redraws resolve one coherent set of
 * settings and camera controls before annotation. Keep that mapping with the
 * SIM producer; the frame-slot transport does not interpret SIM policy. */
static Sim3DTuning CaptureTuning(void) {
  int sim_margin_left = 0, sim_margin_right = 0;
  int sim_margin_top = 0, sim_margin_bottom = 0;
  ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right,
                             &sim_margin_top, &sim_margin_bottom);
  Sim3DCameraPresentationState sim_camera;
  Sim3DCamera_CapturePresentationState(&sim_camera);
  return (Sim3DTuning){
      .pitch_mrad = sim_camera.pitch_mrad,
      .yaw_mrad = sim_camera.yaw_mrad,
      .distance_x100 = sim_camera.distance_x100,
      .landscape_height_pct = g_settings.sim3d_landscape_height_pct,
      .height_scale_x100 = g_settings.sim3d_height_scale_x100,
      .voxel_preset = g_settings.sim3d_voxel_preset,
      .voxel_detail = g_settings.sim3d_voxel_detail,
      .voxel_lod = g_settings.sim3d_voxel_lod,
      .voxel_shading = g_settings.sim3d_voxel_shading,
      .voxel_style = g_settings.sim3d_voxel_style,
      .voxel_facing = g_settings.sim3d_voxel_facing,
      .voxel_render_scale = g_settings.sim3d_voxel_render_scale,
      .shadow_opacity_pct = g_settings.sim3d_shadow_opacity_pct,
      .height_pop_pct = g_settings.sim3d_height_pop_pct,
      .light_azimuth_deg = g_settings.sim3d_light_azimuth_deg,
      .light_elevation_deg = g_settings.sim3d_light_elevation_deg,
      .shadow_softness_pct = g_settings.sim3d_shadow_softness_pct,
      .rim_strength_pct = g_settings.sim3d_rim_strength_pct,
      .underlay_haze_pct = g_settings.sim3d_underlay_haze_pct,
      .cloud_opacity_pct = g_settings.sim3d_cloud_opacity_pct,
      .cloud_falloff_px = g_settings.sim3d_cloud_falloff_px,
      .cloud_inset_px = g_settings.sim3d_cloud_inset_px,
      .cull_lead_px = g_settings.sim3d_cull_lead_px,
      .cull_haze_pct = g_settings.sim3d_cull_haze_pct,
      .cull_dim_pct = g_settings.sim3d_cull_dim_pct,
      .cull_haze_lead_px = g_settings.sim3d_cull_haze_lead_px,
      .cull_corner_px = g_settings.sim3d_cull_corner_px,
      .underlay_defocus_pct = g_settings.sim3d_underlay_defocus_pct,
      .cloud_altitude_px = g_settings.sim3d_cloud_altitude_px,
      .cloud_drift_pct = g_settings.sim3d_cloud_drift_pct,
      .world_navigation_lighting =
          g_settings.sim3d_world_navigation_lighting,
      .world_navigation_clouds =
          g_settings.sim3d_world_navigation_clouds,
      .sky_palace_volumetric_clouds = g_settings.sim3d_sky_palace_volumetric,
      .world_navigation_cloud_shadows = g_settings.sim3d_world_navigation_cloud_shadows,
      .world_navigation_atmosphere = g_settings.sim3d_world_navigation_atmosphere,
      .world_navigation_models = g_settings.sim3d_world_navigation_towns,
      .world_navigation_relief = g_settings.sim3d_world_navigation_relief,
      .world_navigation_ground_detail = g_settings.sim3d_world_navigation_ground_detail,
      .world_navigation_mountains = g_settings.sim3d_world_navigation_mountains,
      .world_navigation_backdrop = g_settings.sim3d_backdrop,
      .world_navigation_haze = g_settings.sim3d_cull_haze,
      .cull_lift_inset = g_settings.sim3d_cull_lift_inset,
      .backdrop_strength_pct = g_settings.sim3d_backdrop_strength_pct,
      .backdrop_horizon_pct = g_settings.sim3d_backdrop_horizon_pct,
      .windmill_wind_stops_all = g_settings.fix_windmill_wind_stop,
      .sprite_margin_left = sim_margin_left,
      .sprite_margin_right = sim_margin_right,
      .sprite_margin_top = sim_margin_top,
      .sprite_margin_bottom = sim_margin_bottom };
}

static void CaptureMetadata(SimFrameData *sim) {
  SimRenderMetadata_CaptureFrame(
      sim, g_ram, g_settings.sim3d_mode,
      g_settings.sim3d_world_navigation,
      Settings_Sim3DRequestedFeatures(),
      g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
  SimRenderMetadata_CaptureSkyPalaceFrame(sim, g_ram,
      g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  Sim3DTuning tuning = CaptureTuning();
  Sim3D_AnnotateFrame(sim, &tuning);
  /* Match the visit's active selection AND donor availability. Requested
   * settings may be pending, and a missing donor leaves the native BG plain. */
  uint8_t artwork = ActRaiserRegional_TownArtworkSnapshot((uint16_t)sim->town << 8) &
      ActRaiserRegionalMedia_AvailableArtwork();
  sim->background_voxel_artwork_flags =
      artwork & (1u << kArRegionalArtwork_PyramidDetail)
          ? kSimBackgroundVoxel_PyramidEye : 0;
  /* Retained globe towns have no live tilemap or town scene ID. Publish the
   * last accepted town-art generation into their value-only model identities;
   * the presenter then invalidates its geometry through the ordinary flags. */
  uint8_t world_artwork = ActRaiserRegional_LastTownArtworkSnapshot() &
      ActRaiserRegionalMedia_AvailableArtwork();
  for (uint16_t at = 0; at < sim->world_navigation_towns.object_count; at++) {
    SimBackgroundVoxelObject *object = &sim->world_navigation_towns.objects[at];
    if (object->kind != kSimBackgroundVoxel_Pyramid) continue;
    object->flags &= (uint8_t)~kSimBackgroundVoxel_PyramidEye;
    if (world_artwork & (1u << kArRegionalArtwork_PyramidDetail))
      object->flags |= kSimBackgroundVoxel_PyramidEye;
  }
  SimWorldNavigationCapture_Capture(sim, RtlGameRunner());
}

static void CaptureCanvasSerials(SimFrameData *sim) {
  sim->town_canvas_serial = SimTownCanvas_Serial();
  sim->background_voxel_serial = SimBackgroundVoxels_Serial();
}

void SimFrameCapture_RefreshMetadata(SimFrameData *sim) {
  CaptureMetadata(sim);
  CaptureCanvasSerials(sim);
}

static bool CaptureTownCanvasPpuView(SrPpuStateSnapshot *ppu,
                                     SrBorrowedU16Span *vram,
                                     SrBorrowedU16Span *cgram) {
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *runner = RtlGameRunner();
  const uint64_t required_caps =
      SR_RUNNER_CAP_PPU_STATE | SR_RUNNER_CAP_BORROWED_U16_SPANS;
  if (!api || !runner || !ppu || !vram || !cgram ||
      api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      (api->capabilities & required_caps) != required_caps)
    return false;
  *ppu = (SrPpuStateSnapshot){.struct_size = sizeof(*ppu)};
  *vram = (SrBorrowedU16Span){.struct_size = sizeof(*vram)};
  *cgram = (SrBorrowedU16Span){.struct_size = sizeof(*cgram)};
  return api->query_ppu_state(runner, ppu) == SR_RESULT_OK &&
      api->borrow_u16_memory(runner, SR_MEMORY_VRAM, vram) ==
             SR_RESULT_OK &&
      api->borrow_u16_memory(runner, SR_MEMORY_CGRAM, cgram) ==
             SR_RESULT_OK &&
      vram->element_count >= SR_PPU_VRAM_WORD_COUNT &&
      cgram->element_count >= SR_PPU_CGRAM_WORD_COUNT &&
      vram->lifetime_generation == ppu->lifetime_generation &&
      cgram->lifetime_generation == ppu->lifetime_generation;
}

/* Producer-side work is owned here, not by the pure SIM classifier or the
 * render backend. This group is independent of presentation's helpers; both
 * stages synchronously join, so their jobs cannot oversubscribe one another. */
static HostParallelWork *s_town_pixel_work;
static bool s_town_pixel_work_attempted;

static void DispatchTownPixelRows(void *context, size_t count,
    SimBackgroundRowRange range, void *work) {
  (void)context;
  if (!s_town_pixel_work_attempted) {
    s_town_pixel_work_attempted = true;
    s_town_pixel_work = HostParallelWork_Create(3);
  }
  HostParallelWork_Run(s_town_pixel_work, count, 64, range, work);
}

void SimFrameCapture_Produce(SimFrameData *sim) {
  /* Own the developed world tilemap instead of observing $7E:C000, which acts
   * and towns both reuse as unrelated scratch. This runs only on the game
   * thread, after an emulated tick reached a stable frame boundary. */
  PerformanceScope pipeline = PerformanceMetrics_Begin(kPerformance_WorldMap);
  SimWorldMap_BuildIfNeeded(
      g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  PerformanceMetrics_End(pipeline);
  pipeline = PerformanceMetrics_Begin(kPerformance_Metadata);
  SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram,
                       RtlGameRunner());
  CaptureMetadata(sim);
  PerformanceMetrics_End(pipeline);
  pipeline = PerformanceMetrics_Begin(kPerformance_TownCanvas);
  /* This site runs for every drawn frame, including headless runs that never
   * call HostDisplay_SubmitFrame or FrameSlot_Capture. */
  SrPpuStateSnapshot town_ppu;
  SrBorrowedU16Span town_vram;
  SrBorrowedU16Span town_cgram;
  const bool have_town_ppu_view =
      Sim3D_TownCanvasNeedsPpuView(sim) &&
      CaptureTownCanvasPpuView(&town_ppu, &town_vram, &town_cgram);
  Sim3D_RenderTownCanvas(
      sim, g_ram,
      have_town_ppu_view ? &town_ppu : NULL,
      have_town_ppu_view ? &town_vram : NULL,
      have_town_ppu_view ? &town_cgram : NULL,
      DispatchTownPixelRows, NULL);
  CaptureCanvasSerials(sim);
  PerformanceMetrics_End(pipeline);
  Sim3D_LogViewTransition(sim);
  SceneInspector_SetSimFrameData(sim);
  /* g_pixels is bound apron-wide; offset past the apron so the trace sees
   * the authentic frame at column 0, as it always has. */
  const size_t trace_pitch =
      ActionApron_SurfacePitch(g_snes_width, SR_PPU_OBJ_APRON);
  if (trace_pitch <= INT_MAX) {
    SimRenderMetadata_TraceFrame(
        (uint32)snes_frame_counter, sim,
        g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON),
        g_snes_width, g_snes_height, (int)trace_pitch);
  }
}

void SimFrameCapture_Shutdown(void) {
  HostParallelWork_Destroy(s_town_pixel_work);
  s_town_pixel_work = NULL;
  s_town_pixel_work_attempted = false;
}
