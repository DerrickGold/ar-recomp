#ifndef SIM3D_CAMERA_LIMITS_H
#define SIM3D_CAMERA_LIMITS_H

/* Mountain art is an authored front facade with only a shallow tapered rear
 * stack. Keep the default three-quarter view as the most overhead pose, and
 * spend the remaining pitch range toward a low near-horizontal view. Crossing
 * either zero or the authored overhead angle would still expose the finite
 * stack as terrain instead of the intended pseudo-3D range. */
enum {
  kSim3DCameraPitchMinimumMrad = -1350,
  kSim3DCameraPitchMaximumMrad = -575,
  kSim3DCameraYawMinimumMrad = -700,
  kSim3DCameraYawMaximumMrad = 700,
};

#endif  /* SIM3D_CAMERA_LIMITS_H */
