/* BG2 valid-span classification and the skybox
 * quad's UV range.
 *
 * The load-bearing assertion in here is the NO-OP one: wherever Fix A padded
 * BG2's margins out to the budget, the new UV math must produce values
 * bit-identical to the pre-fix expression. If that ever stops holding, the fix
 * has silently started cropping frames it was never meant to touch.
 */
#include "diorama_skybox_uv.h"
#include "action/action_room_scene.h"
#include "render/presentation_options.h"

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

/* Fillmore act 1, measured: the fitted playfield camera travels 120..3720,
 * BG2 = BG1/2 over a 2304-wide world, and the 496-wide view is drawn about
 * 1/0.745 the size of the playfield on screen. */
static void TestSkyboxScrollPlacement(void) {
  DioramaSkyboxScrollInput in = {
    .mode = kDioramaSkyScroll_FullRoom, .camera_x = 120,
    .camera_min = 120, .camera_max = 3720,
    .ratio_numerator = 1, .ratio_denominator = 2,
    .playfield_to_sky_scale = 0.745f, .world_width = 2304, .view_width = 496,
  };
  float left = -1.0f;
  /* Full room: BG2's whole width across the travel, ends exactly at its edges,
   * so nothing pins while the playfield still moves. */
  ExpectInt("full room places", DioramaSkyboxScroll_Place(&in, &left), 1);
  ExpectFloat("full room starts at the left edge", left, 0.0f);
  in.camera_x = 3720;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("full room ends at the right edge", left, 1808.0f);
  in.camera_x = 1920;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("full room is linear", left, 904.0f);
  /* Past the travel the sky holds with the playfield. */
  in.camera_x = 4000;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("holds past the right stop", left, 1808.0f);

  /* Matched: native 1/2 parallax at the playfield's scale, centred so the
   * unused BG2 is split between both ends. */
  in.mode = kDioramaSkyScroll_Matched;
  const float rate = 0.5f * 0.745f;
  const float base = 0.5f * (1808.0f - rate * 3600.0f);
  in.camera_x = 120;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("matched starts centred", left, base);
  in.camera_x = 122;
  float next = 0.0f;
  DioramaSkyboxScroll_Place(&in, &next);
  /* A difference of two values near 233; compare at float precision there. */
  ExpectInt("matched moves at the scaled parallax",
            fabsf((next - left) - 2.0f * rate) < 1e-4f, 1);
  in.camera_x = 3720;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("matched ends centred", left, base + rate * 3600.0f);
  /* Unknown scale falls back to the native ratio; a larger one is capped at
   * the room so the view never reaches an edge early. */
  in.playfield_to_sky_scale = 0.0f;
  in.camera_x = 120;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("unmeasured scale uses the native ratio", left, 4.0f);
  in.playfield_to_sky_scale = 2.0f;
  DioramaSkyboxScroll_Place(&in, &left);
  ExpectFloat("fast scale is capped at the room", left, 0.0f);

  /* Original placement stays with the caller. */
  in.mode = kDioramaSkyScroll_Original;
  ExpectInt("original declines", DioramaSkyboxScroll_Place(&in, &left), 0);
  in.mode = kDioramaSkyScroll_Matched;
  in.ratio_numerator = 0;
  ExpectInt("no native parallax declines", DioramaSkyboxScroll_Place(&in, &left), 0);
  in.ratio_numerator = 1;
  in.camera_max = in.camera_min;
  ExpectInt("no travel declines", DioramaSkyboxScroll_Place(&in, &left), 0);
  in.camera_max = 3720;
  in.world_width = in.view_width;
  ExpectInt("no room declines", DioramaSkyboxScroll_Place(&in, &left), 0);
}

/* Death Heim rematches (R9): moon rows still, mountains at camera/4, hills
 * at camera/2. Holding the playfield 8 native pixels right must move those
 * rows by -2 and -4 texels and leave the moon and margins alone. */
