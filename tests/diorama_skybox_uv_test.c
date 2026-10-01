/* BG2 valid-span classification and the skybox
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

enum {
  kTexWidth = 512,
  kTexHeight = 352,
  kBudget = 120,
  kAuthentic = 256,
  kCapture = 496,
  kVerticalCapture = 288,
};

static int s_failures;

static void ExpectInt(const char *label, int got, int want) {
  if (got != want) {
    printf("FAIL %s: got %d, want %d\n", label, got, want);
    s_failures++;
  }
}

static void ExpectFloat(const char *label, float got, float want) {
  /* Exact-equality intent: these are the same arithmetic, so anything beyond
   * float rounding is a real divergence. */
  if (fabsf(got - want) > 1e-7f) {
    printf("FAIL %s: got %.9f, want %.9f\n", label, got, want);
    s_failures++;
  }
}

static ActionBgLayerPlan Layer(ActionBgEdgeMode edge) {
  return (ActionBgLayerPlan){
      .valid = true,
      .source = kActionBgSource_AuthenticViewport,
      .default_edge = edge,
      .camera_y = 64,
      .world_width = 256,
      .world_height = 512,
      .horizontal_extent = {.mode = kActionBgExtent_Available},
      .vertical_extent = {.mode = kActionBgExtent_Available},
  };
}

static void Span(int ws_extra, int budget, int live_l, int live_r, ActionBgEdgeMode edge,
                 bool pad_captured_to_budget, int *x0, int *x1) {
  ActionBgLayerPlan layer = Layer(edge);
  DioramaBgValidSpanPlan spans;
  DioramaBgValidSpanPlan_Build(ws_extra, budget, live_l, live_r,
      pad_captured_to_budget, &layer, NULL, 0, 1, kTexWidth, &spans);
  *x0 = spans.count ? spans.spans[0].x0 : 0;
  *x1 = spans.count ? spans.spans[0].x1 : 0;
}

