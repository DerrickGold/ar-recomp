#include "support/test_assert.h"
#include "actraiser2/module/ar2_world.h"

int main(void) {
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
  return 0;
}
