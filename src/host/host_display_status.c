#include "host_display_status.h"

static int s_nominal_refresh_hz;
static bool s_vsync_active;

void HostDisplayStatus_SetNominalRefreshHz(int hz) {
  s_nominal_refresh_hz = hz > 0 ? hz : 0;
}

int HostDisplayStatus_NominalRefreshHz(void) {
  return s_nominal_refresh_hz;
}

void HostDisplayStatus_SetVsyncActive(bool active) {
  s_vsync_active = active;
}

bool HostDisplayStatus_VsyncActive(void) {
  return s_vsync_active;
}