static void TestValidSpan(void) {
  int x0, x1;

  /* Level start, wide BG2 fetched from tilemap: the left margin collapsed, so
   * only 376 of 496 columns hold content. This is the case Fix B exists for. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("start x0", x0, 120);
  ExpectInt("start x1", x1, 496);

  /* Level end: the collapse is on the other side. Asymmetry matters — a
   * symmetric inset would needlessly crop the still-valid side. */
  Span(kBudget, kBudget, kBudget, 0, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("end x0", x0, 0);
  ExpectInt("end x1", x1, 376);

  /* Post-Fix-A majority: padding reached the budget, so the span is the whole
   * capture regardless of how far the live margin collapsed. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_Mirror, true, &x0, &x1);
  ExpectInt("padded x0", x0, 0);
  ExpectInt("padded x1", x1, 496);

  /* Mid-level, no collapse: full span, so the fix is inert. */
  Span(kBudget, kBudget, kBudget, kBudget, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("mid x0", x0, 0);
  ExpectInt("mid x1", x1, 496);

  /* Clamped BG2 has no margin content at all — centre 256 only. */
  Span(kBudget, kBudget, 0, kBudget, kActionBgEdge_Clamp, true, &x0, &x1);
  ExpectInt("clamped x0", x0, 120);
  ExpectInt("clamped x1", x1, 376);

  /* A fully bounded screen renders only the authentic 256. */
  Span(kBudget, kBudget, 0, 0, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("bounded x0", x0, 120);
  ExpectInt("bounded x1", x1, 376);

  /* Degenerate: g_ws_extra == 0 (4:3). The span remains the authentic 256
   * columns starting at column 0. */
  Span(0, 0, 0, 0, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("no-widescreen x0", x0, 0);
  ExpectInt("no-widescreen x1", x1, 256);

  /* Defensive: margins beyond the budget clamp rather than producing an
   * out-of-range span. */
  Span(kBudget, kBudget, 200, -5, kActionBgEdge_RawWrap, false, &x0, &x1);
  ExpectInt("clamped-input x0", x0, 0);
  ExpectInt("clamped-input x1", x1, 376);
}

static void ExpectSpan(const char *label, const DioramaBgValidSpan *span, int y0, int y1, int x0,
                       int x1) {
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
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 0, 224, kTexWidth,
                               &spans);
  ExpectInt("raw span count", spans.count, 1);
  ExpectSpan("raw", &spans.spans[0], 0, 224, 120, 496);

  layer = Layer(kActionBgEdge_Mirror);
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 0, 224, kTexWidth,
                               &spans);
  ExpectInt("padded mirror span count", spans.count, 1);
  ExpectSpan("padded mirror", &spans.spans[0], 0, 224, 0, 496);
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, false, &layer, NULL, 0, 224, kTexWidth,
                               &spans);
  ExpectSpan("unpadded mirror", &spans.spans[0], 0, 224, 120, 496);

  /* Bloodpool 0201's unique upper moon/cloud family uses its tuned asymmetric
   * cap while the repeat-safe water remains available across the full capture. */
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
      .left = 76,
      .right = 100,
  };
  layer.bands[0] = (ActionBgBand){
      .y0 = 136,
      .y1 = 224,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {.mode = kActionBgExtent_Available},
  };
  layer.band_count = 1;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 16, 256, kTexWidth,
                               &spans);
  ExpectInt("Bloodpool span count", spans.count, 2);
  ExpectSpan("Bloodpool sky", &spans.spans[0], 0, 152, 44, 476);
  /* The lower 16 rows are synthetic, but the water family reaches the
   * authentic y=224 boundary and therefore owns that adjacent margin too. */
  ExpectSpan("Bloodpool water", &spans.spans[1], 152, 256, 0, 496);

  /* Bloodpool 0202 repeats its water within the same inherited 68/68 span as
   * the upper backdrop, so the presenter may coalesce both row families. */
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
      .left = 68,
      .right = 68,
  };
  layer.bands[0].horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Inherit,
  };
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 16, 256, kTexWidth,
                               &spans);
  ExpectInt("Bloodpool act 2 span count", spans.count, 1);
  ExpectSpan("Bloodpool act 2", &spans.spans[0], 0, 256, 52, 444);

  /* Death Heim's upper clamp and lower repeating fog genuinely need distinct
   * UV spans. The authentic y=144 boundary moves down by the 16-row vertical
   * extension in the captured texture. */
  layer = Layer(kActionBgEdge_Clamp);
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
  };
  layer.bands[0] = (ActionBgBand){
      .y0 = 144,
      .y1 = 224,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {.mode = kActionBgExtent_Available},
  };
  layer.band_count = 1;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 16, 256, kTexWidth,
                               &spans);
  ExpectInt("Death Heim span count", spans.count, 2);
  ExpectSpan("Death Heim upper", &spans.spans[0], 0, 160, 120, 376);
  ExpectSpan("Death Heim fog", &spans.spans[1], 160, 256, 0, 496);

  /* A zeroed/invalid frame slot stays safely bounded by its live margins. */
  layer = (ActionBgLayerPlan){0};
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, 0, true,
      &layer, NULL, 0, 224, kTexWidth, &spans);
  ExpectInt("invalid span count", spans.count, 1);
  ExpectSpan("invalid", &spans.spans[0], 0, 224, 120, 376);
}

