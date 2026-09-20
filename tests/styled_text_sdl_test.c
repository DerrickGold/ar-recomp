#include "host/font_resources.h"
#include "localization/text_boundaries.h"
#include "localization/unicode_grapheme.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static const ArTextRunAppearance kWhite = {.font_role = "body",
                                           .scale_basis = 10000,
                                           .band_rgb = 0xffffff,
                                           .body_rgb = 0xffffff};

static ArTextRasterRequest Request(const char *text) {
  return (ArTextRasterRequest){.struct_size = sizeof(ArTextRasterRequest),
                               .abi_version =
                                   AR_TEXT_RASTER_REQUEST_ABI_VERSION,
                               .utf8 = text,
                               .utf8_bytes = strlen(text),
                               .font_stack_id = "sample",
                               .font_stack_id_bytes = 6,
                               .font_revision = 1,
                               .source_revision = 1,
                               .font_pixels = 30,
                               .minimum_font_pixels = 30,
                               .maximum_width = 600,
                               .maximum_height = 500,
                               .filter = kArRenderFilter_Linear,
                               .flags = kArTextRasterFlag_WrapWords |
                                        kArTextRasterFlag_PreserveHardBreaks |
                                        kArTextRasterFlag_IncludeRevealClusters,
                               .appearance = &kWhite};
}

static bool Raster(const ArTextRasterizer *rasterizer,
                   const ArTextRasterRequest *request, ArTextBitmap *bitmap) {
  char error[256] = {0};
  const bool ok = ArTextRasterizer_Rasterize(rasterizer, request, bitmap, NULL,
                                             error, sizeof(error));
  if (!ok)
    fprintf(stderr, "raster: %s (%s)\n", error, request->utf8);
  CHECK(ok);
  return ok;
}

static uint32_t Pixel(const ArTextBitmap *bitmap, int x, int y) {
  uint32_t pixel;
  memcpy(&pixel,
         (const uint8_t *)bitmap->pixels + (size_t)y * bitmap->pitch_bytes +
             (size_t)x * 4,
         4);
  return pixel;
}

static void SameGeometry(const ArTextBitmap *a, const ArTextBitmap *b) {
  if (a->width != b->width || a->height != b->height)
    fprintf(stderr, "geometry %dx%d versus %dx%d\n", a->width, a->height,
            b->width, b->height);
  CHECK(a->width == b->width && a->height == b->height);
  CHECK(a->reveal_cluster_count == b->reveal_cluster_count);
  if (a->width != b->width || a->height != b->height ||
      a->reveal_cluster_count != b->reveal_cluster_count)
    return;
  for (size_t i = 0; i < a->reveal_cluster_count; ++i) {
    const ArTextRevealCluster *x = &a->reveal_clusters[i],
                              *y = &b->reveal_clusters[i];
    CHECK(x->end_utf8_byte == y->end_utf8_byte &&
          x->line_index == y->line_index && x->x == y->x && x->y == y->y &&
          x->width == y->width && x->height == y->height);
  }
  bool same_alpha = true;
  for (int y = 0; y < a->height; ++y)
    for (int x = 0; x < a->width; ++x)
      same_alpha &= (Pixel(a, x, y) & 255) == (Pixel(b, x, y) & 255);
  CHECK(same_alpha);
}

static void TestPaintDoesNotShape(const ArTextRasterizer *rasterizer) {
  const char *samples[] = {
      "ordinary office text wraps here", "مرحبا بالعالم 123", "שלום ABC 123",
      "日本語の文章が折り返されます", "Cafe\xcc\x81 with combining accents"};
  for (size_t s = 0; s < sizeof(samples) / sizeof(samples[0]); ++s) {
    ArTextRasterRequest request = Request(samples[s]);
    request.maximum_width = 190;
    ArTextBitmap original = {0}, painted = {0};
    if (!Raster(rasterizer, &request, &original))
      continue;
    for (size_t i = 0; i < original.font_use_count; ++i)
      CHECK(!original.font_uses[i].missing);
    size_t first = 0;
    CHECK(ArUnicodeGrapheme_Next(request.utf8, request.utf8_bytes, 0, NULL,
                                 &first));
    ArTextAppearanceSpan span = {.start = (uint32_t)first,
                                 .end = (uint32_t)request.utf8_bytes,
                                 .appearance = kWhite};
    span.appearance.band_rgb = span.appearance.body_rgb = 0xffcc00;
    request.appearance_spans = &span;
    request.appearance_span_count = 1;
    if (Raster(rasterizer, &request, &painted))
      SameGeometry(&original, &painted);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &original);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &painted);
  }
  /* A paint boundary inside an optional Latin ligature keeps the cluster
   * whole and uses its first logical character's treatment. */
  ArTextRasterRequest request = Request("office");
  ArTextBitmap original = {0}, painted = {0};
  Raster(rasterizer, &request, &original);
  ArTextAppearanceSpan span = {.start = 2, .end = 4, .appearance = kWhite};
  span.appearance.band_rgb = span.appearance.body_rgb = 0xff0000;
  request.appearance_spans = &span;
  request.appearance_span_count = 1;
  if (Raster(rasterizer, &request, &painted) && original.pixels)
    SameGeometry(&original, &painted);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &original);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &painted);
}