static void TestHoldRasterBands(void) {
  float rows[224];
  for (int y = 0; y < 224; ++y) rows[y] = y < 0x4f ? 0.0f : y < 0x8f ? 0.25f : 0.5f;
  const DioramaBgValidSpanPlan spans = {.count = 1, .spans = {{0, 288, 0, 496}}};
  DioramaBgValidSpanPlan out;
  float shift[kDioramaBgMaxValidSpans];
  ExpectInt("hold splits",
            DioramaBgValidSpanPlan_HoldRaster(
                &spans, 32, rows, NULL, 136, 136, 8.0f,
                &out, shift, NULL), 1);
  ExpectInt("hold piece count", out.count, 4);
  ExpectInt("moon rows end", out.spans[0].y1, 32 + 0x4f);
  ExpectInt("mountain rows end", out.spans[1].y1, 32 + 0x8f);
  ExpectInt("hill rows end", out.spans[2].y1, 32 + 224);
  ExpectInt("margin keeps width", out.spans[3].x1, 496);
  ExpectFloat("moon still", shift[0], 0.0f);
  ExpectFloat("mountains held", shift[1], -2.0f);
  ExpectFloat("hills held", shift[2], -4.0f);
  ExpectFloat("lower margin still", shift[3], 0.0f);
  /* Holding right from camera 186 to 216: the view cancels the camera, the
   * table lags a frame (raster camera = camera - 2) and the measured hold
   * carries float noise. Every shown band keeps its stopped native pixel. */
  for (int camera = 188; camera <= 216; camera += 2) {
    const float hold = (float)(camera - 186) + (camera % 3 ? 0.03f : -0.04f);
    const int raster = camera - 2;
    ExpectInt("held split", DioramaBgValidSpanPlan_HoldRaster(
        &spans, 32, rows, NULL, camera, raster, hold, &out, shift, NULL), 1);
    ExpectInt("mountains keep their place",
              fabsf((float)(raster >> 2) + shift[1] - 186 / 4.0f) < 0.02f, 1);
    ExpectInt("hills keep their place",
              fabsf((float)(raster >> 1) + shift[2] - 186 / 2.0f) < 0.03f, 1);
  }
  /* Releasing the hold while walking left: the table still holds the
   * previous (larger) camera, yet the shown band continues from the held
   * one without stepping back. */
  for (int camera = 178; camera >= 170; camera -= 2) {
    const float hold = camera > 175 ? (float)(camera - 175) : 0.0f;
    const float presented = (float)camera - hold;
    const bool shifted = DioramaBgValidSpanPlan_HoldRaster(
        &spans, 32, rows, NULL, camera, camera + 2, hold, &out, shift, NULL);
    const float shown = (float)((camera + 2) >> 1) + (shifted ? shift[2] : 0.0f);
    ExpectInt("release follows the presented camera",
              fabsf(shown - presented / 2.0f) < 1e-3f, 1);
  }
  ExpectInt("aligned still camera declines",
            DioramaBgValidSpanPlan_HoldRaster(
                &spans, 32, rows, NULL, 136, 136, 0.0f,
                &out, shift, NULL), 0);
  for (int y = 0; y < 224; ++y) rows[y] = 0.0f;
  ExpectInt("static raster declines",
            DioramaBgValidSpanPlan_HoldRaster(
                &spans, 32, rows, NULL, 136, 136, 8.0f,
                &out, shift, NULL), 0);
  /* Without a row table, a per-row gradient cannot fit the band budget;
   * fall back unshifted. */
  for (int y = 0; y < 224; ++y) rows[y] = (float)y / 256.0f;
  ExpectInt("gradient declines",
            DioramaBgValidSpanPlan_HoldRaster(
                &spans, 32, rows, NULL, 136, 136, 8.0f,
                &out, shift, NULL), 0);
}

/* Shown position of one row after the hold: what the table holds plus the
 * band's shift plus the row's own remainder. */
static float ShownRow(float rate, int raster_camera, float band_shift, float row_texels) {
  return floorf(rate * (float)raster_camera) + band_shift + row_texels;
}

/* Northwall (R6): five sky steps and the hills scroll at whole fractions of
 * the camera, but its floor's 32 rows each take their own rate (2i+1)/64.
 * Splitting per rate needs 38 bands; holding still has to move every row. */