static void TestExtentValidSpans(void) {
  DioramaBgValidSpanPlan spans;
  ActionBgLayerPlan layer = Layer(kActionBgEdge_Mirror);
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
      .left = 48,
      .right = 64,
  };
  layer.bands[0] = (ActionBgBand){
      .y0 = 136,
      .y1 = 224,
      .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {.mode = kActionBgExtent_Available},
  };
  layer.band_count = 1;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, true, &layer, NULL, 16, 240, kTexWidth,
                               &spans);
  ExpectInt("fixed/band span count", spans.count, 2);
  ExpectSpan("fixed upper", &spans.spans[0], 0, 152, 72, 440);
  ExpectSpan("available band", &spans.spans[1], 152, 240, 0, 496);

  /* A cap cannot manufacture pixels that the live-world edge did not render. */
  layer = Layer(kActionBgEdge_RawWrap);
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
      .left = 100,
      .right = 64,
  };
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, 0, kBudget, false, &layer, NULL, 0, 1, kTexWidth,
                               &spans);
  ExpectInt("source-limited span count", spans.count, 1);
  ExpectSpan("source-limited", &spans.spans[0], 0, 1, 120, 440);

  /* Fixed vertical extents become transparent capture-row intervals. A
   * 12-row top and 2-row bottom retain authentic rows between them. */
  layer = Layer(kActionBgEdge_RawWrap);
  layer.vertical_extent = (ActionBgVerticalExtent){
      .mode = kActionBgExtent_Fixed,
      .top = 12,
      .bottom = 2,
  };
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, kBudget, kBudget, false, &layer, NULL, 16, 244,
                               kTexWidth, &spans);
  ExpectInt("vertical span count", spans.count, 3);
  ExpectSpan("vertical top clipped", &spans.spans[0], 0, 4, 0, 0);
  ExpectSpan("vertical visible", &spans.spans[1], 4, 242, 0, 496);
  ExpectSpan("vertical bottom clipped", &spans.spans[2], 242, 244, 0, 0);
  /* Aitos `$04/$02` captures 32 rows per side but deliberately admits only 24
   * BG2 rows. Keep the transparent top/bottom intervals explicit even though
   * presentation-only overflow no longer derives its existence from them. */
  layer.vertical_extent = (ActionBgVerticalExtent){
      .mode = kActionBgExtent_Fixed,
      .top = 24,
      .bottom = 24,
  };
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, kBudget, kBudget, false, &layer, NULL, 32, 288,
                               kTexWidth, &spans);
  ExpectInt("Aitos vertical span count", spans.count, 3);
  ExpectSpan("Aitos top clipped", &spans.spans[0], 0, 8, 0, 0);
  ExpectSpan("Aitos waterfall rows", &spans.spans[1], 8, 280, 0, 496);
  ExpectSpan("Aitos bottom clipped", &spans.spans[2], 280, 288, 0, 0);
  int drawable_y0 = -1, drawable_y1 = -1;
  ExpectInt("Aitos has drawable rows",
            DioramaBgValidSpanPlan_DrawableRowBounds(&spans, &drawable_y0, &drawable_y1), true);
  ExpectInt("Aitos drawable row start", drawable_y0, 8);
  ExpectInt("Aitos drawable row end", drawable_y1, 280);

  DioramaBgValidSpanPlan clipped = {
      .count = 1,
      .spans = {{.y0 = 0, .y1 = 8, .x0 = 0, .x1 = 0}},
  };
  drawable_y0 = drawable_y1 = -1;
  ExpectInt("fully clipped has no drawable rows",
            DioramaBgValidSpanPlan_DrawableRowBounds(&clipped, &drawable_y0, &drawable_y1), false);
  ExpectInt("fully clipped drawable start", drawable_y0, 0);
  ExpectInt("fully clipped drawable end", drawable_y1, 0);

  /* Exercise the fixed-capacity proof: four isolated overrides create nine
   * horizontal runs, plus one clipped run above and below. */
  layer = Layer(kActionBgEdge_Mirror);
  layer.horizontal_extent = (ActionBgHorizontalExtent){
      .mode = kActionBgExtent_Fixed,
      .left = 32,
      .right = 32,
  };
  layer.vertical_extent = (ActionBgVerticalExtent){
      .mode = kActionBgExtent_Fixed,
  };
  for (int i = 0; i < kActionBgMaxBands; i++) {
    layer.bands[i] = (ActionBgBand){
        .y0 = (uint16_t)(10 + i * 20),
        .y1 = (uint16_t)(20 + i * 20),
        .edge = kActionBgEdge_Repeat,
        .horizontal_extent = {.mode = kActionBgExtent_Available},
    };
  }
  layer.band_count = kActionBgMaxBands;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, kBudget, kBudget, true,
                               &layer, NULL, 1, 226, kTexWidth,
                               &spans);
  ExpectInt("maximum span count", spans.count, kDioramaBgMaxValidSpans);
  ExpectSpan("maximum top", &spans.spans[0], 0, 1, 0, 0);
  ExpectSpan("maximum bottom", &spans.spans[kDioramaBgMaxValidSpans - 1], 225, 226, 0, 0);
}

