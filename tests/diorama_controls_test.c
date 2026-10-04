#include "diorama/diorama.h"
#include "diorama/diorama_layer_manifest.h"
#include "diorama/diorama_rom_skybox_resource.h"
#include "app/settings.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>

Settings g_settings;
static uint64_t now_ms;
static unsigned saves;
uint64_t HostClock_Milliseconds(void) { return now_ms; }
char *UserDataFile(char *buf, size_t size, const char *leaf) {
  snprintf(buf, size, "%s", leaf); return buf;
}
bool Settings_SaveDeferred(const char *path) { (void)path; ++saves; return true; }
const SettingDesc *Settings_Find(const char *key) { (void)key; return NULL; }
SettingChangeResult Settings_Reset(const SettingDesc *desc) {
  (void)desc; return kSettingChange_Unchanged;
}
const DioramaLayerOrderTable *DioramaLayerManifest_Table(void) { return NULL; }
bool DioramaRomSkyboxResource_Init(const uint8_t *rom, size_t size) {
  (void)rom; (void)size; return false;
}
ArRenderTexture DioramaRomSkyboxResource_Resolve(ArRenderDevice *device,
    int source, bool fill, uint32_t color, bool *failed) {
  (void)device; (void)source; (void)fill; (void)color; (void)failed;
  return (ArRenderTexture){0};
}
void DioramaRomSkyboxResource_Reset(ArRenderDevice *device) { (void)device; }
void Diorama_ResetCompositorResources(ArRenderDevice *device) { (void)device; }
void DioramaUpload_Reset(void) {}

int main(void) {
  g_settings.diorama_camera_mode = kDioramaCam_Free;
  g_settings.diorama_distance_x100 = 500;
  Diorama_SeedCameraFromSettings();
  Diorama_AdjustCamera(.1f, -.2f, .5f);
  /* While the producer owns game state, presentation can change its pose
   * without changing settings that the producer may be reading. */
  assert(g_settings.diorama_tilt_x_mrad == 0);
  assert(g_settings.diorama_tilt_y_mrad == 0);
  assert(g_settings.diorama_distance_x100 == 500);
  DioramaCameraPresentationState state;
  Diorama_CaptureCameraPresentationState(&state);
  assert(fabsf(state.free_pose.tilt_x + .2f) < .0001f);
  assert(fabsf(state.free_pose.tilt_y - .1f) < .0001f);
  assert(state.free_pose.distance == 5.5f);
  /* Ownership returned: settings synchronize now, disk debounce remains. */
  Diorama_FlushSettingsIfDirty();
  assert(g_settings.diorama_tilt_x_mrad == -200);
  assert(g_settings.diorama_tilt_y_mrad == 100);
  assert(g_settings.diorama_distance_x100 == 550 && saves == 0);
  now_ms = 501;
  Diorama_FlushSettingsIfDirty();
  assert(saves == 1);
  /* Explicit menu pose edits replace pending presentation input. */
  Diorama_AdjustCamera(.1f, 0, 0);
  g_settings.diorama_tilt_y_mrad = 300;
  Diorama_SeedCameraFromSettings();
  Diorama_FlushSettingsIfDirty();
  Diorama_CaptureCameraPresentationState(&state);
  assert(g_settings.diorama_tilt_y_mrad == 300);
  assert(fabsf(state.free_pose.tilt_y - .3f) < .0001f);
  puts("diorama_controls_test: PASS");
  return 0;
}