static void TestHoldRasterRows(void) {
  float rows[224];
  for (int y = 0; y < 224; ++y) {
    rows[y] = y < 30 ? 0.5f : y < 46 ? 0.25f : y < 62 ? 0.125f : y < 70 ? 0.0625f
        : y < 78 ? 0.03125f : y < 190 ? 0.5f : (float)(2 * (y < 222 ? y - 190 : 31) + 1) / 64.0f;
  }
  const DioramaBgValidSpanPlan spans = {.count = 3, .spans = {
      {0, 64, 0, 0}, {64, 319, 64, 560}, {319, 352, 0, 0}}};
  DioramaBgValidSpanPlan out;
  float shift[kDioramaBgMaxValidSpans];
  static DioramaRasterRowShift row_shift;
  ExpectInt("per-rate split overflows without rows",
            DioramaBgValidSpanPlan_HoldRaster(
                &spans, 64, rows, NULL, 900, 898, 12.5f,
                &out, shift, NULL), 0);
  bool remainders = false;
  for (int camera = 900; camera <= 960; camera += 3) {
    const float hold = 4.0f + 0.37f * (float)(camera - 900);
    const int raster = camera - 2;
    row_shift = (DioramaRasterRowShift){.active = false};
    ExpectInt("rows hold", DioramaBgValidSpanPlan_HoldRaster(
        &spans, 64, rows, NULL, camera, raster, hold, &out, shift, &row_shift), 1);
    ExpectInt("one band slot stays free", out.count <= kDioramaBgMaxValidSpans - 1, 1);
    remainders |= row_shift.active;
    ExpectSpan("empty top kept", &out.spans[0], 0, 64, 0, 0);
    ExpectSpan("empty bottom kept", &out.spans[out.count - 1], 319, 352, 0, 0);
    const float presented = (float)camera - hold;
    for (unsigned i = 0; i < out.count; ++i) {
      for (int y = out.spans[i].y0; y < out.spans[i].y1; ++y) {
        if (out.spans[i].x1 <= out.spans[i].x0) continue;
        const int row = y - 64;
        const bool native = row >= 0 && row < 224;
        const float rate = native ? rows[row] : 0.0f;
        const float texels = native ? row_shift.texels[row] : 0.0f;
        if (!native) {
          ExpectFloat("rows past the frame stay put", shift[i], 0.0f);
          continue;
        }
        if (fabsf(ShownRow(rate, raster, shift[i], texels) - rate * presented) > 1e-3f) {
          printf("FAIL row %d at camera %d: shown %.4f, want %.4f\n", row, camera,
                 ShownRow(rate, raster, shift[i], texels), rate * presented);
          s_failures++;
        }
      }
    }
    /* The long hill band keeps its own slot and exact shift: effects
     * projected through the band mapping stay on its art. */
    bool hills = false;
    for (unsigned i = 0; i < out.count; ++i)
      hills |= out.spans[i].y0 == 64 + 78 && out.spans[i].y1 == 64 + 190;
    ExpectInt("hill band kept whole", hills, 1);
    for (int row = 78; row < 190; ++row)
      ExpectFloat("hill rows need no remainder", row_shift.texels[row], 0.0f);
  }

  ExpectInt("floor rows carry remainders", remainders, 1);
  /* Unheld, the bands still follow the presented camera, but the floor's
   * per-row gradient keeps its native table; it eases in with the hold. */
  for (int step = 0; step <= 2; ++step) {
    const float hold = 2.0f * (float)step, ease = 0.5f * (float)step;
    const float presented = 900.0f - hold;
    ExpectInt("bands hold unheld", DioramaBgValidSpanPlan_HoldRaster(
        &spans, 64, rows, NULL, 900, 898, hold, &out, shift, &row_shift), 1);
    for (int row = 0; row < 224; ++row) {
      const float rate = rows[row];
      const float native = floorf(rate * 898.0f);
      float band = 0.0f;
      for (unsigned i = 0; i < out.count; ++i)
        if (out.spans[i].y0 <= row + 64 && row + 64 < out.spans[i].y1) band = shift[i];
      const float weight = row >= 190 ? ease : 1.0f;
      if (fabsf(native + band + row_shift.texels[row] -
                (native + weight * (rate * presented - native))) > 1e-3f) {
        printf("FAIL ease row %d hold %.0f\n", row, hold);
        s_failures++;
      }
    }
  }
  /* A room that is only a gradient (Bloodpool, R2) stays native unheld. */
  float gradient[224];
  for (int y = 0; y < 224; ++y)
    gradient[y] = y < 127 ? 0.0f : (float)(y < 223 ? y - 126 : 96) / 128.0f;
  ExpectInt("gradient room unheld stays native", DioramaBgValidSpanPlan_HoldRaster(
      &spans, 64, gradient, NULL, 900, 898, 0.0f, &out, shift, &row_shift), 0);
  ExpectInt("gradient room holds", DioramaBgValidSpanPlan_HoldRaster(
      &spans, 64, gradient, NULL, 900, 898, 9.0f, &out, shift, &row_shift), 1);
  ExpectInt("held again", DioramaBgValidSpanPlan_HoldRaster(
      &spans, 64, rows, NULL, 960, 958, 26.0f, &out, shift, &row_shift), 1);

  /* Runs cover one band in rows of equal remainder; a band with none draws
   * whole. */
  DioramaRasterRowRun runs[352];
  const unsigned floor_runs = DioramaRasterRowShift_Runs(
      &row_shift, 64, 64 + 186, 64 + 224, runs, 352);
  ExpectInt("floor runs", floor_runs >= 32, 1);
  ExpectInt("runs start at the band", runs[0].y0, 64 + 186);
  ExpectInt("runs end at the band", runs[floor_runs - 1].y1, 64 + 224);
  for (unsigned i = 1; i < floor_runs; ++i)
    ExpectInt("runs are contiguous", runs[i].y0, runs[i - 1].y1);
  ExpectInt("hills draw whole", DioramaRasterRowShift_Runs(
      &row_shift, 64, 64 + 78, 64 + 190, runs, 352), 0);
  ExpectInt("too many runs draw whole", DioramaRasterRowShift_Runs(
      &row_shift, 64, 64 + 186, 64 + 224, runs, 8), 0);

  /* Bands that fit the budget never use the row table (Death Heim R9). */
  for (int y = 0; y < 224; ++y) rows[y] = y < 0x4f ? 0.0f : y < 0x8f ? 0.25f : 0.5f;
  const DioramaBgValidSpanPlan r9 = {.count = 1, .spans = {{0, 288, 0, 496}}};
  row_shift.active = true;
  ExpectInt("R9 still splits", DioramaBgValidSpanPlan_HoldRaster(
      &r9, 32, rows, NULL, 136, 136, 8.0f, &out, shift, &row_shift), 1);
  ExpectInt("R9 piece count", out.count, 4);
  ExpectInt("R9 needs no rows", row_shift.active, 0);
}