/* THE no-op guarantee. Wherever the span is the full capture, the UV range must
 * equal the pre-fix expression exactly:
 *     margin_u = (radius + 1) / 512
 *     u0 = margin_u
 *     u1 = snes_width / 512 - margin_u        (snes_width == 496)
 */
static void TestUvRangeMatchesLegacyOnFullSpan(void) {
  const float radii[] = {1.0f, 3.0f};
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

static void TestLiveVerticalWorldMapping(void) {
  DioramaBgValidSpanPlan spans;
  ActionBgLayerPlan layer = Layer(kActionBgEdge_Repeat);
  /* Northwall 0601 at the reported frame: BG1 owns a 32-row top/bottom
   * capture, but BG2 is at world Y=0 in a 256-row world. Its drawable capture
   * is therefore [32,287): 224 native rows plus 31 real lower rows. */
  layer.camera_y = 0;
  layer.world_height = 256;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, kBudget, kBudget, false, &layer, NULL, 32,
                               kVerticalCapture, kTexWidth, &spans);
  ExpectInt("Northwall vertical span count", spans.count, 3);
  ExpectSpan("Northwall unavailable top", &spans.spans[0], 0, 32, 0, 0);
  ExpectSpan("Northwall drawable BG2", &spans.spans[1], 32, 287, 0, kCapture);
  ExpectSpan("Northwall unavailable bottom", &spans.spans[2], 287, 288, 0, 0);

  DioramaSkyboxVerticalMapping mapping;
  ExpectInt(
      "Northwall vertical mapping",
      DioramaSkyboxVerticalMapping_Build(&spans, kVerticalCapture, kTexHeight, 3.0f, &mapping),
      true);
  ExpectInt("Northwall source start", mapping.capture_y0, 32);
  ExpectInt("Northwall source end", mapping.capture_y1, 287);
  ExpectFloat("Northwall blur-safe v0", mapping.texture_v0, 36.0f / (float)kTexHeight);
  ExpectFloat("Northwall blur-safe v1", mapping.texture_v1, 283.0f / (float)kTexHeight);
  ExpectFloat("Northwall output top", DioramaSkyboxVerticalMapping_Fraction(&mapping, 32), 0.0f);
  ExpectFloat("Northwall output bottom", DioramaSkyboxVerticalMapping_Fraction(&mapping, 287),
              1.0f);

  /* A BG2 with enough real rows on both sides retains the exact legacy map:
   * no blur inset and capture midpoint remains output midpoint. */
  layer.camera_y = 64;
  layer.world_height = 512;
  DioramaBgValidSpanPlan_Build(kBudget, kBudget, kBudget, kBudget, false, &layer, NULL, 32,
                               kVerticalCapture, kTexWidth, &spans);
  ExpectInt("full vertical span count", spans.count, 1);
  ExpectInt(
      "full vertical mapping",
      DioramaSkyboxVerticalMapping_Build(&spans, kVerticalCapture, kTexHeight, 3.0f, &mapping),
      true);
  ExpectInt("full source start", mapping.capture_y0, 0);
  ExpectInt("full source end", mapping.capture_y1, kVerticalCapture);
  ExpectFloat("full legacy v0", mapping.texture_v0, 0.0f);
  ExpectFloat("full legacy v1", mapping.texture_v1, (float)kVerticalCapture / (float)kTexHeight);
  ExpectFloat("full output midpoint", DioramaSkyboxVerticalMapping_Fraction(&mapping, 144), 0.5f);
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
    s_failures++;
  }
}

/* An absurd radius must not invert the range into sampling backwards. */
static void TestUvRangeNeverInverts(void) {
  float u0, u1;
  DioramaSkyboxUvRange(kTexWidth, 120, 376, 200.0f, &u0, &u1);
  if (u1 < u0) {
    printf("FAIL inverted range: u0=%.6f u1=%.6f\n", u0, u1);
    s_failures++;
  }
  DioramaSkyboxUvRange(0, 0, 0, 1.0f, &u0, &u1);
  ExpectFloat("zero width u0", u0, 0.0f);
  ExpectFloat("zero width u1", u1, 0.0f);
  /* A negative radius must be treated as zero, not widen the range. */
  DioramaSkyboxUvRange(kTexWidth, 0, kCapture, -5.0f, &u0, &u1);
  ExpectFloat("negative radius u0", u0, 1.0f / (float)kTexWidth);
}

