#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sim/sim_world_navigation_globe.h"

static float Dot(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void TestHorizonBounds(void) {
  const float eye[3] = {0, 0, 3};
  const float limb[3] = {sinf(1.6f), 0, cosf(1.6f)};
  assert(SimWorldNavigationGlobe_CapOccluded(eye, limb, 0, 1, 1));
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, limb, 0, 1.5f, 1)); /* Tower tip. */
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, limb, .4f, 1, 1)); /* Overhanging crown. */
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, limb, NAN, 1, 1));
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, limb, 0, INFINITY, 1));
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, limb, 0, 1, -1));
  assert(!SimWorldNavigationGlobe_CapOccluded((float[]){0, 0, .5f}, limb, 0, 1, 1));
  assert(!SimWorldNavigationGlobe_CapOccluded(eye, (float[]){0, 0, 0}, 0, 1, 1));
  assert(!SimWorldNavigationGlobe_CapOccluded(NULL, limb, 0, 1, 1));
  int culled = 0, retained = 0;
  /* Independent segment/sphere oracle. Every sampled point in a rejected
   * curved footprint, at multiple heights, must have an intervening opaque
   * sphere hit. Sweep near/far eyes, tall objects and the entire far side. */
  for (int e = 0; e < 3; e++)
    for (int h = 0; h < 5; h++)
      for (int w = 0; w < 5; w++)
        for (int t = 0; t <= 64; t++) {
          const float camera[3] = {0, 0, 1.1f + e * 2};
          const float theta = t * 3.14159265359f / 64;
          const float axis[3] = {sinf(theta), 0, cosf(theta)};
          const float width = w * .1f, radius = 1 + h * .5f;
          if (!SimWorldNavigationGlobe_CapOccluded(camera, axis, width, radius, 1)) {
            retained++;
            continue;
          }
          culled++;
          for (int ring = 0; ring <= 2; ring++)
            for (int sector = 0; sector < 16; sector++)
              for (int height = 1; height <= 3; height++) {
                const float angle = width * ring / 2;
                const float azimuth = sector * 6.28318530718f / 16;
                const float r = radius * height / 3;
                const float point[3] = {
                  r * (cosf(angle) * axis[0] + sinf(angle) * cosf(azimuth) * cosf(theta)),
                  r * sinf(angle) * sinf(azimuth),
                  r * (cosf(angle) * axis[2] - sinf(angle) * cosf(azimuth) * sinf(theta)),
                };
                float ray[3], closest[3];
                for (int i = 0; i < 3; i++) ray[i] = point[i] - camera[i];
                const float at = fminf(1, fmaxf(0, -Dot(camera, ray) / Dot(ray, ray)));
                for (int i = 0; i < 3; i++) closest[i] = camera[i] + at * ray[i];
                assert(Dot(closest, closest) < 1);
              }
        }
  assert(culled > 100 && retained > 100);
}

