#include "sim/church/church_detail.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c)                                                                                   \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                                              \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)
static void Quad(ChurchFocus *f, float distance) {
  const Scene3DClipPoint p[4] = {{-distance, -distance, 0, distance},
                                 {distance, -distance, 0, distance},
                                 {distance, distance, 0, distance},
                                 {-distance, distance, 0, distance}};
  ChurchFocus_Quad(f, p);
}
int main(void) {
  ChurchFocus f;
  ChurchFocus_Begin(&f, -1, 1, -1, 1);
  float sky = ChurchFocus_Blur(&f, 0, 0);
  Quad(&f, 120);
  float near = ChurchFocus_Blur(&f, 0, 0);
  CHECK(near >= .6f && near < .7f && sky > .85f);
  Quad(&f, 900);
  CHECK(ChurchFocus_Blur(&f, 0, 0) == near); /* far cannot overwrite near */
  ChurchFocus_Begin(&f, -1, 1, -1, 1);
  Quad(&f, 900);
  Quad(&f, 120);
  CHECK(ChurchFocus_Blur(&f, 0, 0) == near); /* submission order independent */
  Quad(&f, -2);
  CHECK(ChurchFocus_Blur(&f, 0, 0) == near);
  for (int i = 0; i < 32; i++) {
    const ArRenderColorF input = {i / 31.0f, i / 34.0f, i / 40.0f, 1};
    ArRenderColorF a = ChurchDetail_Stone(input, i), b = ChurchDetail_Stone(input, i);
    CHECK(!memcmp(&a, &b, sizeof(a)) && a.r >= a.g && a.g >= a.b && a.r <= 1);
  }
  CHECK(ChurchDetail_Phase(0, 4, 8) == 0 && ChurchDetail_Phase(.125, 4, 8) == 1);
  CHECK(ChurchDetail_Phase(.5, 4, 8) == 0 && ChurchDetail_Phase(NAN, 4, 8) == 0);
  CHECK(ChurchDetail_Phase(10000000.125, 4, 8) == 1);
  puts("church details: near occlusion, bounded palette and deterministic animation PASS");
  return 0;
}
