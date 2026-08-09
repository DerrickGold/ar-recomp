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

enum { kTexWidth = 512, kBudget = 120, kAuthentic = 256, kCapture = 496 };

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

static ActionBgLayerPlan Layer(ActionBgEdgeMode edge) {
  return (ActionBgLayerPlan) {
    .valid = true,
    .source = kActionBgSource_AuthenticViewport,
    .default_edge = edge,
  };
}

static void Span(int ws_extra, int budget, int live_l, int live_r,
                 ActionBgEdgeMode edge, bool pad_captured_to_budget,
                 int *x0, int *x1) {
  ActionBgLayerPlan layer = Layer(edge);
  DioramaBgValidSpanPlan spans;
  DioramaBgValidSpanPlan_Build(
      ws_extra, budget, live_l, live_r, pad_captured_to_budget,
      &layer, 0, 1, kTexWidth, &spans);
  *x0 = spans.count ? spans.spans[0].x0 : 0;
  *x1 = spans.count ? spans.spans[0].x1 : 0;
}

static void TestValidSpan(void) {
  int x0, x1;

  /* Level start, wide BG2 fetched from tilemap: the left margin collapsed, so
   * only 376 of 496 columns hold content. This is the case Fix B exists for. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_RawWrap, false,
       &x0, &x1);
  ExpectInt("start x0", x0, 120);
  ExpectInt("start x1", x1, 496);

  /* Level end: the collapse is on the other side. Asymmetry matters — a
   * symmetric inset would needlessly crop the still-valid side. */
  Span(kBudget, kBudget, kBudget, 0, kActionBgEdge_RawWrap, false,
       &x0, &x1);
  ExpectInt("end x0", x0, 0);
  ExpectInt("end x1", x1, 376);

  /* Post-Fix-A majority: padding reached the budget, so the span is the whole
   * capture regardless of how far the live margin collapsed. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_Mirror, true,
       &x0, &x1);
  ExpectInt("padded x0", x0, 0);
  ExpectInt("padded x1", x1, 496);

  /* Mid-level, no collapse: full span, so the fix is inert. */
  Span(kBudget, kBudget, kBudget, kBudget, kActionBgEdge_RawWrap, false,
       &x0, &x1);
  ExpectInt("mid x0", x0, 0);
  ExpectInt("mid x1", x1, 496);

  /* Clamped BG2 has no margin content at all — centre 256 only. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_Clamp, true,
       &x0, &x1);
  ExpectInt("clamped x0", x0, 120);
  ExpectInt("clamped x1", x1, 376);

  /* A fully bounded screen renders only the authentic 256. */
  Span(kBudget, kBudget, 0, 0, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("bounded x0", x0, 120);
  ExpectInt("bounded x1", x1, 376);

  /* Degenerate: g_ws_extra == 0 (4:3). The span is exactly the authentic 256
   * starting at column 0, i.e. identical to today's behaviour. */
  Span(0, 0, 0, 0, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("no-widescreen x0", x0, 0);
  ExpectInt("no-widescreen x1", x1, 256);

  /* Defensive: margins beyond the budget clamp rather than producing an
   * out-of-range span. */
  Span(kBudget, kBudget, 200, -5, kActionBgEdge_RawWrap, false,
       &x0, &x1);
  ExpectInt("clamped-input x0", x0, 0);
  ExpectInt("clamped-input x1", x1, 376);
}

static void ExpectSpan(const char *label, const DioramaBgValidSpan *span,
                       int y0, int y1, int x0, int x1) {
  char part[96];
  snprintf(part, sizeof(part), "%s y0", label);
  ExpectInt(part, span->y0, y0);
  snprintf(part, sizeof(part), "%s y1", label);
  ExpectInt(part, span->y1, y1);
  snprintf(part, sizeof(part), "%s x0", label);
  ExpectInt(part, span->x0, x0);
  snprintf(part, sizeof(part), "%s x1", label);
  ExpectInt(part, span->x1, x1);
}

static void TestBandedValidSpans(void) {
  DioramaBgValidSpanPlan spans;
  ActionBgLayerPlan layer = Layer(kActionBgEdge_RawWrap);
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true,
                               &layer, 0, 224, kTexWidth, &spans);
  ExpectInt("raw span count", spans.count, 1);
  ExpectSpan("raw", &spans.spans[0], 0, 224, 120, 496);

  layer = Layer(kActionBgEdge_Mirror);
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true,
                               &layer, 0, 224, kTexWidth, &spans);
  ExpectInt("padded mirror span count", spans.count, 1);
  ExpectSpan("padded mirror", &spans.spans[0], 0, 224, 0, 496);
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, false,
                               &layer, 0, 224, kTexWidth, &spans);
  ExpectSpan("unpadded mirror", &spans.spans[0], 0, 224, 120, 496);

  /* Bloodpool's Mirror default and Repeat water band have the same full-budget
   * extent. Exact evaluation can therefore coalesce them without the old,
   * incorrect conservative clamp. */
  layer.bands[0] = (ActionBgBand) {
    .y0 = 136, .y1 = 224, .edge = kActionBgEdge_Repeat,
  };
  layer.band_count = 1;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true,
                               &layer, 16, 240, kTexWidth, &spans);
  ExpectInt("Bloodpool span count", spans.count, 1);
  ExpectSpan("Bloodpool", &spans.spans[0], 0, 240, 0, 496);

  /* Death Heim's upper clamp and lower repeating fog genuinely need distinct
   * UV spans. The authentic y=144 boundary moves down by the 16-row vertical
   * extension in the captured texture. */
  layer = Layer(kActionBgEdge_Clamp);
  layer.bands[0] = (ActionBgBand) {
    .y0 = 144, .y1 = 224, .edge = kActionBgEdge_Repeat,
  };
  layer.band_count = 1;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true,
                               &layer, 16, 240, kTexWidth, &spans);
  ExpectInt("Death Heim span count", spans.count, 2);
  ExpectSpan("Death Heim upper", &spans.spans[0], 0, 160, 120, 376);
  ExpectSpan("Death Heim fog", &spans.spans[1], 160, 240, 0, 496);

  /* A zeroed/invalid frame slot stays safely bounded by its live margins. */
  layer = (ActionBgLayerPlan){ 0 };
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, 0, true,
                               &layer, 0, 224, kTexWidth, &spans);
  ExpectInt("invalid span count", spans.count, 1);
  ExpectSpan("invalid", &spans.spans[0], 0, 224, 120, 376);
}