static void TestUniformMigration(const ArTextRasterizer *rasterizer) {
  const char *samples[] = {"The people offer their gratitude.", "HP 12",
                           "مرحبا ABC 123"};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    ArTextRasterRequest request = Request(samples[i]);
    ArTextBitmap styled = {0}, legacy = {0};
    Raster(rasterizer, &request, &styled);
    request.appearance = NULL;
    if (Raster(rasterizer, &request, &legacy) && styled.pixels)
      SameGeometry(&styled, &legacy);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &styled);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &legacy);
  }
}

static const ArTextRevealCluster *Cluster(const ArTextBitmap *bitmap,
                                          size_t end) {
  for (size_t i = 0; i < bitmap->reveal_cluster_count; ++i)
    if (bitmap->reveal_clusters[i].end_utf8_byte == end)
      return &bitmap->reveal_clusters[i];
  return NULL;
}

static int InkBottom(const ArTextBitmap *bitmap,
                     const ArTextRevealCluster *cluster) {
  int bottom = -1;
  for (int y = cluster->y;
       y < cluster->y + cluster->height && y < bitmap->height; ++y)
    for (int x = cluster->x;
         x < cluster->x + cluster->width && x < bitmap->width; ++x)
      if (x >= 0 && y >= 0 && (Pixel(bitmap, x, y) & 255))
        bottom = y;
  return bottom;
}

static void TestMixedSizesAndRoles(const ArTextRasterizer *rasterizer) {
  ArTextRasterRequest request = Request("H H H\nH");
  ArTextAppearanceSpan spans[] = {{.start = 0, .end = 1, .appearance = kWhite},
                                  {.start = 4, .end = 5, .appearance = kWhite}};
  spans[0].appearance.scale_basis = 8000;
  spans[1].appearance.scale_basis = 12000;
  request.appearance_spans = spans;
  request.appearance_span_count = 2;
  ArTextBitmap bitmap = {0};
  if (Raster(rasterizer, &request, &bitmap)) {
    CHECK(bitmap.line_count == 2 && bitmap.lines);
    if (bitmap.line_count == 2) {
      CHECK(bitmap.lines[0].baseline > bitmap.lines[0].top);
      CHECK(bitmap.lines[1].top >=
            bitmap.lines[0].top + bitmap.lines[0].height);
    }
    const ArTextRevealCluster *small = Cluster(&bitmap, 1),
                              *normal = Cluster(&bitmap, 3);
    const ArTextRevealCluster *large = Cluster(&bitmap, 5),
                              *next = Cluster(&bitmap, 7);
    CHECK(small && normal && large && next);
    if (small && normal && large && next) {
      CHECK(small->height < normal->height && normal->height < large->height);
      CHECK(InkBottom(&bitmap, small) == InkBottom(&bitmap, normal));
      CHECK(InkBottom(&bitmap, normal) == InkBottom(&bitmap, large));
      CHECK(next->y >= large->y + large->height);
    }
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  strcpy(spans[1].appearance.font_role, "alternate");
  Raster(rasterizer, &request, &bitmap);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request.maximum_width = 80;
  request.maximum_height = 160;
  if (Raster(rasterizer, &request, &bitmap)) {
    CHECK(bitmap.width <= 80 && bitmap.height <= 160);
    CHECK(bitmap.reveal_cluster_count >= 4);
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
}

static void TestBoundariesAndEffects(const ArTextRasterizer *rasterizer) {
  ArTextRasterRequest request = Request("e\xcc\x81");
  ArTextAppearanceSpan span = {.start = 0, .end = 1, .appearance = kWhite};
  request.appearance_spans = &span;
  request.appearance_span_count = 1;
  ArTextBitmap bitmap = {0};
  ArTextRasterFailure failure = kArTextRasterFailure_None;
  char error[256] = {0};
  CHECK(!ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, &failure,
                                    error, sizeof(error)));
  CHECK(failure == kArTextRasterFailure_Deterministic &&
        strstr(error, "grapheme"));
  request = Request("12 italic ending");
  span.end = (uint32_t)request.utf8_bytes;
  span.appearance.italic = true;
  span.appearance.shadow_enabled = true;
  span.appearance.shadow_rgb = 0x123456;
  span.appearance.keyline_shadow = true;
  span.appearance.scale_basis = 12000;
  request.appearance_spans = &span;
  request.appearance_span_count = 1;
  request.flags &= ~kArTextRasterFlag_WrapWords;
  request.minimum_font_pixels = 12;
  request.maximum_width = 180;
  if (Raster(rasterizer, &request, &bitmap)) {
    CHECK(bitmap.width <= 180 && bitmap.font_pixels < request.font_pixels);
    bool shadow = false;
    for (int y = 0; y < bitmap.height; ++y)
      for (int x = 0; x < bitmap.width; ++x) {
        const uint32_t pixel = Pixel(&bitmap, x, y);
        shadow |= (pixel & 255) && (pixel >> 8) == 0x123456;
      }
    CHECK(shadow);
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
}

static void TestPreferredBreak(const ArTextRasterizer *rasterizer) {
  ArTextRasterRequest request = Request("small LARGE rest");
  uint8_t breaks[AR_TEXT_BOUNDARY_BYTES(64)] = {0};
  ArTextBoundary_Set(breaks, 5, true);
  request.preferred_line_breaks = breaks;
  request.preferred_line_break_capacity = 64;
  ArTextAppearanceSpan span = {.start = 6, .end = 11, .appearance = kWhite};
  span.appearance.scale_basis = 12000;
  request.appearance_spans = &span;
  request.appearance_span_count = 1;
  ArTextBitmap preferred = {0}, explicit = {0};
  Raster(rasterizer, &request, &preferred);
  request.utf8 = "small\nLARGE rest";
  request.preferred_line_breaks = NULL;
  request.preferred_line_break_capacity = 0;
  if (Raster(rasterizer, &request, &explicit) && preferred.pixels)
    SameGeometry(&preferred, &explicit);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &preferred);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &explicit);
}

