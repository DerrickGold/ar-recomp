#include "sim/sim3d.h"
#include "sim/sim3d_camera_limits.h"
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
static SimRenderFeatureMask requested_features = kSimFeature_All;
uint64_t HostClock_Milliseconds(void) { return 1000; }
SimRenderFeatureMask Sim3D_ImplementedFeatures(void) { return kSimFeature_All; }
SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void) { return requested_features; }
const SettingDesc *Settings_Find(const char *key) { (void)key; settings_writes++; return NULL; }
SettingChangeResult Settings_Reset(const SettingDesc *desc) { (void)desc; settings_writes++; return kSettingChange_Applied; }
bool Settings_Save(const char *path) { (void)path; settings_writes++; return true; }
char *UserDataFile(char *buf, size_t size, const char *leaf) { (void)size; (void)leaf; return buf; }

static void TestTownZoomFromVisiblePose(void) {
  g_settings.sim3d_mode = true;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  for (int mode = kSimCam_Free; mode <= kSimCam_Dynamic; ++mode) {
    g_settings.sim3d_camera_mode = mode;
    g_settings.sim3d_distance_x100 = g_settings.sim3d_dyncam_baseline_distance_x100 = 2000;
    int *distance = mode == kSimCam_Free ? &g_settings.sim3d_distance_x100
        : &g_settings.sim3d_dyncam_baseline_distance_x100;
    const int *other = mode == kSimCam_Free ? &g_settings.sim3d_dyncam_baseline_distance_x100
        : &g_settings.sim3d_distance_x100;
    Sim3DCameraPresentationState state;
    assert(Sim3DCamera_ControlsAvailable(true));
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 450 && *distance == 2000);
    Sim3DCamera_Adjust(0, 0, -.25f); /* Even a partial wheel step must be visible. */
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 425 && *distance == 425 && *other == 2000);
    Sim3DCamera_Adjust(0, 0, 100);
    assert(*distance == 450); /* No hidden travel past the rendered ceiling. */
    Sim3DCamera_Adjust(0, 0, -.25f);
    assert(*distance == 425);
    Sim3DCamera_Adjust(0, 0, -100);
    assert(*distance == 200);
    Sim3DCamera_Adjust(0, 0, .25f);
    assert(*distance == 225); /* Reversing at the near limit works too. */
    *distance = 0;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 0); /* Keep auto-fit's existing meaning. */
    Sim3DCamera_Adjust(0, 0, -.25f);
    assert(*distance > 200 && *distance < 247);
    /* Flat-underlay users retain their larger inspection range. */
    requested_features &= ~kSimFeature_GlobeUnderlay;
    *distance = 550;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 550);
    Sim3DCamera_Adjust(0, 0, -.25f);
    assert(*distance == 525);
    Sim3DCamera_Adjust(0, 0, 100);
    assert(*distance == 2000);
    requested_features = kSimFeature_All;
    /* Navigation keeps its own native-scale baseline and wider zoom range. */
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 0);
    Sim3DCamera_Adjust(0, 0, 100);
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 2000 && *distance == 2000);
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.distance_x100 == 450 && *distance == 2000);
    Sim3DCamera_Adjust(0, 0, -.25f);
    assert(*distance == 425);
  }
}

static float CapturedTownYaw(void) {
  Sim3DCameraPresentationState state;
  Sim3DCamera_CapturePresentationState(&state);
  return state.yaw_mrad / 1000.0f +
      (state.mode == kSimCam_Dynamic ? state.orbit_yaw : 0);
}

static void ResetTownOrbit(void) {
  g_settings.sim3d_camera_mode = kSimCam_Free;
  Sim3DCamera_UpdateDynamic(.1f, false);
}