static void TestRomUvRepeatsAcrossDisplayedWidth(void) {
  float u0 = -1.0f, u1 = -1.0f;
  DioramaRomSkyboxUvRange(256, 256, &u0, &u1);
  ExpectFloat("ROM 4:3 u0", u0, 0.0f);
  ExpectFloat("ROM 4:3 u1", u1, 1.0f);

  DioramaRomSkyboxUvRange(kCapture, 256, &u0, &u1);
  ExpectFloat("ROM widescreen u0", u0, 0.0f);
  ExpectFloat("ROM widescreen u1", u1, (float)kCapture / 256.0f);

  DioramaRomSkyboxUvRange(kCapture, 0, &u0, &u1);
  ExpectFloat("ROM invalid u0", u0, 0.0f);
  ExpectFloat("ROM invalid u1", u1, 0.0f);
}

static void TestSkyboxFollowsNativeWindow(void) {
  /* In the reported 03/01 descent, the texture's top margin grows even
   * though the BG2 camera has only 1/3-rate motion. Once the virtual camera
   * stops, a fixed statue must stay put through both kinds of row changes. */
  for (int y = 504; y <= 543; y++) {
    const int top = y - 415;
    const int bg_camera = y / 3;
    DioramaSkyboxVerticalMapping mapping = {0, 352, 0, 1};
    DioramaSkyboxVerticalMapping_FollowCamera(
        &mapping, 352, top, 170.0f - bg_camera);
    ExpectFloat("skybox window retains native scale",
                mapping.capture_y1 - mapping.capture_y0, 224);
    ExpectFloat("skybox statue stops with virtual camera",
                DioramaSkyboxVerticalMapping_Fraction(
                    &mapping, 299 - bg_camera + top), 129.0f / 224);
  }
  DioramaSkyboxVerticalMapping mapping = {4, 348, 4.0f / 352, 348.0f / 352};
  DioramaSkyboxVerticalMapping_FollowCamera(&mapping, 352, 0, -20);
  ExpectFloat("skybox respects its own top", mapping.capture_y0, 4);
  mapping = (DioramaSkyboxVerticalMapping){4, 348, 4.0f / 352, 348.0f / 352};
  DioramaSkyboxVerticalMapping_FollowCamera(&mapping, 352, 128, 20);
  ExpectFloat("skybox floor keeps a full window", mapping.capture_y0, 124);
  ExpectFloat("skybox floor never samples outside", mapping.capture_y1, 348);
}

