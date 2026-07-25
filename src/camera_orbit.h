#ifndef CAMERA_ORBIT_H
#define CAMERA_ORBIT_H

#include <stdbool.h>

/* Player-owned orbit layered over an authored dynamic-camera baseline. The
 * offset is deliberately transient: input moves it immediately, then it
 * returns to zero when orbit input is released. */
typedef struct CameraOrbit {
  float yaw;
  float pitch;
} CameraOrbit;

void CameraOrbit_Adjust(CameraOrbit *orbit, float yaw_delta, float pitch_delta,
                        float baseline_yaw, float baseline_pitch,
                        float minimum_tilt, float maximum_tilt);

/* Advances the return-to-baseline motion. Returns true when the offset
 * changed. Holding the orbit control freezes the current offset even when the
 * mouse/stick is momentarily stationary. */
bool CameraOrbit_Update(CameraOrbit *orbit, float elapsed_seconds,
                        bool input_held, float return_time_seconds);

void CameraOrbit_Reset(CameraOrbit *orbit);

#endif  /* CAMERA_ORBIT_H */
