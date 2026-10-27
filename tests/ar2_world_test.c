#include "support/test_assert.h"
#include "actraiser2/module/ar2_world.h"

int main(void) {
  /* The research executable's broader catalog must not enable shipping warps. */
  for (unsigned scene = 0; scene < 256; ++scene) {
    assert(Ar2World_SupportsEntrance(scene) == (scene == 0x14 || scene == 0x16));
    assert(Ar2World_SupportsScene(scene) ==
           (scene == 0x14 || scene == 0x15 || scene == 0x16));
  }
  const int margins[] = {0, 43, 72, 128};
  for (unsigned i = 0; i < sizeof(margins) / sizeof(*margins); i++) {
    const int margin = margins[i];
    int previous = -1;
    for (int camera = 0; camera <= 768; camera++) {
      const int shift = Ar2World_CaptureShift(camera, 1024, margin);
      const int origin = camera + shift - margin;
      assert(origin >= 0 && origin + 256 + margin * 2 <= 1024);
      assert(origin >= previous && (previous < 0 || origin <= previous + 1));
      assert(shift == -Ar2World_CaptureShift(768 - camera, 1024, margin));
      /* Terrain and a world-space actor meet at the same captured pixel. */
      const int actor = 400;
      assert(actor - camera - shift + margin == actor - origin);
      if (!margin) assert(shift == 0);
      previous = origin;
    }
  }
  for (int camera = 0; camera <= 128; camera++) {
    const int shifted = camera + Ar2World_CaptureShift(camera, 384, 128);
    assert(shifted == 64); /* A smaller room stays centered at either native edge. */
  }
  assert(Ar2World_CaptureShift(0, 0, 128) == 0);
  assert(Ar2World_CaptureShift(-1, 1024, 128) == 0);
  assert(Ar2World_CaptureShift(769, 1024, 128) == 0);

  static Ar2World world;
  world.valid = true;
  const unsigned ratios[] = {0x0201, 0x0403, 0x0101, 0};
  const int widths[] = {384, 1024};
  for (unsigned r = 0; r < sizeof(ratios) / sizeof(*ratios); ++r) {
    world.background_x_ratio = ratios[r];
    const int numerator = ratios[r] & 255;
    const int denominator = ratios[r] ? ratios[r] >> 8 : 1;
    for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); ++w) {
      for (unsigned m = 0; m < sizeof(margins) / sizeof(*margins); ++m) {
        int previous_foreground = -1, previous_background = -1;
        for (int camera = 0; camera <= widths[w] - 256; ++camera) {
          world.layers[0].camera_x = camera;
          const int view = Ar2World_CaptureShift(camera, widths[w], margins[m]);
          const int capture = Ar2World_CaptureShift(camera, widths[w], 128);
          const int foreground = camera + view;
          const int native_background = camera * numerator / denominator;
          const int background = native_background +
              Ar2World_BackgroundCaptureShift(&world, capture, view) + view - capture;
          /* Holding the foreground at either edge also holds its background.
           * Once it moves, native parallax keeps its subpixel rounding phase. */
          if (foreground == previous_foreground) assert(background == previous_background);
          assert(background * denominator <= foreground * numerator);
          assert((background + 1) * denominator > foreground * numerator);
          /* A larger diorama apron must not change the visible background. */
          assert(background == native_background +
              Ar2World_BackgroundCaptureShift(&world, view, view));
          previous_foreground = foreground;
          previous_background = background;
        }
      }
    }
  }
  /* Native timed scrolling includes foreground motion at full speed. Preserve
   * that authored movement while sharing the foreground's visible stop. */
  world.background_x_ratio = 1;
  world.background_x_autoscroll = true;
  assert(Ar2World_BackgroundCaptureShift(&world, 90, 13) == 90);
  world.background_x_ratio = 0;
  assert(Ar2World_BackgroundCaptureShift(&world, 90, 13) == 77);
  return 0;
}
