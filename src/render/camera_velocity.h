#ifndef AR_CAMERA_VELOCITY_H
#define AR_CAMERA_VELOCITY_H

#include <math.h>
#include <stdint.h>

/* Calibrate reactive lean against recent motion, rather than a permanent
 * peak. Action and town cameras keep separate averages because their actors
 * move at different scales. Advance by emulated ticks: a retained/paused
 * frame must not change the calibration just because it is drawn again. */
static inline float CameraVelocity_Normalize(int16_t velocity, float *average,
                                             int elapsed_ticks) {
  const float minimum_reference = 4.0f;
  const float average_alpha = 0.02f; /* About 0.8 seconds at the game tick rate. */
  const float full_lean_multiple = 3.0f;
  float magnitude = fabsf((float)velocity);
  for (int tick = 0; tick < elapsed_ticks; tick++)
    *average += (magnitude - *average) * average_alpha;
  float reference = *average * full_lean_multiple;
  if (reference < minimum_reference) reference = minimum_reference;
  float normalized = (float)velocity / reference;
  if (normalized > 1.0f) normalized = 1.0f;
  if (normalized < -1.0f) normalized = -1.0f;
  return normalized;
}

#endif /* AR_CAMERA_VELOCITY_H */