static void TestSkyboxAspectFit(void) {
  /* A wide capture includes more scenery than the displayed native window.
   * Fitting that whole width independently to 224 rows made the moon tall. */
  const float widths[] = {252, 356, 492, 620};
  const float aspects[] = {4.0f / 3, 16.0f / 10, 16.0f / 9, 9.0f / 16};
  const float pixel_aspects[] = {1, 7.0f / 6};
  for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
    for (unsigned a = 0; a < sizeof(aspects) / sizeof(aspects[0]); a++) {
      for (unsigned p = 0; p < sizeof(pixel_aspects) / sizeof(pixel_aspects[0]); p++) {
        DioramaSkyboxVerticalMapping mapping = {64, 288, 64.0f / 352, 288.0f / 352};
        const float width = DioramaSkyboxVerticalMapping_FitAspect(
            &mapping, 352, widths[w], aspects[a], pixel_aspects[p]);
        const float height = (mapping.texture_v1 - mapping.texture_v0) * 352;
        const float shape = aspects[a] * height / width;
        if (fabsf(shape - pixel_aspects[p]) > 0.00001f || width <= 0 ||
            width > widths[w] || mapping.capture_y0 < 64 || mapping.capture_y1 > 288 ||
            fabsf(mapping.capture_y0 + mapping.capture_y1 - 352) > 0.0001f) {
          printf("FAIL skybox aspect/coverage: available=%g output=%g par=%g got=%g\n",
                 widths[w], aspects[a], pixel_aspects[p], shape);
          s_failures++;
        }
      }
    }
  }
  /* Capture-row redistribution must not move a fixed source after fitting.
   * The wider capture crops horizontally, retaining the camera's V window. */
  for (int y = 504; y <= 543; y++) {
    const int top = y - 415, bg_camera = y / 3;
    DioramaSkyboxVerticalMapping mapping = {0, 352, 0, 1};
    DioramaSkyboxVerticalMapping_FollowCamera(&mapping, 352, top, 170.0f - bg_camera);
    const float width = DioramaSkyboxVerticalMapping_FitAspect(
        &mapping, 352, 620, 1.6f, 1);
    ExpectFloat("aspect fit width", width, 224 * 1.6f);
    ExpectFloat("aspect fit preserves camera stop",
                DioramaSkyboxVerticalMapping_Fraction(
                    &mapping, 299 - bg_camera + top), 129.0f / 224);
  }
  const float invalid[] = {0, -1, INFINITY, NAN};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    DioramaSkyboxVerticalMapping mapping = {64, 288, 64.0f / 352, 288.0f / 352};
    ExpectFloat("invalid aspect fails closed",
                DioramaSkyboxVerticalMapping_FitAspect(&mapping, 352, 620, invalid[i], 1), 0);
    ExpectFloat("invalid width fails closed",
                DioramaSkyboxVerticalMapping_FitAspect(&mapping, 352, invalid[i], 1.6f, 1), 0);
    ExpectFloat("invalid PAR fails closed",
                DioramaSkyboxVerticalMapping_FitAspect(&mapping, 352, 620, 1.6f, invalid[i]), 0);
    ExpectFloat("invalid mapping preserved", mapping.capture_y0, 64);
  }
}

static void TestSupportedCaptureBudgets(void) {
  const int budgets[] = {0, 26, 43, 52, 72, 120};
  for (unsigned i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    const int budget = budgets[i];
    /* Surface origin includes the OBJ guard; the BG validity budget does not. */
    const int origin = 64 + budget;
    DioramaBgValidSpanPlan spans;
    ActionBgLayerPlan layer = Layer(kActionBgEdge_LiveWorld);
    DioramaBgValidSpanPlan_Build(origin, budget, 0, budget, false,
                               &layer, NULL, 0, 224, 640, &spans);
    ExpectInt("budget start count", spans.count, 1);
    ExpectSpan("budget start", &spans.spans[0], 0, 224,
               origin, origin + 256 + budget);
    DioramaBgValidSpanPlan_Build(origin, budget, budget, 0, false,
                               &layer, NULL, 0, 224, 640, &spans);
    ExpectSpan("budget end", &spans.spans[0], 0, 224, 64, origin + 256);
    /* A room transition to clamped art cannot reuse the previous wide span. */
    layer = Layer(kActionBgEdge_Clamp);
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
                               &layer, NULL, 0, 224, 640, &spans);
    ExpectSpan("budget clamped", &spans.spans[0], 0, 224, origin, origin + 256);
    layer = Layer(kActionBgEdge_Repeat);
    DioramaBgValidSpanPlan_Build(origin, budget, 0, 0, true,
                               &layer, NULL, 0, 224, 640, &spans);
    ExpectSpan("budget repeat", &spans.spans[0], 0, 224, 64, 64 + 256 + 2 * budget);
    layer.horizontal_extent = (ActionBgHorizontalExtent){
        .mode = kActionBgExtent_Fixed, .left = 16, .right = 32,
    };
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
                               &layer, NULL, 0, 224, 640, &spans);
    ExpectSpan("budget fixed cap", &spans.spans[0], 0, 224,
               origin - (budget < 16 ? budget : 16),
               origin + 256 + (budget < 32 ? budget : 32));
  }
}

