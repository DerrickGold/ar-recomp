/* F4: which captured diorama layers are half-added with the subscreen, and so
 * must be captured at 50% alpha instead of opaque.
 *
 * The assertions that matter are the measured Fillmore act 2 frame and the
 * subscreen-identity exclusion. Everything else here is fail-closed coverage:
 * colour math that is NOT a half-add-with-subscreen must keep the old opaque
 * capture rather than be approximated. */
#include <stdio.h>

#include "diorama_capture_blend.h"

static int s_failures;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

/* Bit order is shared by CGADSUB, TM/TS and kPpuOverlaySource_*. */
enum {
  kBg1 = 1 << 0,
  kBg2 = 1 << 1,
  kBg3 = 1 << 2,
  kObj = 1 << 4,
};

/* The state measured across all 4952 frames of Fillmore act 2. */
enum {
  kActCgwsel = 0x02,
  kActCgadsub = 0x43, /* half + add, math on BG1|BG2 */
  kActSub = 0x11,     /* BG1 + OBJ on the subscreen */
};

static void TestMeasuredFillmoreAct2Frame(void) {
  CHECK(DioramaCaptureBlend_IsHalfAddWithSubscreen(kActCgwsel, kActCgadsub));

  /* BG2 is the water: colour math on, and NOT on the subscreen, so it really is
   * blended over what is behind it. This is the pixel the report was about. */
  CHECK(DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, kActSub,
                                             kBg2));

  /* BG1 has its CGADSUB bit set too, but it is ALSO on the subscreen, so the
   * hardware is adding it to itself and the result is unchanged. Marking it
   * would make an opaque layer half-transparent -- the exact wrong outcome, and
   * the one a naive "CGADSUB bit is set" test would produce. */
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, kActSub,
                                              kBg1));

  /* BG3 (the HUD) has no colour-math bit at all. */
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, kActSub,
                                              kBg3));
}

static void TestFailsClosedOnUnreproducibleMath(void) {
  /* Fixed-colour addend: there is no "behind" to blend with, so alpha cannot
   * express it. */
  CHECK(!DioramaCaptureBlend_IsHalfAddWithSubscreen(0x00, kActCgadsub));
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(0x00, kActCgadsub, kActSub, kBg2));

  /* A full add is not an alpha blend (it brightens; alpha interpolates). */
  CHECK(!DioramaCaptureBlend_IsHalfAddWithSubscreen(kActCgwsel, 0x03));
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, 0x03, kActSub, kBg2));

  /* Subtract, even at half, is not a source-over. */
  CHECK(!DioramaCaptureBlend_IsHalfAddWithSubscreen(kActCgwsel, 0xC3));
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, 0xC3, kActSub, kBg2));

  /* No layer selected. */
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, 0x40, kActSub, kBg2));
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, kActSub,
                                              0));
}

static void TestSubscreenIdentityIsPerLayer(void) {
  /* With nothing on the subscreen, every math-enabled layer blends. */
  CHECK(DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, 0x00,
                                             kBg1));
  CHECK(DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, 0x00,
                                             kBg2));
  /* With BOTH on the subscreen, neither does. */
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, 0x03,
                                              kBg1));
  CHECK(!DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, 0x03,
                                              kBg2));
  /* The exclusion is keyed on the layer's OWN bit, not on the subscreen being
   * non-empty: OBJ sitting on the subscreen must not exempt BG2. */
  CHECK(DioramaCaptureBlend_LayerIsHalfAdded(kActCgwsel, kActCgadsub, kObj,
                                            kBg2));
}

int main(void) {
  TestMeasuredFillmoreAct2Frame();
  TestFailsClosedOnUnreproducibleMath();
  TestSubscreenIdentityIsPerLayer();
  printf("diorama capture blend tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
