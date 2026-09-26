#include "sim/sim_frame_capture.h"

#include <limits.h>
#include <stddef.h>

#include "action/action_obj_apron.h"
#include "actraiser_game.h"
#include "app/performance_metrics.h"
#include "app/settings.h"
#include "dev/scene_inspector.h"
#include "host/host_display.h"
#include "host/host_frame_surfaces.h"
#include "host/parallel_work.h"
#include "present/frame_slot.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_world_map_build.h"
#include "sim/town/sim_town_canvas.h"
#include "sim/voxels/sim_background_voxels.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/runner.h"

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
  SimRenderMetadata_CaptureFrame(
      sim, g_ram, g_settings.sim3d_mode,
      g_settings.sim3d_world_navigation,
      Settings_Sim3DRequestedFeatures(),
      g_settings.sim3d_diagnostic_layers, Sim3D_ImplementedFeatures());
  SimRenderMetadata_CaptureSkyPalaceFrame(sim, g_ram,
      g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace);
  Sim3DTuning tuning = BuildSim3DTuning();
  Sim3D_AnnotateFrame(sim, &tuning);
  SimWorldNavigationCapture_Capture(sim, RtlGameRunner());
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
  sim->town_canvas_serial = SimTownCanvas_Serial();
  sim->background_voxel_serial = SimBackgroundVoxels_Serial();
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
