#include "sim/church/church_lighting.h"
#include <math.h>
#include <stdio.h>

static int s_failures;
#define CHECK(test)                                                                                \
  do {                                                                                             \
    if (!(test)) {                                                                                 \
      fprintf(stderr, "line %d: %s\n", __LINE__, #test);                                           \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

int main(void) {
  const ChurchPoint up = {0, 0, 1}, floor = {0, 31, 0};
  ChurchLighting_Begin(false);
  ChurchLighting_End();
  CHECK(ChurchLighting_Ready(false));
  CHECK(!ChurchLighting_Ready(true));
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, floor, up) > .99f);
  const ArRenderColorF white = {1, 1, 1, 1};
  const ArRenderColorF lit = ChurchLighting_Shade(floor, up, white, 1);
  const ArRenderColorF outside = ChurchLighting_Shade((ChurchPoint){16, 31, 0}, up, white, 1);
  CHECK(lit.r > outside.r * 4);
  const ArRenderColorF left = ChurchLighting_Shade((ChurchPoint){-10, 28, 0}, up, white, 1);
  const ArRenderColorF right = ChurchLighting_Shade((ChurchPoint){10, 28, 0}, up, white, 1);
  const ArRenderColorF upper = ChurchLighting_Shade((ChurchPoint){10, 28, 14}, up, white, 1);
  CHECK(left.r > .12f && right.r > .12f); /* Pool reaches both column bases. */
  CHECK(fabsf(left.r - right.r) < .001f);
  CHECK(right.r > upper.r * 3); /* Upper shafts retain the dark room. */
  CHECK(ChurchLighting_WindowPoint(0, 0).x == 0);
  CHECK(ChurchLighting_WindowPoint(1, 1).x > 10);

  const ArRenderColorF actor_lit = ChurchLighting_ActorTint(floor, 0);
  CHECK(actor_lit.r > actor_lit.b && actor_lit.a == 1);
  for (int z = 0; z <= 6; z++) {
    const ArRenderColorF a = ChurchLighting_ActorTint((ChurchPoint){-3.6f, 31, z}, -.2f);
    const ArRenderColorF b = ChurchLighting_ActorTint((ChurchPoint){3.6f, 31, z}, .2f);
    CHECK(isfinite(a.r) && a.r >= .35f && a.r <= 1 && a.g >= .35f && a.g <= 1 && a.b >= .35f &&
          a.b <= 1 && a.a == 1);
    CHECK(fabsf(a.r - b.r) < .01f); /* Symmetric window, gentle off-center door fill. */
  }

  /* A roof crossing the actual window-to-floor ray must shadow the floor,
   * while its upper face and a point beyond its silhouette stay illuminated. */
  ChurchLighting_Begin(false);
  const ChurchPoint roof[4] = {{-4, 19, 6}, {2, 19, 6}, {2, 25, 6}, {-4, 25, 6}};
  ChurchLighting_AddQuad(roof);
  ChurchLighting_End();
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, floor, up) < .01f);
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, (ChurchPoint){-1, 22, 6}, up) > .99f);
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, (ChurchPoint){8, 31, 0}, up) > .99f);
  const ArRenderColorF shadow = ChurchLighting_Shade(floor, up, white, 1);
  CHECK(shadow.r < lit.r * .3f);
  CHECK(shadow.r > .005f); /* Bounced light survives in the shadow. */
  const ArRenderColorF actor_shadow = ChurchLighting_ActorTint(floor, 0);
  CHECK(actor_shadow.r < actor_lit.r - .1f && actor_shadow.a == 1);
  CHECK(actor_shadow.r >= .35f); /* Native painted detail survives environment shadows. */
  bool penumbra = false;
  for (int i = 0; i < 80; i++) {
    const float visibility =
        ChurchLighting_Visibility(kChurchLight_Window, (ChurchPoint){i * .1f, 31, 0}, up);
    CHECK(isfinite(visibility) && visibility >= 0 && visibility <= 1.00001f);
    if (visibility > .05f && visibility < .95f) penumbra = true;
  }
  CHECK(penumbra);

  /* Door shadows are independent of the window map. */
  ChurchLighting_Begin(true);
  const ChurchPoint door_roof[4] = {{-3, 37, 6}, {3, 37, 6}, {3, 44, 6}, {-3, 44, 6}};
  ChurchLighting_AddQuad(door_roof);
  ChurchLighting_End();
  CHECK(ChurchLighting_Visibility(kChurchLight_Door, (ChurchPoint){0, 20, 0}, up) < .01f);
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, floor, up) > .99f);
  const ArRenderColorF cool = ChurchLighting_ActorTint((ChurchPoint){0, 51, 3}, 0);
  CHECK(cool.b > cool.r); /* Outside the warm pool, the doorway supplies blue fill. */
  ChurchLighting_Reset();
  CHECK(!ChurchLighting_Ready(true));
  CHECK(ChurchLighting_Visibility(kChurchLight_Window, floor, up) == 1);
  return s_failures ? 1 : 0;
}
