#include "sim/sim_frame_capture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "action/action_obj_apron.h"
#include "actraiser_game.h"
#include "app/performance_metrics.h"
#include "app/settings.h"
#include "dev/scene_inspector.h"
#include "host/host_frame_surfaces.h"
#include "host/parallel_work.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_phase0_trace.h"
#include "sim/sim_world_map_build.h"
#include "sim/town/sim_town_canvas.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/game/runtime.h"

Settings g_settings;
uint8 g_ram[kActRaiserWramSize];
uint8_t g_pixels[kHostFrameSurfaceBytes];
int g_snes_width = 320, g_snes_height = 224;
int snes_frame_counter = 17;
static SimFrameData frame;
static uint32_t canvas_serial = 7, voxel_serial = 11;
static bool needs_ppu;
static int api_queries, worker_creates, worker_destroys;
static char events[64];
static size_t event_count;
static SrRunnerHandle *const runner = (SrRunnerHandle *)(uintptr_t)1;
static Sim3DCameraPresentationState camera = {
  .pitch_mrad = 620, .yaw_mrad = -80, .distance_x100 = 340,
};

static void Event(char event) {
  assert(event_count + 1 < sizeof(events));
  events[event_count++] = event;
  events[event_count] = 0;
}
static void ExpectEvents(const char *expected) {
  assert(!strcmp(events, expected));
  event_count = 0;
  events[0] = 0;
}
uint32_t PerformanceMetrics_Epoch(void) { return 0; }
uint64_t HostClock_Nanoseconds(void) { assert(false); return 0; }
void PerformanceMetrics_Record(uint32_t epoch, PerformanceStage stage, uint64_t elapsed) {
  (void)epoch;
  (void)stage;
  (void)elapsed;
  assert(false);
}
SrRunnerHandle *RtlGameRunner(void) { return runner; }
const SnesRunnerApi *sr_runner_get_api(uint32_t version) {
  assert(version == SR_RUNNER_ABI_VERSION);
  api_queries++;
  return NULL; /* A missing PPU view must still reach the safe canvas fallback. */
}
SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void) { return kSimFeature_All; }
SimRenderFeatureMask Sim3D_ImplementedFeatures(void) { return kSimFeature_GroundProjection; }
void SimWorldMap_BuildIfNeeded(bool palace) {
  assert(palace == (g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace));
  Event('W');
}
void SimPhase0Trace_Frame(uint32 host_frame, const uint8 *wram, SrRunnerHandle *actual) {
  assert(host_frame == 17 && wram == g_ram && actual == runner);
  Event('P');
}
void SimRenderMetadata_CaptureFrame(SimFrameData *dst, const uint8 *wram,
    bool town, bool world, SimRenderFeatureMask requested, uint32_t diagnostic,
    SimRenderFeatureMask implemented) {
  assert(wram == g_ram && town == g_settings.sim3d_mode);
  assert(world == g_settings.sim3d_world_navigation);
  assert(requested == kSimFeature_All && implemented == kSimFeature_GroundProjection);
  assert(diagnostic == g_settings.sim3d_diagnostic_layers);
  *dst = (SimFrameData){0};
  Event('C');
}
void SimRenderMetadata_CaptureSkyPalaceFrame(SimFrameData *dst, const uint8 *wram, bool enabled) {
  assert(dst == &frame && wram == g_ram);
  assert(enabled == (g_settings.sim3d_world_navigation && g_settings.sim3d_sky_palace));
  Event('S');
}
void ActRaiser_SimSpriteMargins(int *left, int *right, int *top, int *bottom) {
  *left = 23;
  *right = 29;
  *top = 13;
  *bottom = 19;
}
void Sim3DCamera_CapturePresentationState(Sim3DCameraPresentationState *dst) { *dst = camera; }
void Sim3D_AnnotateFrame(SimFrameData *dst, const Sim3DTuning *tuning) {
  /* The resolved camera may differ from the stored Free Cam settings. */
  assert(tuning->pitch_mrad == camera.pitch_mrad && tuning->yaw_mrad == camera.yaw_mrad);
  assert(tuning->distance_x100 == camera.distance_x100);
  assert(tuning->sprite_margin_left == 23 && tuning->sprite_margin_right == 29);
  assert(tuning->sprite_margin_top == 13 && tuning->sprite_margin_bottom == 19);
  assert(tuning->voxel_detail == g_settings.sim3d_voxel_detail);
  assert(tuning->cloud_opacity_pct == g_settings.sim3d_cloud_opacity_pct);
  dst->projection_pitch_mrad = (int16_t)tuning->pitch_mrad;
  Event('A');
}
bool SimWorldNavigationCapture_Capture(SimFrameData *dst, SrRunnerHandle *actual) {
  assert(dst->projection_pitch_mrad == camera.pitch_mrad && actual == runner);
  Event('N');
  return true;
}
bool Sim3D_TownCanvasNeedsPpuView(const SimFrameData *sim) {
  assert(sim->projection_pitch_mrad == camera.pitch_mrad);
  return needs_ppu;
}
struct HostParallelWork { int unused; };
static HostParallelWork workers;
HostParallelWork *HostParallelWork_Create(unsigned maximum) {
  assert(maximum == 3);
  worker_creates++;
  return &workers;
}
void HostParallelWork_Run(HostParallelWork *work, size_t count, size_t minimum,
    HostParallelWorkRange range, void *context) {
  assert(work == &workers && minimum > 0);
  range(context, 0, count);
}
void HostParallelWork_Destroy(HostParallelWork *work) {
  assert(work == &workers);
  worker_destroys++;
}
static void CompleteCanvas(void *context, size_t first, size_t end) {
  (void)context;
  assert(first == 0 && end == 1);
  canvas_serial++;
  voxel_serial++;
}
void Sim3D_RenderTownCanvas(const SimFrameData *sim, const uint8 *wram,
    const SrPpuStateSnapshot *ppu, const SrBorrowedU16Span *vram,
    const SrBorrowedU16Span *cgram, SimBackgroundRowDispatch dispatch, void *context) {
  assert(sim->projection_pitch_mrad == camera.pitch_mrad && wram == g_ram);
  assert(!ppu && !vram && !cgram);
  Event('R');
  dispatch(context, 1, CompleteCanvas, NULL);
}
uint32_t SimTownCanvas_Serial(void) { return canvas_serial; }
uint32_t SimBackgroundVoxels_Serial(void) { return voxel_serial; }
static void ExpectCompletedFrame(const SimFrameData *sim) {
  assert(sim->town_canvas_serial == canvas_serial);
  assert(sim->background_voxel_serial == voxel_serial);
}
void Sim3D_LogViewTransition(const SimFrameData *sim) {
  ExpectCompletedFrame(sim);
  Event('L');
}
void SceneInspector_SetSimFrameData(const SimFrameData *sim) {
  ExpectCompletedFrame(sim);
  Event('I');
}
void SimRenderMetadata_TraceFrame(uint32_t host_frame, const SimFrameData *sim,
    const uint8_t *rgba, int width, int height, int pitch) {
  ExpectCompletedFrame(sim);
  assert(host_frame == 17 && width == g_snes_width && height == g_snes_height);
  assert(rgba == g_pixels + ActionApron_DisplayOffset(SR_PPU_OBJ_APRON));
  assert((size_t)pitch == ActionApron_SurfacePitch(width, SR_PPU_OBJ_APRON));
  Event('T');
}

