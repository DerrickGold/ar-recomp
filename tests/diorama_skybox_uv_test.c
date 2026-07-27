/* Fix B (SPEC-backdrop-clip.md): BG2 valid-span classification and the skybox
 * quad's UV range.
 *
 * The load-bearing assertion in here is the NO-OP one: wherever Fix A padded
 * BG2's margins out to the budget, the new UV math must produce values
 * bit-identical to the pre-fix expression. If that ever stops holding, the fix
 * has silently started cropping frames it was never meant to touch.
 */
#include "diorama_skybox_uv.h"

#include <math.h>
#include <stdio.h>

enum { kTexWidth = 448, kBudget = 95, kAuthentic = 256, kCapture = 446 };

static int g_failures;

static void ExpectInt(const char *label, int got, int want) {
  if (got != want) {
    printf("FAIL %s: got %d, want %d\n", label, got, want);
    g_failures++;
  }
}

static void ExpectFloat(const char *label, float got, float want) {
  /* Exact-equality intent: these are the same arithmetic, so anything beyond
   * float rounding is a real divergence. */
  if (fabsf(got - want) > 1e-7f) {
    printf("FAIL %s: got %.9f, want %.9f\n", label, got, want);
    g_failures++;
  }
}

static void Span(int ws_extra, int budget, int live_l, int live_r, int source,
                 int *x0, int *x1) {
  DioramaBg2ValidSpan(ws_extra, budget, live_l, live_r, source, kTexWidth,
                      x0, x1);
}

/* The classification drives everything else, so pin each policy combination. */
static void TestClassify(void) {
  const uint8_t bg1 = 1u << 0, bg2 = 1u << 1;
  ExpectInt("mirror -> Padded",
            DioramaBg2MarginSource_Classify(0, bg2, 0, false),
            kBg2Margin_Padded);
  ExpectInt("repeat -> Padded",
            DioramaBg2MarginSource_Classify(0, 0, bg2, false),
            kBg2Margin_Padded);
  ExpectInt("clamp -> Clamped",
            DioramaBg2MarginSource_Classify(bg2, 0, 0, false),
            kBg2Margin_Clamped);
  /* A clamp bit wins outright: PpuLayerExtra returns 0 before consulting
   * anything else, so no margin is rendered whatever the other bits say. */
  ExpectInt("clamp beats mirror",
            DioramaBg2MarginSource_Classify(bg2, bg2, 0, false),
            kBg2Margin_Clamped);
  /* A repeat BAND varies per scanline; one span cannot express that, so the
   * conservative answer is required. */
  ExpectInt("repeat band -> Clamped (conservative)",
            DioramaBg2MarginSource_Classify(0, 0, 0, true),
            kBg2Margin_Clamped);
  ExpectInt("no policy -> Live",
            DioramaBg2MarginSource_Classify(0, 0, 0, false),
            kBg2Margin_Live);
  /* BG1's policy must not leak into BG2's classification. */
  ExpectInt("bg1 clamp ignored",
            DioramaBg2MarginSource_Classify(bg1, 0, 0, false),
            kBg2Margin_Live);
  ExpectInt("bg1 mirror ignored",
            DioramaBg2MarginSource_Classify(0, bg1, 0, false),
            kBg2Margin_Live);
}

static void TestValidSpan(void) {
  int x0, x1;

  /* Level start, wide BG2 fetched from tilemap: the left margin collapsed, so
   * only 351 of 446 columns hold content. This is the case Fix B exists for. */
  Span(kBudget, kBudget, 0, kBudget, kBg2Margin_Live, &x0, &x1);
  ExpectInt("start x0", x0, 95);
  ExpectInt("start x1", x1, 446);

  /* Level end: the collapse is on the other side. Asymmetry matters — a
   * symmetric inset would needlessly crop the still-valid side. */
  Span(kBudget, kBudget, kBudget, 0, kBg2Margin_Live, &x0, &x1);
  ExpectInt("end x0", x0, 0);
  ExpectInt("end x1", x1, 351);

  /* Post-Fix-A majority: padding reached the budget, so the span is the whole
   * capture regardless of how far the live margin collapsed. */
  Span(kBudget, kBudget, 0, kBudget, kBg2Margin_Padded, &x0, &x1);
  ExpectInt("padded x0", x0, 0);
  ExpectInt("padded x1", x1, 446);

  /* Mid-level, no collapse: full span, so the fix is inert. */
  Span(kBudget, kBudget, kBudget, kBudget, kBg2Margin_Live, &x0, &x1);
  ExpectInt("mid x0", x0, 0);
  ExpectInt("mid x1", x1, 446);

  /* Clamped BG2 has no margin content at all — centre 256 only. */
  Span(kBudget, kBudget, 0, kBudget, kBg2Margin_Clamped, &x0, &x1);
  ExpectInt("clamped x0", x0, 95);
  ExpectInt("clamped x1", x1, 351);

  /* A fully bounded screen renders only the authentic 256. */
  Span(kBudget, kBudget, 0, 0, kBg2Margin_Live, &x0, &x1);
  ExpectInt("bounded x0", x0, 95);
  ExpectInt("bounded x1", x1, 351);

  /* Degenerate: g_ws_extra == 0 (4:3). The span is exactly the authentic 256
   * starting at column 0, i.e. identical to today's behaviour. */
  Span(0, 0, 0, 0, kBg2Margin_Live, &x0, &x1);
  ExpectInt("no-widescreen x0", x0, 0);
  ExpectInt("no-widescreen x1", x1, 256);

  /* Defensive: margins beyond the budget clamp rather than producing an
   * out-of-range span. */
  Span(kBudget, kBudget, 200, -5, kBg2Margin_Live, &x0, &x1);
  ExpectInt("clamped-input x0", x0, 0);
  ExpectInt("clamped-input x1", x1, 351);
}