static void TestChart(float radius) {
  const float focus[][2] = {
    {96, 64}, {64, 64}, {32, 80}, {32, 48}, {80, 112}, {48, 16}, {0, 128},
  };
  float a[3], b[3], rotated_a[3], rotated_b[3];
  for (int y = 0; y <= 128; y += 4) {
    for (int x = 0; x <= 128; x += 4) {
      float scale, restored_x, restored_y;
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, x, y, a, &scale));
      if (radius == kSimWorldNavigationGlobeRadiusTiles) {
        float legacy[3], legacy_scale;
        assert(SimWorldNavigationGlobe_Sample(x, y, legacy, &legacy_scale));
        assert(!memcmp(a, legacy, sizeof(legacy)) && scale == legacy_scale);
      }
      assert(fabsf(Dot(a, a) - 1) < 0.000001f);
      assert(scale > 0.5f && scale <= 1.0f);
      assert(SimWorldNavigationGlobe_SourceAtRadius(radius, a, &restored_x, &restored_y));
      if (radius == kSimWorldNavigationGlobeRadiusTiles) {
        float legacy_x, legacy_y;
        assert(SimWorldNavigationGlobe_Source(a, &legacy_x, &legacy_y));
        assert(legacy_x == restored_x && legacy_y == restored_y);
      }
      assert(fabsf(restored_x - x) < 0.00003f);
      assert(fabsf(restored_y - y) < 0.00003f);
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, 128 - x, 128 - y, b, NULL));
      const float original_dot = Dot(a, b);
      float west[3], east[3], north[3], south[3], dx[3], dy[3];
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, x - 0.25f, y, west, NULL));
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, x + 0.25f, y, east, NULL));
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, x, y - 0.25f, north, NULL));
      assert(SimWorldNavigationGlobe_SampleAtRadius(radius, x, y + 0.25f, south, NULL));
      for (int i = 0; i < 3; i++) {
        dx[i] = east[i] - west[i];
        dy[i] = south[i] - north[i];
      }
      const float dx_length = sqrtf(Dot(dx, dx));
      const float dy_length = sqrtf(Dot(dy, dy));
      /* Local town blocks stay square everywhere, not only at the equator.
       * Model height uses this same metric to retain authored proportions. */
      assert(fabsf(dx_length / dy_length - 1) < 0.001f);
      assert(fabsf(Dot(dx, dy) / (dx_length * dy_length)) < 0.001f);
      assert(fabsf(dx_length * (2 * radius) - scale)
          < 0.0001f);
      for (unsigned f = 0; f < sizeof(focus) / sizeof(focus[0]); f++) {
        for (int spin = 0; spin < 4; spin++) {
          SimWorldNavigationGlobeFrame frame;
          assert(SimWorldNavigationGlobe_BuildFrameAtRadius(
              radius, focus[f][0], focus[f][1], spin * 1.57079632679f, &frame));
          if (radius == kSimWorldNavigationGlobeRadiusTiles) {
            SimWorldNavigationGlobeFrame legacy;
            assert(SimWorldNavigationGlobe_BuildFrame(
                focus[f][0], focus[f][1], spin * 1.57079632679f, &legacy));
            assert(!memcmp(&legacy, &frame, sizeof(frame)));
          }
          assert(fabsf(Dot(frame.right, frame.up)) < 0.000001f);
          assert(fabsf(Dot(frame.right, frame.outward)) < 0.000001f);
          assert(fabsf(Dot(frame.up, frame.outward)) < 0.000001f);
          assert(fabsf(Dot(frame.right, frame.right) - 1) < 0.000001f);
          assert(fabsf(Dot(frame.up, frame.up) - 1) < 0.000001f);
          SimWorldNavigationGlobe_TransformNormal(&frame, a, rotated_a);
          SimWorldNavigationGlobe_TransformNormal(&frame, b, rotated_b);
          /* Moving the Palace is a rigid rotation: angular distances between
           * arbitrary town/terrain points must not change with focus/spin. */
          assert(fabsf(Dot(rotated_a, rotated_b) - original_dot) < 0.000001f);
          SimWorldNavigationGlobe_TransformNormal(
              &frame, frame.outward, rotated_a);
          assert(fabsf(rotated_a[0]) < 0.000001f);
          assert(fabsf(rotated_a[1]) < 0.000001f);
          assert(fabsf(rotated_a[2] - 1) < 0.000001f);
        }
      }
    }
  }
  /* The uncharted hemisphere is real spherical space, not compressed back
   * onto the near limb. The inverse can address it outside native map bounds. */
  float x, y;
  assert(SimWorldNavigationGlobe_SampleAtRadius(radius, 400, 64, a, NULL));
  assert(a[2] < 0);
  assert(SimWorldNavigationGlobe_SourceAtRadius(radius, a, &x, &y));
  assert(fabsf(x - 400) < 0.001f && fabsf(y - 64) < 0.001f);
  assert(!SimWorldNavigationGlobe_Source((float[]){0, 0, -1}, &x, &y));
  assert(!SimWorldNavigationGlobe_Source((float[]){0, 0, 0}, &x, &y));
  assert(!SimWorldNavigationGlobe_Sample(NAN, 64, a, NULL));
  assert(!SimWorldNavigationGlobe_Sample(64, INFINITY, a, NULL));
  SimWorldNavigationGlobeFrame frame;
  assert(!SimWorldNavigationGlobe_BuildFrame(64, 64, NAN, &frame));
  for (unsigned f = 0; f < sizeof(focus) / sizeof(focus[0]); f++) {
    assert(SimWorldNavigationGlobe_BuildFrameAtRadius(radius, focus[f][0], focus[f][1], .37f, &frame));
    const SimWorldNavigationGlobeFrame original = frame;
    assert(SimWorldNavigationGlobe_OrbitFrame(&frame, 0, 0));
    assert(!memcmp(&frame, &original, sizeof(frame)));
    assert(!SimWorldNavigationGlobe_OrbitFrame(&frame, NAN, 0));
    assert(!memcmp(&frame, &original, sizeof(frame)));
    for (int yaw = -8; yaw <= 8; yaw++)
      for (int pitch = -4; pitch <= 4; pitch++) {
        frame = original;
        assert(SimWorldNavigationGlobe_OrbitFrame(&frame, yaw * .7853981634f, pitch * .3926990817f));
        assert(fabsf(Dot(frame.right, frame.up)) < .000001f);
        assert(fabsf(Dot(frame.right, frame.outward)) < .000001f);
        assert(fabsf(Dot(frame.up, frame.outward)) < .000001f);
        assert(fabsf(Dot(frame.right, frame.right) - 1) < .000001f);
        assert(fabsf(Dot(frame.up, frame.up) - 1) < .000001f);
        assert(fabsf(Dot(frame.outward, frame.outward) - 1) < .000001f);
        SimWorldNavigationGlobe_TransformNormal(&frame, original.outward, a);
        if (yaw == 4 && !pitch) assert(a[2] < -.999999f); /* Real far hemisphere. */
        if (!yaw && pitch == 4) assert(fabsf(a[2]) < .000001f); /* Pole, no singularity. */
      }
  }
}

static void TestInvalidRadii(void) {
  const float invalid[] = {0, -1, NAN, INFINITY, FLT_MAX, FLT_MIN};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    float normal[3] = {1, 2, 3}, scale = 42, x = 17, y = 19;
    SimWorldNavigationGlobeFrame frame;
    assert(SimWorldNavigationGlobe_BuildFrame(32, 48, .37f, &frame));
    const SimWorldNavigationGlobeFrame original = frame;
    assert(!SimWorldNavigationGlobe_SampleAtRadius(invalid[i], 32, 48, normal, &scale));
    assert(normal[0] == 1 && normal[1] == 2 && normal[2] == 3 && scale == 42);
    assert(!SimWorldNavigationGlobe_BuildFrameAtRadius(invalid[i], 32, 48, 0, &frame));
    assert(!memcmp(&original, &frame, sizeof(frame)));
    /* Tiny positive radii can still invert a representable sphere normal. */
    if (invalid[i] == FLT_MIN) continue;
    assert(!SimWorldNavigationGlobe_SourceAtRadius(invalid[i], normal, &x, &y));
    assert(x == 17 && y == 19);
  }
}

int main(void) {
  TestHorizonBounds();
  TestChart(kSimWorldNavigationGlobeRadiusTiles);
  TestChart(kSimWorldNavigationGlobeRadiusTiles * 3.0f);
  TestInvalidRadii();
  puts("sim_world_navigation_globe_test: PASS");
  return 0;
}
