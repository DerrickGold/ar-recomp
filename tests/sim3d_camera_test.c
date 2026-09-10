#include "sim/sim3d.h"
#include "actraiser_game.h"
#include "settings.h"
#include "user_data_dir.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;
uint8 g_ram[kActRaiserWramSize];
static int settings_writes;
uint64_t HostClock_Milliseconds(void) { return 1000; }
SimRenderFeatureMask Sim3D_ImplementedFeatures(void) { return kSimFeature_All; }
SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void) { return kSimFeature_All; }
const SettingDesc *Settings_Find(const char *key) { (void)key; settings_writes++; return NULL; }
SettingChangeResult Settings_Reset(const SettingDesc *desc) { (void)desc; settings_writes++; return kSettingChange_Applied; }
bool Settings_Save(const char *path) { (void)path; settings_writes++; return true; }
char *UserDataFile(char *buf, size_t size, const char *leaf) { (void)size; (void)leaf; return buf; }

int main(void) {
  g_settings.sim3d_world_navigation = true;
  g_settings.sim3d_camera_mode = kSimCam_Free;
  g_settings.sim3d_tilt_x_mrad = -575;
  g_settings.sim3d_distance_x100 = 300;
  g_settings.sim3d_dyncam_baseline_tilt_x_mrad = -575;
  g_settings.sim3d_dyncam_baseline_distance_x100 = 300;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
  assert(Sim3DCamera_ControlsAvailable(true)); /* Independent town master. */
  assert(!Sim3DCamera_ControlsAvailable(false));
  const Settings original = g_settings;
  Sim3DCameraPresentationState state;
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 300);
  assert(state.world_inspection_blend == 0);
  Sim3DCamera_Adjust(3.5f, 20, 2);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw < -2.7f && state.orbit_yaw > -3.14f);
  assert(fabsf(state.orbit_pitch - 1.57079632679f) < .00001f);
  assert(state.distance_x100 == 500);
  const float held = state.orbit_yaw;
  assert(Sim3DCamera_UpdateDynamic(.1f, true));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == held);
  assert(state.world_inspection_blend > 0 && state.world_inspection_blend < 1);
  assert(Sim3DCamera_UpdateDynamic(.1f, false));
  Sim3DCamera_CapturePresentationState(&state);
  assert(fabsf(state.orbit_yaw) < fabsf(held));
  Sim3DCamera_Adjust(NAN, 0, 0);
  Sim3DCamera_Adjust(0, 0, 100);
  Sim3DCamera_CapturePresentationState(&state);
  assert(isfinite(state.orbit_yaw) && state.distance_x100 == 2000);
  Sim3DCamera_Adjust(0, 0, -100);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.distance_x100 == 200);
  Sim3DCamera_Reset();
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 300);
  assert(state.world_inspection_blend == 0);
  Sim3DCamera_FlushSettingsIfDirty();
  assert(settings_writes == 0 && !memcmp(&g_settings, &original, sizeof(original)));
  /* Framing follows input/zoom, not the wrapped orbit angle. Full yaw turns,
   * a held-but-neutral stick/drag and varied update cadence remain stable. */
  assert(Sim3DCamera_UpdateDynamic(2, true));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == 1 && state.orbit_yaw == 0);
  Sim3DCamera_Adjust(6.28318530718f, 0, 0);
  assert(!Sim3DCamera_UpdateDynamic(.1f, true));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == 1 && fabsf(state.orbit_yaw) < .00001f);
  assert(!Sim3DCamera_UpdateDynamic(NAN, false));
  assert(!Sim3DCamera_UpdateDynamic(-1, false));
  assert(Sim3DCamera_UpdateDynamic(.1f, false));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend > 0 && state.world_inspection_blend < 1);
  Sim3DCamera_UpdateDynamic(10, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == 0);
  Sim3DCamera_UpdateDynamic(.6f, true);
  Sim3DCamera_CapturePresentationState(&state);
  const float single_step = state.world_inspection_blend;
  Sim3DCamera_Reset();
  for (int i = 0; i < 60; i++) Sim3DCamera_UpdateDynamic(.01f, true);
  Sim3DCamera_CapturePresentationState(&state);
  assert(fabsf(single_step - state.world_inspection_blend) < .000001f);
  Sim3DCamera_Reset();
  Sim3DCamera_Adjust(0, 0, .5f);
  Sim3DCamera_UpdateDynamic(2, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == .5f && state.distance_x100 == 350);
  Sim3DCamera_Adjust(0, 0, 1);
  Sim3DCamera_UpdateDynamic(2, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == 1);
  Sim3DCamera_Adjust(0, 0, -1.5f);
  Sim3DCamera_UpdateDynamic(10, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.world_inspection_blend == 0 && state.distance_x100 == 300);
  /* Leave/re-enter and master Off/On must not leak the globe's pose or zoom
   * into a town, action room, or a later navigation visit. */
  for (int mode = kSimCam_Free; mode <= kSimCam_Dynamic; mode++) {
    g_settings.sim3d_camera_mode = mode;
    Sim3DCamera_Adjust(1, -.4f, 4);
    Sim3DCamera_UpdateDynamic(2, true);
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 300);
    assert(state.world_inspection_blend == 0);
    assert(!Sim3DCamera_ControlsAvailable(true));
    g_settings.sim3d_mode = 1;
    assert(Sim3DCamera_ControlsAvailable(true));
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.distance_x100 == 300);
    assert(state.world_inspection_blend == 0);
    Sim3DCamera_Adjust(.5f, .3f, 1);
    g_settings.sim3d_world_navigation = false;
    assert(!Sim3DCamera_ControlsAvailable(true));
    Sim3DCamera_CapturePresentationState(&state);
    g_settings.sim3d_world_navigation = true;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.distance_x100 == 300);
    assert(state.world_inspection_blend == 0);
    g_settings.sim3d_mode = 0;
  }
  assert(settings_writes == 0);
  puts("sim3d_camera_test: PASS");
  return 0;
}