static void TestFiniteParallaxBounds(void) {
  /* All supported source margins (square/CRT, flat/diorama, and native).
   * The playfield can reach its fitted stop before a half-speed BG2. */
  const int budgets[] = {0, 26, 43, 52, 72, 120};
  for (unsigned i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    const int budget = budgets[i], origin = 64 + budget;
    const int available = budget / 2;
    ActionBgLayerPlan layer = Layer(kActionBgEdge_LiveWorld);
    layer.source = kActionBgSource_WorldMap;
    layer.world_width = 1024;
    DioramaBgSourceBounds source = {
        .x0 = -available, .x1 = 1024 - available, .valid = true,
    };
    DioramaBgValidSpanPlan spans;
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectInt("parallax left count", spans.count, 1);
    ExpectSpan("parallax left", &spans.spans[0], 0, 224,
               origin - available, origin + 256 + budget);
    source.x0 = -(1024 - 256 - available);
    source.x1 = 256 + available;
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectSpan("parallax right", &spans.spans[0], 0, 224,
               origin - budget, origin + 256 + available);

    /* Authored terrain can extend the source, but never invent uncaptured
     * pixels beyond the shared capture or a tuned policy's fixed cap. */
    source = (DioramaBgSourceBounds){-160, 1024, true};
    DioramaBgValidSpanPlan_Build(origin, budget, 0, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectSpan("capture still bounds source", &spans.spans[0], 0, 224,
               origin, origin + 256 + budget);
    layer.horizontal_extent = (ActionBgHorizontalExtent){
        .mode = kActionBgExtent_Fixed, .left = 16, .right = 32,
    };
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectSpan("authored fixed caps", &spans.spans[0], 0, 224,
        origin - (budget < 16 ? budget : 16),
        origin + 256 + (budget < 32 ? budget : 32));

    /* Entering a narrow room replaces the previous source interval. */
    layer.horizontal_extent = (ActionBgHorizontalExtent){.mode = kActionBgExtent_Available};
    layer.world_width = 256;
    source = (DioramaBgSourceBounds){0, 256, true};
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectSpan("narrow finite room", &spans.spans[0], 0, 224, origin, origin + 256);
    layer.wrap_world_x = true;
    DioramaBgValidSpanPlan_Build(origin, budget, budget, budget, true,
        &layer, &source, 0, 224, 640, &spans);
    ExpectSpan("cyclic world", &spans.spans[0], 0, 224, 64, 64 + 256 + 2 * budget);
  }
}

static void TestRasterSourceBounds(void) {
  ActionBgLayerPlan layer = Layer(kActionBgEdge_LiveWorld);
  layer.source = kActionBgSource_WorldMap;
  layer.world_width = 1024;
  layer.bands[0] = (ActionBgBand){
      .y0 = 136, .y1 = 224, .edge = kActionBgEdge_Repeat,
      .horizontal_extent = {.mode = kActionBgExtent_Available},
  };
  layer.band_count = 1;
  DioramaBgSourceBounds source = {0};
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, -60, 964, 1);
  DioramaBgSourceBounds_AddRow(&source, &layer, 1, -52, 972, 1);
  DioramaBgSourceBounds_AddRow(&source, &layer, 2, -68, 956, 1);
  /* Synthetic and vertically unavailable rows do not narrow the live sky. */
  DioramaBgSourceBounds_AddRow(&source, &layer, 150, 100, 200, 1);
  DioramaBgSourceBounds_AddRow(&source, &layer, -65, 100, 200, 1);
  ExpectInt("raster interval valid", source.valid, true);
  ExpectInt("raster left intersection", source.x0, -52);
  ExpectInt("raster right intersection", source.x1, 956);
  DioramaBgValidSpanPlan spans;
  DioramaBgValidSpanPlan_Build(184, 120, 120, 120, true,
      &layer, &source, 0, 224, 640, &spans);
  ExpectInt("raster band count", spans.count, 2);
  ExpectSpan("raster finite sky", &spans.spans[0], 0, 136, 132, 560);
  ExpectSpan("raster repeating water", &spans.spans[1], 136, 224, 64, 560);

  source = (DioramaBgSourceBounds){0};
  layer.wrap_world_x = true;
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, 0, 256, 1);
  ExpectInt("cyclic rows do not create finite bounds", source.valid, false);
  layer.wrap_world_x = false;
  layer.source = kActionBgSource_AuthenticViewport;
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, 0, 256, 1);
  ExpectInt("viewport rows do not create finite bounds", source.valid, false);
  layer.source = kActionBgSource_WorldMap;
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, -60, 964, 1);
  DioramaBgSourceBounds_AddRow(&source, &layer, 1, 1000, 1100, 1);
  DioramaBgValidSpanPlan_Build(184, 120, 120, 120, true,
      &layer, &source, 0, 136, 640, &spans);
  ExpectSpan("empty source intersection", &spans.spans[0], 0, 136, 0, 0);

  source = (DioramaBgSourceBounds){0};
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, -60, 956, 16);
  ExpectInt("mosaic negative left", source.x0, -48);
  ExpectInt("mosaic right includes final group", source.x1, 960);
  source = (DioramaBgSourceBounds){0};
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, 1, 257, 16);
  ExpectInt("mosaic positive left", source.x0, 16);
  ExpectInt("mosaic positive right", source.x1, 272);
  source = (DioramaBgSourceBounds){0};
  DioramaBgSourceBounds_AddRow(&source, &layer, 0, -64, 960, 16);
  ExpectInt("aligned mosaic left", source.x0, -64);
  ExpectInt("aligned mosaic right", source.x1, 960);
}

