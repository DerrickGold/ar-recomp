#include "sim/sim_frame_capture.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "sim/sim_world_map.h"

#include "action/action_obj_apron.h"
#include "actraiser_game.h"
#include "render/presentation_options.h"
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
static Sim3DTuning CaptureTuning(bool owned) {
  int sim_margin_left = 0, sim_margin_right = 0;
  int sim_margin_top = 0, sim_margin_bottom = 0;
  ActRaiser_SimSpriteMargins(&sim_margin_left, &sim_margin_right,
                             &sim_margin_top, &sim_margin_bottom);
  Sim3DCameraPresentationState sim_camera = {0};
  if (!owned) Sim3DCamera_CapturePresentationState(&sim_camera);
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

static bool CompletionBackdropRequested(void) {
  if (!g_settings.death_heim_completion_world || !g_settings.diorama_mode ||
      g_ram[kActRaiserWram_MapGroup] != kActRaiserMapGroup_DeathHeim ||
      g_ram[kActRaiserWram_CurrentMap] != kActRaiserDeathHeimMap_Hub ||
      g_ram[kActRaiserWram_DeathHeimProgress] < kActRaiserDeathHeimProgress_FinalBossBeaten)
    return false;
  return true;
}

static bool CaptureCompletionBackdrop(SimFrameData *sim) {
  if (!CompletionBackdropRequested()) return false;
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrRunnerHandle *runner = RtlGameRunner();
  SrPpuStateSnapshot ppu = {.struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE};
  if (!api || !runner || api->struct_size < SNES_RUNNER_API_PPU_STATE_SIZE ||
      !(api->capabilities & SR_RUNNER_CAP_PPU_STATE) || !api->query_ppu_state ||
      api->query_ppu_state(runner, &ppu) != SR_RESULT_OK ||
      !ActRaiser_IsDeathHeimCompletionScene(
          g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap],
          g_ram[kActRaiserWram_DeathHeimProgress],
          ppu.background_tilemap_control[0], ppu.background_tilemap_control[1]))
    return false;
  SimWorldMap_BuildForDeathHeimCompletion();
  SimRenderMetadata_CaptureDeathHeimCompletionFrame(sim, g_ram,
      ppu.display_control & 0x80 ? 0 : ppu.brightness);
  return sim->death_heim_completion_world;
}

