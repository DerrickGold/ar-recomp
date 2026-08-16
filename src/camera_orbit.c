#include "camera_orbit.h"

#include <math.h>

static float ClampFloat(float value, float minimum, float maximum) {
  if (value < minimum) return minimum;
  if (value > maximum) return maximum;
  return value;
}

void CameraOrbit_Adjust(CameraOrbit *orbit, float yaw_delta, float pitch_delta,
                        float baseline_yaw, float baseline_pitch,
                        float minimum_yaw, float maximum_yaw,
                        float minimum_pitch, float maximum_pitch) {
  if (!orbit) return;
  orbit->yaw = ClampFloat(
      baseline_yaw + orbit->yaw + yaw_delta, minimum_yaw, maximum_yaw) -
      baseline_yaw;
  orbit->pitch = ClampFloat(
      baseline_pitch + orbit->pitch + pitch_delta,
      minimum_pitch, maximum_pitch) - baseline_pitch;
}

bool CameraOrbit_Update(CameraOrbit *orbit, float elapsed_seconds,
                        bool input_held, float return_time_seconds) {
  if (!orbit || input_held || elapsed_seconds <= 0.0f ||
      (orbit->yaw == 0.0f && orbit->pitch == 0.0f))
    return false;

  if (return_time_seconds <= 0.0f) {
    CameraOrbit_Reset(orbit);
    return true;
  }

  const float decay = expf(-elapsed_seconds / return_time_seconds);
  orbit->yaw *= decay;
  orbit->pitch *= decay;
  /* Avoid an asymptotic tail that keeps requesting redraws forever. */
  if (fabsf(orbit->yaw) < 0.0001f) orbit->yaw = 0.0f;
  if (fabsf(orbit->pitch) < 0.0001f) orbit->pitch = 0.0f;
  return true;
}

void CameraOrbit_Reset(CameraOrbit *orbit) {
  if (!orbit) return;
  *orbit = (CameraOrbit){0};
}