static void TestWorldLockedSkybox(void) {
  /* Project a fixed waterfall rock through the foreground camera, then
   * recover the source sampled under it. Camera travel, extra rows, aspect
   * and perspective must not change that source coordinate. */
  for (int rows = 0; rows <= 64; rows += 32) {
    for (int crt = 0; crt < 2; ++crt) {
      const float par = crt ? 7.0f / 6.0f : 1.0f;
      const float width = 496, height = 224 + 2 * rows;
      const float aspect = width / 224 * par, scale = height / 224;
      const float m[16] = {2,0,0,.15f, 0,3,0,.1f, 0,0,1,0, -.2f,.1f,0,4};
      for (int camera = 0; camera < 3; ++camera) {
        const float cx = 80 + camera * 175, cy = 64 + camera * 103;
        const float capture_x = 570 - cx + 120;
        const float capture_y = 630 - cy - 1 + rows;
        const float wx = (capture_x / width - .5f) * aspect;
        const float wy = (.5f - capture_y / height) * scale;
        const float w = m[3] * wx + m[7] * wy + m[15];
        const float sx = .5f + .5f * (m[0] * wx + m[12]) / w;
        const float sy = .5f - .5f * (m[5] * wy + m[13]) / w;
        float x = 0, y = 0;
        ExpectInt("world mapping valid", DioramaSkyboxWorldPoint(
            m, 0, aspect, scale, sx, sy, &x, &y), 1);
        const float world_x = x * width - 120 + cx;
        const float world_y = y * height - rows + cy + 1;
        ExpectInt("waterfall X stays registered", fabsf(world_x - 570) < .001f, 1);
        ExpectInt("waterfall Y stays registered", fabsf(world_y - 630) < .001f, 1);
      }
    }
  }
  float x = 0, y = 0;
  float degenerate[16] = {0};
  ExpectInt("horizon fails closed", DioramaSkyboxWorldPoint(
      degenerate, 0, 1, 1, .5f, .5f, &x, &y), 0);
  ExpectInt("invalid scale fails closed", DioramaSkyboxWorldPoint(
      degenerate, 0, NAN, 1, .5f, .5f, &x, &y), 0);
}

int main(void) {
  TestWorldLockedSkybox();
  TestFiniteParallaxBounds();
  TestRasterSourceBounds();
  TestSupportedCaptureBudgets();
  TestValidSpan();
  TestBandedValidSpans();
  TestExtentValidSpans();
  TestUvRangeMatchesLegacyOnFullSpan();
  TestLiveVerticalWorldMapping();
  TestSkyboxFollowsNativeWindow();
  TestSkyboxAspectFit();
  TestUvRangeCropsNarrowedSpan();
  TestUvRangeNeverInverts();
  TestRomUvRepeatsAcrossDisplayedWidth();
  if (s_failures) {
    printf("diorama_skybox_uv_test: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("diorama_skybox_uv_test: all checks passed\n");
  return 0;
}