/* The probe must report each row's whole rate even where the table keeps
 * only the low scroll byte, which wraps whenever the camera crosses a
 * multiple of 256 / rate. */
static void TestRasterCameraSensitivity(void) {
  static ActionRoomScene scene;
  scene.have_video_profile = true;
  scene.have_raster_waveform = true;
  float rows[kActionRoomSceneFrameHeight];
  for (int camera = 0; camera < 2400; camera += 37) {
    const ActionRoomSceneFrameRequest request = {
      .camera_x = camera, .game_frame = (uint16_t)(1000 + camera),
      .animation_phase = -1, .page_phase = -1};
    scene.raster_effect = kActionRoomRaster_Bg2ParallaxPerspective;
    ExpectInt("R6 probes",
              ActionRoomScene_RasterCameraSensitivity(&scene, &request, rows), 1);
    ExpectFloat("R6 sky", rows[0], 0.5f);
    ExpectFloat("R6 high sky", rows[70], 0.03125f);
    ExpectFloat("R6 hills", rows[150], 0.5f);
    for (int i = 0; i < 32; ++i)
      ExpectFloat("R6 floor row", rows[190 + i], (float)(2 * i + 1) / 64.0f);
    scene.raster_effect = kActionRoomRaster_Bg2LowerPerspective;
    ActionRoomScene_RasterCameraSensitivity(&scene, &request, rows);
    ExpectFloat("R2 upper rows", rows[0x7e], 0.0f);
    for (int i = 0; i < 96; ++i)
      ExpectFloat("R2 floor row", rows[0x7f + i], (float)(i + 1) / 128.0f);
    scene.raster_effect = kActionRoomRaster_Bg2CameraParallaxBands;
    ActionRoomScene_RasterCameraSensitivity(&scene, &request, rows);
    ExpectFloat("R9 moon", rows[0x4e], 0.0f);
    ExpectFloat("R9 mountains", rows[0x4f], 0.25f);
    ExpectFloat("R9 hills", rows[0x8f], 0.5f);
    /* Aitos (R5) also has BG2 base parallax 1/2; the raster replaces it on
     * every row, so the bands keep their own rates. */
    scene.raster_effect = kActionRoomRaster_Bg2LayeredParallax;
    scene.video_profile[9] = 0x12;
    ActionRoomScene_RasterCameraSensitivity(&scene, &request, rows);
    ExpectFloat("R5 sky", rows[0], 0.5f);
    ExpectFloat("R5 far mountains", rows[87], 0.0625f);
    ExpectFloat("R5 hills", rows[200], 0.5f);
    scene.video_profile[9] = 0;
    scene.raster_effect = kActionRoomRaster_None;
    ExpectInt("no raster declines",
              ActionRoomScene_RasterCameraSensitivity(&scene, &request, rows), 0);
  }
}