static void TestTownRotationFromVisiblePose(void) {
  const float limit = kSim3DConnectedCameraYawMaximumMrad / 1000.0f;
  g_settings.sim3d_mode = true;
  g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  requested_features = kSimFeature_All;
  for (int mode = kSimCam_Free; mode <= kSimCam_Dynamic; ++mode) {
    for (int sign = -1; sign <= 1; sign += 2) {
      ResetTownOrbit();
      g_settings.sim3d_camera_mode = mode;
      g_settings.sim3d_tilt_y_mrad = g_settings.sim3d_dyncam_baseline_tilt_y_mrad = sign*700;
      const Settings saved = g_settings;
      assert(fabsf(CapturedTownYaw() - sign*limit) < .00001f);
      assert(!memcmp(&saved, &g_settings, sizeof(saved))); /* Capture is not a save migration. */
      Sim3DCamera_Adjust(-sign*.05f, 0, 0);
      assert(fabsf(CapturedTownYaw() - sign*(limit-.05f)) < .00001f);
      Sim3DCamera_Adjust(sign*100, 0, 0);
      assert(fabsf(CapturedTownYaw() - sign*limit) < .00001f);
      Sim3DCamera_Adjust(-sign*.05f, 0, 0);
      assert(fabsf(CapturedTownYaw() - sign*(limit-.05f)) < .00001f);
      if (mode == kSimCam_Dynamic) {
        assert(g_settings.sim3d_dyncam_baseline_tilt_y_mrad == sign*700);
        Sim3DCamera_UpdateDynamic(.035f, false);
        const float returned = sign*CapturedTownYaw();
        assert(returned > limit-.05f && returned < limit);
      }
    }
  }
  /* Enabling the globe while an ordinary dynamic orbit is held must clamp
   * both the captured view and the next input/return animation immediately. */
  for (int sign = -1; sign <= 1; sign += 2) {
    for (int release = 0; release < 2; ++release) {
      ResetTownOrbit();
      g_settings.sim3d_camera_mode = kSimCam_Dynamic;
      g_settings.sim3d_dyncam_baseline_tilt_y_mrad = 0;
      requested_features &= ~kSimFeature_GlobeUnderlay;
      Sim3DCamera_Adjust(sign*100, 0, 0);
      assert(fabsf(CapturedTownYaw() - sign*.7f) < .00001f);
      requested_features = kSimFeature_All;
      assert(fabsf(CapturedTownYaw() - sign*limit) < .00001f);
      float yaw;
      Sim3DCamera_GetDynamicOrbit(&yaw, NULL);
      assert(fabsf(yaw - sign*limit) < .00001f);
      if (release) {
        assert(Sim3DCamera_UpdateDynamic(.035f, false));
        assert(sign*CapturedTownYaw() > 0 && sign*CapturedTownYaw() < limit);
      } else {
        Sim3DCamera_Adjust(-sign*.05f, 0, 0);
        assert(fabsf(CapturedTownYaw() - sign*(limit-.05f)) < .00001f);
      }
    }
  }
  ResetTownOrbit();
  /* Ordinary SIM retains its wider inspection range and saved pose. */
  requested_features &= ~kSimFeature_GlobeUnderlay;
  g_settings.sim3d_tilt_y_mrad = 650;
  assert(fabsf(CapturedTownYaw() - .65f) < .00001f);
  Sim3DCamera_Adjust(.05f, 0, 0);
  assert(g_settings.sim3d_tilt_y_mrad == 700);
  requested_features = kSimFeature_All;
}

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
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 0);
  g_settings.sim3d_distance_x100 = 550;
  g_settings.sim3d_dyncam_baseline_distance_x100 = 450;
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.distance_x100 == 0); /* Town zoom must not shrink the world. */
  g_settings = original;
  Sim3DCamera_Adjust(3.5f, 20, 2);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw < -2.7f && state.orbit_yaw > -3.14f);
  assert(fabsf(state.orbit_pitch - 1.57079632679f) < .00001f);
  assert(state.distance_x100 == 447);
  const float held = state.orbit_yaw;
  assert(!Sim3DCamera_UpdateDynamic(.1f, true)); /* Holding a pose needs no re-aim animation. */
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == held);
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
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 0);
  Sim3DCamera_FlushSettingsIfDirty();
  assert(settings_writes == 0 && !memcmp(&g_settings, &original, sizeof(original)));
  /* The radial camera is always centered. Held-neutral input and zoom alone
   * must not start a redundant centering animation or request extra redraws. */
  assert(!Sim3DCamera_UpdateDynamic(2, true));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == 0);
  Sim3DCamera_Adjust(6.28318530718f, 0, 0);
  assert(!Sim3DCamera_UpdateDynamic(.1f, true));
  Sim3DCamera_CapturePresentationState(&state);
  assert(fabsf(state.orbit_yaw) < .00001f);
  assert(!Sim3DCamera_UpdateDynamic(NAN, false));
  assert(!Sim3DCamera_UpdateDynamic(-1, false));
  Sim3DCamera_Reset();
  Sim3DCamera_Adjust(.8f, -.4f, 0);
  Sim3DCamera_UpdateDynamic(.6f, false);
  Sim3DCamera_CapturePresentationState(&state);
  const float single_step_yaw = state.orbit_yaw, single_step_pitch = state.orbit_pitch;
  Sim3DCamera_Reset();
  Sim3DCamera_Adjust(.8f, -.4f, 0);
  for (int i = 0; i < 60; i++) Sim3DCamera_UpdateDynamic(.01f, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(fabsf(single_step_yaw - state.orbit_yaw) < .000001f);
  assert(fabsf(single_step_pitch - state.orbit_pitch) < .000001f);
  Sim3DCamera_Reset();
  Sim3DCamera_Adjust(0, 0, .5f);
  assert(!Sim3DCamera_UpdateDynamic(2, false));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 297);
  Sim3DCamera_Adjust(0, 0, 1);
  assert(!Sim3DCamera_UpdateDynamic(2, false));
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.distance_x100 == 397);
  Sim3DCamera_Adjust(0, 0, -1.5f);
  Sim3DCamera_UpdateDynamic(10, false);
  Sim3DCamera_CapturePresentationState(&state);
  assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 0);
  /* Leave/re-enter and master Off/On must not leak the globe's pose or zoom
   * into a town, action room, or a later navigation visit. */
  for (int mode = kSimCam_Free; mode <= kSimCam_Dynamic; mode++) {
    g_settings.sim3d_camera_mode = mode;
    Sim3DCamera_Adjust(1, -.4f, 4);
    Sim3DCamera_UpdateDynamic(2, true);
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.orbit_pitch == 0 && state.distance_x100 == 300);
    assert(!Sim3DCamera_ControlsAvailable(true));
    g_settings.sim3d_mode = 1;
    assert(Sim3DCamera_ControlsAvailable(true));
    g_ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.distance_x100 == 0);
    Sim3DCamera_Adjust(.5f, .3f, 1);
    g_settings.sim3d_world_navigation = false;
    assert(!Sim3DCamera_ControlsAvailable(true));
    Sim3DCamera_CapturePresentationState(&state);
    g_settings.sim3d_world_navigation = true;
    Sim3DCamera_CapturePresentationState(&state);
    assert(state.orbit_yaw == 0 && state.distance_x100 == 0);
    g_settings.sim3d_mode = 0;
  }
  assert(settings_writes == 0);
  TestTownZoomFromVisiblePose();
  TestTownRotationFromVisiblePose();
  puts("sim3d_camera_test: PASS");
  return 0;
}