/* THE no-op guarantee. Wherever the span is the full capture, the UV range must
 * equal the pre-fix expression exactly:
 *     margin_u = (radius + 1) / 448
 *     u0 = margin_u
 *     u1 = snes_width / 448 - margin_u        (snes_width == 446)
 */
static void TestUvRangeMatchesLegacyOnFullSpan(void) {
  const float radii[] = { 1.0f, 3.0f };
  for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); i++) {
    const float radius = radii[i];
    const float margin_u = (radius + 1.0f) / (float)kTexWidth;
    const float legacy_u0 = margin_u;
    const float legacy_u1 = (float)kCapture / (float)kTexWidth - margin_u;
    float u0 = -1.0f, u1 = -1.0f;
    DioramaSkyboxUvRange(kTexWidth, 0, kCapture, radius, &u0, &u1);
    ExpectFloat("legacy u0", u0, legacy_u0);
    ExpectFloat("legacy u1", u1, legacy_u1);
  }
}

static void TestUvRangeCropsNarrowedSpan(void) {
  float u0, u1;
  /* Level start with radius 1: the blur inset must still apply at the NEW
   * boundary, or the kernel pulls the black columns back across it. */
  DioramaSkyboxUvRange(kTexWidth, 95, kCapture, 1.0f, &u0, &u1);
  ExpectFloat("start u0", u0, (95.0f + 2.0f) / (float)kTexWidth);
  ExpectFloat("start u1", u1, ((float)kCapture - 2.0f) / (float)kTexWidth);

  DioramaSkyboxUvRange(kTexWidth, 95, kCapture, 3.0f, &u0, &u1);
  ExpectFloat("start u0 r3", u0, (95.0f + 4.0f) / (float)kTexWidth);
  ExpectFloat("start u1 r3", u1, ((float)kCapture - 4.0f) / (float)kTexWidth);

  /* The cropped range must be strictly inside the full one — the whole point. */
  float full_u0, full_u1;
  DioramaSkyboxUvRange(kTexWidth, 0, kCapture, 1.0f, &full_u0, &full_u1);
  DioramaSkyboxUvRange(kTexWidth, 95, kCapture, 1.0f, &u0, &u1);
  if (!(u0 > full_u0)) {
    printf("FAIL cropped u0 (%.6f) must exceed full u0 (%.6f)\n", u0, full_u0);
    g_failures++;
  }
}

/* An absurd radius must not invert the range into sampling backwards. */
static void TestUvRangeNeverInverts(void) {
  float u0, u1;
  DioramaSkyboxUvRange(kTexWidth, 95, 351, 200.0f, &u0, &u1);
  if (u1 < u0) {
    printf("FAIL inverted range: u0=%.6f u1=%.6f\n", u0, u1);
    g_failures++;
  }
  DioramaSkyboxUvRange(0, 0, 0, 1.0f, &u0, &u1);
  ExpectFloat("zero width u0", u0, 0.0f);
  ExpectFloat("zero width u1", u1, 0.0f);
  /* A negative radius must be treated as zero, not widen the range. */
  DioramaSkyboxUvRange(kTexWidth, 0, kCapture, -5.0f, &u0, &u1);
  ExpectFloat("negative radius u0", u0, 1.0f / (float)kTexWidth);
}

int main(void) {
  TestClassify();
  TestValidSpan();
  TestUvRangeMatchesLegacyOnFullSpan();
  TestUvRangeCropsNarrowedSpan();
  TestUvRangeNeverInverts();
  if (g_failures) {
    printf("diorama_skybox_uv_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("diorama_skybox_uv_test: all checks passed\n");
  return 0;
}