static void CaptureMetadata(SimFrameData *sim, bool owned, uint32_t underlay_serial) {
  if (owned) SimRenderMetadata_CaptureFrameWithUnderlay(
      sim, g_ram, g_settings.sim3d_mode, false,
      Settings_Sim3DRequestedFeatures(), g_settings.sim3d_diagnostic_layers,
      Sim3D_ImplementedFeatures(), underlay_serial);
  else SimRenderMetadata_CaptureFrame(
      sim, g_ram, g_settings.sim3d_mode,
      g_settings.sim3d_world_navigation,
      Settings_Sim3DRequestedFeatures(),
      g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
  if (!owned) SimRenderMetadata_CaptureSkyPalaceFrame(sim, g_ram,
      g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  if (!owned) (void)CaptureCompletionBackdrop(sim);
  if (!owned && sim->view != kSimView_None) Sim3DCamera_SetPresentationScene(sim);
  Sim3DTuning tuning = CaptureTuning(owned || sim->view == kSimView_None);
  if (sim->death_heim_completion_world) {
    sim->death_heim_completion_flags =
        (g_settings.death_heim_completion_waves ? kDeathHeimCompletion_Waves : 0) |
        (g_settings.death_heim_completion_pixel_water ? kDeathHeimCompletion_PixelWater : 0) |
        (g_settings.death_heim_completion_cherubs ? kDeathHeimCompletion_Cherubs : 0) |
        (g_settings.death_heim_completion_feathers ? kDeathHeimCompletion_Feathers : 0) |
        (g_settings.death_heim_completion_platform_details ? kDeathHeimCompletion_Platform : 0) |
        (g_settings.death_heim_completion_rim_light ? kDeathHeimCompletion_Rim : 0) |
        (g_settings.death_heim_completion_light_shafts ? kDeathHeimCompletion_Shafts : 0) |
        (g_settings.death_heim_completion_sun_glints ? kDeathHeimCompletion_SunGlints : 0);
    /* Fixed scene tuning plus its own switches: SIM preferences cannot change
     * the completion camera, cloud density, illumination or terrain. */
    tuning.landscape_height_pct = tuning.height_scale_x100 = 100;
    tuning.light_azimuth_deg = 225;
    tuning.light_elevation_deg = 70;
    tuning.cloud_opacity_pct = kSimCloudOpacityDefaultPct;
    tuning.cloud_drift_pct = kSimCloudDriftDefaultPct;
    tuning.world_navigation_lighting = true;
    tuning.world_navigation_clouds = g_settings.death_heim_completion_clouds;
    tuning.sky_palace_volumetric_clouds = true;
    tuning.world_navigation_cloud_shadows = false;
    tuning.world_navigation_atmosphere = tuning.world_navigation_haze = false;
    tuning.world_navigation_models = tuning.world_navigation_mountains = false;
    tuning.world_navigation_ground_detail = tuning.world_navigation_relief = false;
    tuning.underlay_haze_pct = tuning.underlay_defocus_pct = 0;
  }
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
  if (!owned) SimWorldNavigationCapture_Capture(sim, RtlGameRunner());
}

static void CaptureCanvasSerials(SimFrameData *sim) {
  sim->town_canvas_serial = SimTownCanvas_Serial();
  sim->background_voxel_serial = SimBackgroundVoxels_Serial();
}

void SimFrameCapture_RefreshMetadata(SimFrameData *sim) {
  CaptureMetadata(sim, false, 0);
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

/* Resource preparation owns this group. During town streaming it runs only
 * on the presentation owner, independently of producer PPU helpers. Each
 * dispatch joins before the canvas can be uploaded or another packet prepared. */
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

static void TraceFrame(const SimFrameData *sim) {
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

void SimFrameCapture_Produce(SimFrameData *sim) {
  /* Own the developed world tilemap instead of observing $7E:C000, which acts
   * and towns both reuse as unrelated scratch. This runs only on the game
   * thread, after an emulated tick reached a stable frame boundary. */
  PerformanceScope pipeline = PerformanceMetrics_Begin(kPerformance_WorldMap);
  if (!CompletionBackdropRequested())
    SimWorldMap_BuildIfNeeded(
        g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  PerformanceMetrics_End(pipeline);
  pipeline = PerformanceMetrics_Begin(kPerformance_Metadata);
  SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram,
                       RtlGameRunner());
  CaptureMetadata(sim, false, 0);
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
  TraceFrame(sim);
}

void SimFrameCapture_Shutdown(void) {
  HostParallelWork_Destroy(s_town_pixel_work);
  s_town_pixel_work = NULL;
  s_town_pixel_work_attempted = false;
}

void SimFrameCapture_ProduceOwned(SimFrameData *sim, SimFrameInputs *inputs,
                                 uint32_t underlay_serial) {
  PerformanceScope scope = PerformanceMetrics_Begin(kPerformance_Metadata);
  SimPhase0Trace_Frame((uint32)snes_frame_counter, g_ram, RtlGameRunner());
  CaptureMetadata(sim, true, underlay_serial);
  PerformanceMetrics_End(scope);
  SrBorrowedU16Span vram, cgram;
  inputs->ppu_valid = Sim3D_TownCanvasNeedsPpuView(sim) &&
      CaptureTownCanvasPpuView(&inputs->ppu, &vram, &cgram);
  memcpy(inputs->wram, g_ram, sizeof(inputs->wram));
  if (inputs->ppu_valid) {
    memcpy(inputs->vram, vram.data, sizeof(inputs->vram));
    memcpy(inputs->cgram, cgram.data, sizeof(inputs->cgram));
  }
  Sim3D_LogViewTransition(sim);
  TraceFrame(sim);
}

void SimFrameCapture_PrepareOwned(SimFrameData *sim, const SimFrameInputs *inputs) {
  if (!sim || !inputs) return;
  PerformanceScope scope = PerformanceMetrics_Begin(kPerformance_WorldMap);
  SimWorldMap_BuildFromSnapshot(inputs->wram);
  sim->underlay_serial = SimWorldMap_DevelopedAvailable() ? SimWorldMap_Serial() : 0;
  PerformanceMetrics_End(scope);
  scope = PerformanceMetrics_Begin(kPerformance_TownCanvas);
  const SrBorrowedU16Span vram = {
    .struct_size = sizeof(vram), .region = SR_MEMORY_VRAM,
    .data = inputs->vram, .element_count = SR_PPU_VRAM_WORD_COUNT,
    .lifetime_generation = inputs->ppu.lifetime_generation,
  };
  const SrBorrowedU16Span cgram = {
    .struct_size = sizeof(cgram), .region = SR_MEMORY_CGRAM,
    .data = inputs->cgram, .element_count = SR_PPU_CGRAM_WORD_COUNT,
    .lifetime_generation = inputs->ppu.lifetime_generation,
  };
  Sim3D_RenderTownCanvas(sim, inputs->wram,
      inputs->ppu_valid ? &inputs->ppu : NULL,
      inputs->ppu_valid ? &vram : NULL, inputs->ppu_valid ? &cgram : NULL,
      DispatchTownPixelRows, NULL);
  CaptureCanvasSerials(sim);
  PerformanceMetrics_End(scope);
}