/* Rows of several lines at one rate are held as bands. */
static bool BandRow(const float rates[224], int row) {
  int y0 = row, y1 = row + 1;
  while (y0 > 0 && rates[y0 - 1] == rates[row]) --y0;
  while (y1 < 224 && rates[y1] == rates[row]) ++y1;
  return rates[row] != 0.0f && y1 - y0 >= 4;
}

/* Aitos (R5) horizon clouds scroll (2 * frame + camera / 2) >> n and
 * Northwall's (R6) high sky (4 * frame + camera / 2) >> n: they step on their
 * own camera phase, not where floor(rate * camera) does. Each band row's
 * table plus its subtexel must advance by exactly its rate per camera pixel,
 * or the held band jumps a whole texel and back. */
static void TestRasterSubtexel(void) {
  static ActionRoomScene scene;
  static ActionRoomSceneFrameState state;
  scene.have_video_profile = true;
  scene.have_raster_waveform = true;
  float rates[kActionRoomSceneFrameHeight], sub[kActionRoomSceneFrameHeight];
  float previous[kActionRoomSceneFrameHeight];
  static const struct { int effect; uint8_t base; } kBandRooms[] = {
    {kActionRoomRaster_Bg2LayeredParallax, 0x12},     /* Aitos */
    {kActionRoomRaster_Bg2ParallaxPerspective, 0x02}, /* Northwall */
    {kActionRoomRaster_Bg2CameraParallaxBands, 0x01}, /* Death Heim */
  };
  for (unsigned room = 0; room < sizeof kBandRooms / sizeof kBandRooms[0]; ++room) {
    scene.raster_effect = kBandRooms[room].effect;
    scene.video_profile[9] = kBandRooms[room].base;
    for (int frame = 1001; frame <= 1004; ++frame)
      for (int camera = 600; camera <= 680; ++camera) {
        const ActionRoomSceneFrameRequest request = {
          .camera_x = camera, .game_frame = (uint32_t)frame,
          .animation_phase = -1, .page_phase = -1};
        ExpectInt("band rates",
                  ActionRoomScene_RasterCameraSensitivity(&scene, &request, rates), 1);
        ExpectInt("band subtexel", ActionRoomScene_RasterSubtexel(&scene, &request, rates, sub), 1);
        ExpectInt("band state", ActionRoomScene_BuildFrameState(&scene, &request, &state), 1);
        for (int row = 0; row < kActionRoomSceneFrameHeight; ++row) {
          const float smooth = (float)state.bg_hscroll[1][row] + sub[row];
          const float moved =
              fmodf(smooth - previous[row] - rates[row] + 1536.0f, 1024.0f) - 512.0f;
          if (camera > 600 && BandRow(rates, row))
            ExpectInt("band row moves at its rate", fabsf(moved) < 1e-3f, 1);
          previous[row] = smooth;
        }
      }
  }
  scene.video_profile[9] = 0;
  /* Holding the band applies that phase. */
  float rows[224], phase[224];
  for (int y = 0; y < 224; ++y) { rows[y] = 0.125f; phase[y] = 0.5f; }
  const DioramaBgValidSpanPlan spans = {.count = 1, .spans = {{0, 224, 0, 496}}};
  DioramaBgValidSpanPlan out;
  float shift[kDioramaBgMaxValidSpans];
  ExpectInt("phased hold", DioramaBgValidSpanPlan_HoldRaster(
      &spans, 0, rows, phase, 600, 600, 0.0f, &out, shift, NULL), 1);
  ExpectFloat("phase shows", shift[0], 0.5f);
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
  TestSkyboxScrollPlacement();
  TestHoldRasterBands();
  TestHoldRasterRows();
  TestRasterCameraSensitivity();
  TestRasterSubtexel();
  if (s_failures) {
    printf("diorama_skybox_uv_test: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("diorama_skybox_uv_test: all checks passed\n");
  return 0;
}
