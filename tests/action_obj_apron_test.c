/* The apron channel's geometry and admission rules.
 *
 * The load-bearing property is NEGATIVE and is why this test exists at all: the
 * two apron spans must never map into the display window. Everything about the
 * phase's byte-identity gate rests on that -- a span off by one column would let
 * capture-time rasterization overwrite a pixel scanout owns, silently, in a way
 * only a full A/B render would catch. Here it is one assertion. */

#include <stdio.h>
#include <string.h>

#include "action_obj_apron.h"

static int failures;
#define CHECK(e) do { if(!(e)){ \
  fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e); failures++; } } while(0)

enum { kAuthentic = 256 };

int main(void) {
  /* The live diorama geometry: kWsExtraMax margin, kPpuObjApron headroom. */
  const ActionApronGeometry g = { 120, 64 };
  const int surface = ActionApron_SurfaceWidth(&g);
  CHECK(surface == kAuthentic + 2 * 120 + 2 * 64);  /* 624 */
  CHECK(surface <= kPpuSurfaceWidth);               /* fits the allocation */

  /* Screen x = 0 sits at apron + ws_extra; the display window is the middle. */
  CHECK(ActionApron_SurfaceColumn(&g, 0) == 64 + 120);
  CHECK(ActionApron_SurfaceColumn(&g, -120) == 64);
  CHECK(ActionApron_SurfaceColumn(&g, kAuthentic + 120 - 1) ==
        surface - 64 - 1);

  int l0, l1, r0, r1;
  ActionApron_LeftSpan(&g, &l0, &l1);
  ActionApron_RightSpan(&g, &r0, &r1);
  CHECK(l0 == -(120 + 64) && l1 == -120);
  CHECK(r0 == kAuthentic + 120 && r1 == kAuthentic + 120 + 64);
  CHECK(l1 - l0 == 64 && r1 - r0 == 64);

  /* THE invariant: every apron column is outside [apron, apron+display). */
  {
    const int display_lo = 64;
    const int display_hi = 64 + kAuthentic + 2 * 120;
    for (int x = l0; x < l1; x++) {
      const int c = ActionApron_SurfaceColumn(&g, x);
      CHECK(c >= 0 && c < display_lo);
    }
    for (int x = r0; x < r1; x++) {
      const int c = ActionApron_SurfaceColumn(&g, x);
      CHECK(c >= display_hi && c < surface);
    }
    /* And they ABUT the window -- a gap would leave dead columns nothing ever
     * writes, which reads as a seam rather than as a clean edge. */
    CHECK(ActionApron_SurfaceColumn(&g, l1) == display_lo);
    CHECK(ActionApron_SurfaceColumn(&g, r0) == display_hi);
  }

  /* Admission. A part is only worth recording when it actually reaches an
   * apron band: the emitter's reject branch fires for parts that missed the
   * OAM window for any reason, including ones far outside it. */
  CHECK(!ActionApron_PartTouchesApron(&g, 0, 16));        /* mid-screen */
  CHECK(!ActionApron_PartTouchesApron(&g, -120, 16));     /* flush left edge */
  CHECK(ActionApron_PartTouchesApron(&g, -121, 16));      /* one col into left */
  CHECK(ActionApron_PartTouchesApron(&g, kAuthentic + 115, 16)); /* straddles */
  CHECK(ActionApron_PartTouchesApron(&g, r1 - 1, 16));    /* last apron col */
  CHECK(!ActionApron_PartTouchesApron(&g, r1, 16));       /* just past it */
  CHECK(!ActionApron_PartTouchesApron(&g, l0 - 16, 16));  /* just before it */
  CHECK(!ActionApron_PartTouchesApron(&g, 0, 0));         /* degenerate size */

  /* Apron 0 is the phase's disable lever: no span, so nothing is ever
   * admitted and every consumer collapses to its pre-apron form. */
  {
    const ActionApronGeometry off = { 120, 0 };
    int a0, a1, b0, b1;
    ActionApron_LeftSpan(&off, &a0, &a1);
    ActionApron_RightSpan(&off, &b0, &b1);
    CHECK(a1 == a0 && b1 == b0);
    CHECK(ActionApron_SurfaceWidth(&off) == kAuthentic + 2 * 120);
    CHECK(ActionApron_SurfaceColumn(&off, 0) == 120);
    CHECK(!ActionApron_PartTouchesApron(&off, kAuthentic + 115, 16));
    CHECK(!ActionApron_AddPart(&off, kAuthentic + 115, 0, 0, 16));
  }

  /* Colour math follows CGADSUB's OBJ rule: palettes 4-7 only. Bits 9-11 of
   * the attribute word, the same three PpuRasterizeParts derives its palette
   * base from. */
  for (int pal = 0; pal < 8; pal++) {
    const uint16_t attr = (uint16_t)(pal << 9);
    CHECK(ActionApron_PartUsesColorMath(attr) == (pal >= 4));
  }

  /* Channel: stores exactly what it was handed, rejects what misses. */
  ActionApron_BeginFrame();
  CHECK(ActionApron_Count() == 0);
  CHECK(ActionApron_AddPart(&g, kAuthentic + 115, 12, 0x2345, 16));
  CHECK(ActionApron_Count() == 1);
  {
    const PpuObjPart *p = ActionApron_Parts();
    CHECK(p[0].x == kAuthentic + 115 && p[0].y == 12);
    CHECK(p[0].tile_attr == 0x2345 && p[0].size == 16);
  }
  CHECK(!ActionApron_AddPart(&g, 0, 0, 0x2345, 16));   /* mid-screen: refused */
  CHECK(ActionApron_Count() == 1);                     /* and not recorded */

  /* BeginFrame drops last frame's parts; a frame that emits none must not
   * inherit them (the pass runs off Count(), so a stale list would redraw
   * last frame's sprites into this frame's apron). */
  ActionApron_BeginFrame();
  CHECK(ActionApron_Count() == 0);

  /* Overflow is COUNTED, never silent -- the capacity is a sizing question to
   * be answered by measurement, so it has to be observable. */
  {
    const int before = ActionApron_Overflow();
    for (int i = 0; i < kActionApronMaxParts + 8; i++)
      ActionApron_AddPart(&g, kAuthentic + 115, i & 63, 0x2345, 16);
    CHECK(ActionApron_Count() == kActionApronMaxParts);
    CHECK(ActionApron_Overflow() == before + 8);
    CHECK(ActionApron_PeakCount() >= kActionApronMaxParts);
  }

  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("action_obj_apron: OK\n");
  return 0;
}