int main(void) {
  g_settings.sim3d_mode = true;
  g_settings.sim3d_world_navigation = true;
  g_settings.sim3d_sky_palace = true;
  g_settings.sim3d_diagnostic_layers = 5;
  g_settings.sim3d_voxel_detail = 2;
  g_settings.sim3d_cloud_opacity_pct = 45;
  /* A screenshot can refresh before the first produced frame, without
   * starting workers, building the world, rendering pixels or tracing. */
  SimFrameCapture_RefreshMetadata(&frame);
  ExpectEvents("CSAN");
  ExpectCompletedFrame(&frame);
  assert(!worker_creates && !api_queries && canvas_serial == 7);

  /* Headless production still completes canvas work and stamps its new
   * serials before inspector/trace consumers, without frame submission. */
  SimFrameCapture_Produce(&frame);
  ExpectEvents("WPCSANRLIT");
  assert(canvas_serial == 8 && voxel_serial == 12 && worker_creates == 1);
  assert(!api_queries);
  for (int i = 0; i < 3; i++) {
    camera.pitch_mrad += 10;
    g_settings.sim3d_cloud_opacity_pct += 5;
    SimFrameCapture_RefreshMetadata(&frame);
    ExpectEvents("CSAN");
    ExpectCompletedFrame(&frame);
    assert(frame.projection_pitch_mrad == camera.pitch_mrad && canvas_serial == 8);
  }
  needs_ppu = true;
  g_settings.sim3d_world_navigation = false;
  SimFrameCapture_Produce(&frame);
  ExpectEvents("WPCSANRLIT");
  assert(api_queries == 1 && canvas_serial == 9 && worker_creates == 1);
  SimFrameCapture_Shutdown();
  assert(worker_destroys == 1);
  SimFrameCapture_Produce(&frame);
  ExpectEvents("WPCSANRLIT");
  assert(worker_creates == 2);
  SimFrameCapture_Shutdown();
  assert(worker_destroys == 2);
  puts("SIM capture: metadata refresh, headless production and canvas publication passed");
  return 0;
}