/* THE no-op guarantee. Wherever the span is the full capture, the UV range must
 * equal the pre-fix expression exactly:
 *     margin_u = (radius + 1) / 512
 *     u0 = margin_u
 *     u1 = snes_width / 512 - margin_u        (snes_width == 496)
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
  DioramaSkyboxUvRange(kTexWidth, 120, kCapture, 1.0f, &u0, &u1);
  ExpectFloat("start u0", u0, (120.0f + 2.0f) / (float)kTexWidth);
  ExpectFloat("start u1", u1, ((float)kCapture - 2.0f) / (float)kTexWidth);

  DioramaSkyboxUvRange(kTexWidth, 120, kCapture, 3.0f, &u0, &u1);
  ExpectFloat("start u0 r3", u0, (120.0f + 4.0f) / (float)kTexWidth);
  ExpectFloat("start u1 r3", u1, ((float)kCapture - 4.0f) / (float)kTexWidth);

  /* The cropped range must be strictly inside the full one — the whole point. */
  float full_u0, full_u1;
  DioramaSkyboxUvRange(kTexWidth, 0, kCapture, 1.0f, &full_u0, &full_u1);
  DioramaSkyboxUvRange(kTexWidth, 120, kCapture, 1.0f, &u0, &u1);
  if (!(u0 > full_u0)) {
    printf("FAIL cropped u0 (%.6f) must exceed full u0 (%.6f)\n", u0, full_u0);
    g_failures++;
  }
}

/* An absurd radius must not invert the range into sampling backwards. */
static void TestUvRangeNeverInverts(void) {
  float u0, u1;
  DioramaSkyboxUvRange(kTexWidth, 120, 376, 200.0f, &u0, &u1);
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
  TestValidSpan();
  TestBandedValidSpans();
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
