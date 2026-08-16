#include "camera_orbit.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    failures++; \
  } \
} while (0)

static bool Near(float actual, float expected) {
  return fabsf(actual - expected) < 0.0001f;
}

int main(void) {
  CameraOrbit orbit = {0};
  CameraOrbit_Adjust(&orbit, 0.2f, -0.3f, 0.1f, -0.1f,
                     -0.7f, 0.7f, -0.7f, 0.7f);
  CHECK(Near(orbit.yaw, 0.2f));
  CHECK(Near(orbit.pitch, -0.3f));

  /* The baseline participates in the clamp: the transient offset can never
   * push the authored camera past the supported projection range. */
  CameraOrbit_Adjust(&orbit, 2.0f, -2.0f, 0.1f, -0.1f,
                     -0.7f, 0.7f, -0.7f, 0.7f);
  CHECK(Near(orbit.yaw, 0.6f));
  CHECK(Near(orbit.pitch, -0.6f));

  /* Yaw and pitch are independent axes. SIM uses symmetric yaw limits and
   * an all-negative pitch interval, so reusing pitch bounds for yaw would
   * make rightward orbit impossible. */
  CameraOrbit asymmetric = {0};
  CameraOrbit_Adjust(&asymmetric, 2.0f, 2.0f, 0.0f, -0.575f,
                     -0.7f, 0.7f, -1.35f, -0.575f);
  CHECK(Near(asymmetric.yaw, 0.7f));
  CHECK(Near(asymmetric.pitch, 0.0f));
  CameraOrbit_Adjust(&asymmetric, -4.0f, -2.0f, 0.0f, -0.575f,
                     -0.7f, 0.7f, -1.35f, -0.575f);
  CHECK(Near(asymmetric.yaw, -0.7f));
  CHECK(Near(asymmetric.pitch, -0.775f));

  CameraOrbit held = orbit;
  CHECK(!CameraOrbit_Update(&held, 1.0f, true, 0.35f));
  CHECK(Near(held.yaw, orbit.yaw));
  CHECK(Near(held.pitch, orbit.pitch));

  /* Exponential return is frame-rate independent. */
  CameraOrbit one_step = orbit;
  CameraOrbit two_steps = orbit;
  CHECK(CameraOrbit_Update(&one_step, 0.2f, false, 0.35f));
  CHECK(CameraOrbit_Update(&two_steps, 0.1f, false, 0.35f));
  CHECK(CameraOrbit_Update(&two_steps, 0.1f, false, 0.35f));
  CHECK(Near(one_step.yaw, two_steps.yaw));
  CHECK(Near(one_step.pitch, two_steps.pitch));
  CHECK(fabsf(one_step.yaw) < fabsf(orbit.yaw));
  CHECK(fabsf(one_step.pitch) < fabsf(orbit.pitch));

  CameraOrbit_Update(&one_step, 10.0f, false, 0.35f);
  CHECK(one_step.yaw == 0.0f);
  CHECK(one_step.pitch == 0.0f);

  CameraOrbit_Reset(&orbit);
  CHECK(orbit.yaw == 0.0f && orbit.pitch == 0.0f);

  if (!failures) puts("camera orbit tests passed");
  return failures ? 1 : 0;
}