static void TestActualFontSelections(const ArTextRasterizer *rasterizer,
                                     ArFontResourceId body,
                                     ArFontResourceId japanese) {
  ArTextRasterRequest request = Request("Hello 日本");
  ArTextAppearanceSpan span = {
      .start = 6, .end = (uint32_t)request.utf8_bytes, .appearance = kWhite};
  span.appearance.scale_basis = 20000;
  request.appearance_spans = &span;
  request.appearance_span_count = 1;
  ArTextBitmap bitmap = {0};
  if (Raster(rasterizer, &request, &bitmap)) {
    bool used_body = false, used_fallback = false;
    for (size_t i = 0; i < bitmap.font_use_count; ++i) {
      const ArTextFontUse *use = &bitmap.font_uses[i];
      CHECK(!use->missing && use->end <= request.utf8_bytes);
      used_body |=
          use->resource == body && use->start < 5 && use->font_pixels == 30;
      used_fallback |= use->resource == japanese && use->start >= 6 &&
                       use->font_pixels == 60;
    }
    CHECK(used_body && used_fallback);
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  /* Changing the role chooses its declared primary, even for Latin text that
   * the body stack also covers. Report actual selection, not scalar coverage.
   */
  request = Request("Hello");
  ArTextRunAppearance alternate = kWhite;
  strcpy(alternate.font_role, "alternate");
  request.appearance = &alternate;
  if (Raster(rasterizer, &request, &bitmap)) {
    CHECK(bitmap.font_use_count > 0);
    for (size_t i = 0; i < bitmap.font_use_count; ++i)
      CHECK(bitmap.font_uses[i].resource == japanese);
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request = Request("a\xf4\x8f\xbf\xbd");
  if (Raster(rasterizer, &request, &bitmap)) {
    bool missing = false;
    for (size_t i = 0; i < bitmap.font_use_count; ++i)
      missing |= bitmap.font_uses[i].missing && bitmap.font_uses[i].start == 1;
    CHECK(missing);
  }
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
}

int main(void) {
  ArHostFontResources store = {0};
  const ArFontResourceId body =
      ArHostFontResources_RegisterFile(&store, AR_TEST_FONT_PATH, NULL, 0);
  const ArFontResourceId japanese = ArHostFontResources_RegisterFile(
      &store, AR_TEST_JAPANESE_FONT_PATH, NULL, 0);
  const ArFontResourceId arabic = ArHostFontResources_RegisterFile(
      &store, AR_TEST_ARABIC_FONT_PATH, NULL, 0);
  const ArFontResourceId hebrew = ArHostFontResources_RegisterFile(
      &store, AR_TEST_HEBREW_FONT_PATH, NULL, 0);
  const ArFontResourceId fallbacks[] = {japanese, arabic, hebrew};
  const ArTextFontRole alternate = {.name = "alternate",
                                    .primary = japanese,
                                    .fallbacks = {body, arabic, hebrew},
                                    .fallback_count = 3};
  const ArTextBackendConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = "sample",
      .font_revision = 1,
      .primary_font = body,
      .resources = ArHostFontResources_Provider(&store),
      .fallback_fonts = fallbacks,
      .fallback_font_count = 3,
      .roles = &alternate,
      .role_count = 1,
      .cached_size_capacity = 32};
  ArSdlTextRasterizer adapter = {0};
  char error[256] = {0};
  CHECK(body && japanese && arabic && hebrew);
  CHECK(ArSdlTextRasterizer_Init(&adapter, &config, error, sizeof(error)));
  const ArTextRasterizer *rasterizer = ArSdlTextRasterizer_Get(&adapter);
  if (rasterizer) {
    TestActualFontSelections(rasterizer, body, japanese);
    TestPaintDoesNotShape(rasterizer);
    TestUniformMigration(rasterizer);
    TestMixedSizesAndRoles(rasterizer);
    TestBoundariesAndEffects(rasterizer);
    TestPreferredBreak(rasterizer);
  }
  ArSdlTextRasterizer_Destroy(&adapter);
  CHECK(ArHostFontResources_Destroy(&store));
  return failures ? 1 : 0;
}
