#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "platform/sdl/text_rasterizer_sdl.h"

#ifndef AR_TEST_FONT_PATH
#error AR_TEST_FONT_PATH must identify the bundled test font
#endif
#ifndef AR_TEST_JAPANESE_FONT_PATH
#error AR_TEST_JAPANESE_FONT_PATH must identify the Japanese fallback font
#endif
#ifndef AR_TEST_ARABIC_FONT_PATH
#error AR_TEST_ARABIC_FONT_PATH must identify the Arabic fallback font
#endif

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n",                            \
            __FILE__, __LINE__, #condition);                                 \
    ++g_failures;                                                            \
  }                                                                          \
} while (0)

static ArTextRasterRequest Request(const char *text) {
  return (ArTextRasterRequest){
    .struct_size = sizeof(ArTextRasterRequest),
    .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
    .utf8 = text,
    .utf8_bytes = strlen(text),
    .font_stack_id = "actraiser-default",
    .font_stack_id_bytes = strlen("actraiser-default"),
    .source_revision = 1,
    .font_revision = 7,
    .style_id = kArTextStyle_RetailBlueWhiteBands,
    .flags = kArTextRasterFlag_WrapWords |
             kArTextRasterFlag_PreserveHardBreaks |
             kArTextRasterFlag_CropHorizontalWhitespace,
    .direction = kArTextDirection_LeftToRight,
    .alignment = kArTextHorizontalAlignment_Leading,
    .font_pixels = 24,
    .minimum_font_pixels = 24,
    .maximum_width = 220,
    .maximum_height = 160,
    .filter = kArRenderFilter_Linear,
    .language_bcp47 = "fr",
    .language_bcp47_bytes = 2,
  };
}

static uint64_t BitmapHash(const ArTextBitmap *bitmap) {
  uint64_t hash = UINT64_C(1469598103934665603);
  const uint8_t *pixels = (const uint8_t *)bitmap->pixels;
  for (int y = 0; y < bitmap->height; ++y) {
    for (int x = 0; x < bitmap->width * 4; ++x) {
      hash ^= pixels[(size_t)y * bitmap->pitch_bytes + (size_t)x];
      hash *= UINT64_C(1099511628211);
    }
  }
  hash ^= (uint64_t)(uint32_t)bitmap->width;
  hash *= UINT64_C(1099511628211);
  hash ^= (uint64_t)(uint32_t)bitmap->height;
  return hash;
}

static bool BottomClusterInk(
    const ArTextBitmap *bitmap, size_t end_utf8_byte,
    int *bottom_y, uint8_t *red, uint8_t *green, uint8_t *blue) {
  if (!bitmap || !bitmap->pixels || !bitmap->reveal_clusters) return false;
  const ArTextRevealCluster *owner = NULL;
  for (size_t index = 0; index < bitmap->reveal_cluster_count; ++index) {
    if (bitmap->reveal_clusters[index].end_utf8_byte == end_utf8_byte) {
      owner = &bitmap->reveal_clusters[index];
      break;
    }
  }
  if (!owner) return false;
  const SDL_PixelFormatDetails *details =
      SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
  if (!details) return false;
  const uint8_t *pixels = (const uint8_t *)bitmap->pixels;
  for (int y = owner->y + owner->height - 1; y >= owner->y; --y) {
    for (int x = owner->x; x < owner->x + owner->width; ++x) {
      uint32_t pixel;
      uint8_t alpha;
      memcpy(&pixel, pixels + (size_t)y * bitmap->pitch_bytes +
             (size_t)x * 4u, sizeof(pixel));
      SDL_GetRGBA(pixel, details, NULL, red, green, blue, &alpha);
      if (!alpha) continue;
      if (bottom_y) *bottom_y = y;
      return true;
    }
  }
  return false;
}

static void TestRasterization(void) {
  ArSdlTextRasterizer adapter = {0};
  char error[256];
  const char *const fallback_fonts[] = {
    AR_TEST_JAPANESE_FONT_PATH,
    AR_TEST_ARABIC_FONT_PATH,
  };
  const ArSdlTextRasterizerConfig config = {
    .struct_size = sizeof(ArSdlTextRasterizerConfig),
    .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
    .font_stack_id = "actraiser-default",
    .primary_font_path = AR_TEST_FONT_PATH,
    .fallback_font_paths = fallback_fonts,
    .fallback_font_count =
        sizeof(fallback_fonts) / sizeof(fallback_fonts[0]),
    .font_revision = 7,
    .cached_size_capacity = 3,
  };
  CHECK(ArSdlTextRasterizer_Init(
      &adapter, &config, error, sizeof(error)));
  const ArTextRasterizer *rasterizer =
      ArSdlTextRasterizer_Get(&adapter);
  CHECK(rasterizer != NULL);

  /* Pin the fixture's reason for existing: neither non-Latin script is in
   * the base face, and its ordered fallback really does provide it. */
  TTF_Font *primary = TTF_OpenFont(AR_TEST_FONT_PATH, 24.0f);
  TTF_Font *japanese = TTF_OpenFont(AR_TEST_JAPANESE_FONT_PATH, 24.0f);
  TTF_Font *arabic = TTF_OpenFont(AR_TEST_ARABIC_FONT_PATH, 24.0f);
  CHECK(primary != NULL && japanese != NULL && arabic != NULL);
  if (primary && japanese && arabic) {
    CHECK(!TTF_FontHasGlyph(primary, 0x65E5)); /* 日 */
    CHECK(TTF_FontHasGlyph(japanese, 0x65E5));
    CHECK(!TTF_FontHasGlyph(primary, 0x0639)); /* ع */
    CHECK(TTF_FontHasGlyph(arabic, 0x0639));
  }
  TTF_CloseFont(primary);
  TTF_CloseFont(japanese);
  TTF_CloseFont(arabic);

  ArTextRasterRequest request = Request(
      "Crème brûlée — Élévation\nUtiliser un don");
  ArTextBitmap bitmap;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(bitmap.format == kArRenderPixelFormat_Rgba8888);
  CHECK(bitmap.width > 0 && bitmap.width <= request.maximum_width);
  CHECK(bitmap.height > 0 && bitmap.height <= request.maximum_height);
  CHECK(bitmap.pitch_bytes >= bitmap.width * 4);
  CHECK(bitmap.ascent > 0 && bitmap.line_advance > 0);

  bool saw_blue = false;
  bool saw_white = false;
  const SDL_PixelFormatDetails *details =
      SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA8888);
  CHECK(details != NULL);
  const uint8_t *pixels = (const uint8_t *)bitmap.pixels;
  for (int y = 0; y < bitmap.height; ++y) {
    for (int x = 0; x < bitmap.width; ++x) {
      uint32_t pixel;
      uint8_t red, green, blue, alpha;
      memcpy(&pixel, pixels + (size_t)y * bitmap.pitch_bytes +
             (size_t)x * 4u, sizeof(pixel));
      SDL_GetRGBA(pixel, details, NULL, &red, &green, &blue, &alpha);
      if (!alpha) continue;
      if (red < 220 && green < 235 && blue > 240)
        saw_blue = true;
      if (red > 245 && green > 245 && blue > 245)
        saw_white = true;
    }
  }
  CHECK(saw_blue && saw_white);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* A user-selected accessibility size may make a long translated heading
   * wrap beyond its retail-height box. The backend chooses the largest size
   * that fits, but never below the caller's authored-size floor. */
  request = Request("Combattre les monstres");
  request.font_pixels = 40;
  request.minimum_font_pixels = 20;
  request.maximum_width = 240;
  request.maximum_height = 60;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(bitmap.width <= request.maximum_width);
  CHECK(bitmap.height <= request.maximum_height);
  CHECK(bitmap.line_advance >= request.minimum_font_pixels);
  CHECK(bitmap.line_advance < request.font_pixels * 2);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.minimum_font_pixels = request.font_pixels;
  request.maximum_height = 1;
  CHECK(!ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(strstr(error, "minimum font size") != NULL);

  /* The retail SNES font has one blue/white/blue palette cell per 8x8 tile,
   * not one shared vertical gradient per typographic line. Verify that a
   * baseline capital and a descender each finish in the exact lower blue even
   * though their visible bottoms occupy different output rows. */
  request = Request("Mg");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  int capital_bottom = -1;
  int descender_bottom = -1;
  uint8_t capital_red = 0, capital_green = 0, capital_blue = 0;
  uint8_t descender_red = 0, descender_green = 0, descender_blue = 0;
  CHECK(BottomClusterInk(
      &bitmap, 1u, &capital_bottom,
      &capital_red, &capital_green, &capital_blue));
  CHECK(BottomClusterInk(
      &bitmap, 2u, &descender_bottom,
      &descender_red, &descender_green, &descender_blue));
  CHECK(capital_bottom < descender_bottom);
  CHECK(capital_red == 156 && capital_green == 206 && capital_blue == 255);
  CHECK(descender_red == capital_red &&
        descender_green == capital_green &&
        descender_blue == capital_blue);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* Release-sized, independently authored status fixtures use the exact
   * logical ownership bounds from the 4x Sky Palace layout. They guard the
   * complete seven-line tables and narrow Master report at shaped-pixel level,
   * complementing scalar warnings and live replay screenshots. */
  static const struct {
    const char *text;
    int font_pixels;
    int maximum_width;
    int maximum_height;
    int minimum_line_advances;
  } status_fixtures[] = {
    {
      "Villes — population 3032\n"
      "VILLE ÉTOILE: 922, Maximale, N3, O0\n"
      "LAC POURPRE: 514, Normale, N3, O1\n"
      "DÉSERT D'OR: 776, Normale, N3, O0\n"
      "MONT AZUR: 362, Normale, N3, O1\n"
      "ÎLE VERTE: 456, Lente, N3, O1\n"
      "MUR DU NORD: 2, Lente, N1, O0",
      28, 832, 640, 6,
    },
    {
      "Score total : 98850\n"
      "VILLE ÉTOILE : A1 10440 • A2 14670\n"
      "LAC POURPRE : A1 11090 • A2 14500\n"
      "DÉSERT D'OR : A1 0 • A2 14540\n"
      "MONT AZUR : A1 13020 • A2 0\n"
      "ÎLE VERTE : A1 10080 • A2 10510\n"
      "MUR DU NORD : A1 0 • A2 0",
      28, 832, 640, 6,
    },
    {
      "Derrick — 9 vies\n"
      "Niveau 17 • PV 24\n"
      "SP 170 • PM 10\n"
      "Prochain niveau : 3300\n"
      "Population totale : 3032",
      24, 384, 352, 4,
    },
  };
  for (size_t index = 0;
       index < sizeof(status_fixtures) / sizeof(status_fixtures[0]);
       ++index) {
    request = Request(status_fixtures[index].text);
    request.font_pixels = status_fixtures[index].font_pixels;
    request.minimum_font_pixels = status_fixtures[index].font_pixels;
    request.maximum_width = status_fixtures[index].maximum_width;
    request.maximum_height = status_fixtures[index].maximum_height;
    CHECK(ArTextRasterizer_Rasterize(
        rasterizer, &request, &bitmap, error, sizeof(error)));
    CHECK(bitmap.width > 0 && bitmap.width <= request.maximum_width);
    CHECK(bitmap.height > 0 && bitmap.height <= request.maximum_height);
    CHECK(bitmap.height >=
          bitmap.line_advance * status_fixtures[index].minimum_line_advances);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  }

  request = Request("Ame\xCC\x81lie et crème brûlée");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(bitmap.reveal_clusters != NULL);
  CHECK(bitmap.reveal_cluster_count > 0);
  bool saw_complete_text = false;
  bool saw_naked_combining_base = false;
  for (size_t index = 0; index < bitmap.reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &bitmap.reveal_clusters[index];
    CHECK(cluster->x >= 0 && cluster->y >= 0);
    CHECK(cluster->x + cluster->width <= bitmap.width);
    CHECK(cluster->y + cluster->height <= bitmap.height);
    if (cluster->end_utf8_byte == request.utf8_bytes)
      saw_complete_text = true;
    /* byte 3 is immediately after the bare 'e'; the following U+0301 must
     * remain in the same reveal cluster. */
    if (cluster->end_utf8_byte == 3u)
      saw_naked_combining_base = true;
  }
  CHECK(saw_complete_text);
  CHECK(!saw_naked_combining_base);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* A Japanese page must come from the ordered fallback, not a host font or
   * the primary face's missing-glyph box. */
  request = Request("天空城 — 日本語のメニュー");
  request.language_bcp47 = "ja-JP";
  request.language_bcp47_bytes = strlen("ja-JP");
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  const uint64_t japanese_fallback_hash = BitmapHash(&bitmap);
  CHECK(bitmap.width > 0 && bitmap.height > 0);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request = Request("か\xE3\x82\x99"); /* decomposed U+304B + U+3099 */
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  request.language_bcp47 = "ja-JP";
  request.language_bcp47_bytes = strlen("ja-JP");
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  bool split_japanese_dakuten = false;
  bool saw_complete_japanese = false;
  for (size_t index = 0; index < bitmap.reveal_cluster_count; ++index) {
    if (bitmap.reveal_clusters[index].end_utf8_byte == 3u)
      split_japanese_dakuten = true;
    if (bitmap.reveal_clusters[index].end_utf8_byte == request.utf8_bytes)
      saw_complete_japanese = true;
  }
  CHECK(saw_complete_japanese);
  CHECK(!split_japanese_dakuten);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  ArSdlTextRasterizer primary_only = {0};
  const ArSdlTextRasterizerConfig primary_only_config = {
    .struct_size = sizeof(ArSdlTextRasterizerConfig),
    .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
    .font_stack_id = "primary-only",
    .primary_font_path = AR_TEST_FONT_PATH,
    .font_revision = 8,
    .cached_size_capacity = 1,
  };
  CHECK(ArSdlTextRasterizer_Init(
      &primary_only, &primary_only_config, error, sizeof(error)));
  request.font_stack_id = "primary-only";
  request.font_stack_id_bytes = strlen("primary-only");
  request.font_revision = 8;
  const ArTextRasterizer *primary_rasterizer =
      ArSdlTextRasterizer_Get(&primary_only);
  if (ArTextRasterizer_Rasterize(
          primary_rasterizer, &request, &bitmap, error, sizeof(error))) {
    CHECK(BitmapHash(&bitmap) != japanese_fallback_hash);
    ArTextRasterizer_ReleaseBitmap(primary_rasterizer, &bitmap);
  } else {
    CHECK(error[0] != 0);
  }
  ArSdlTextRasterizer_Destroy(&primary_only);

  /* Mixed Arabic and European digits changes visual order when paragraph
   * direction changes. The complete word is shaped by HarfBuzz through the
   * Arabic fallback; the renderer never assembles per-codepoint glyphs. */
  request = Request("القوة 123");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  request.direction = kArTextDirection_RightToLeft;
  request.language_bcp47 = "ar";
  request.language_bcp47_bytes = 2;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  const uint64_t rtl_hash = BitmapHash(&bitmap);
  CHECK(bitmap.reveal_cluster_count > 0);
  bool saw_complete_rtl_run = false;
  for (size_t index = 0; index < bitmap.reveal_cluster_count; ++index)
    if (bitmap.reveal_clusters[index].end_utf8_byte == request.utf8_bytes)
      saw_complete_rtl_run = true;
  CHECK(saw_complete_rtl_run);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.direction = kArTextDirection_LeftToRight;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(BitmapHash(&bitmap) != rtl_hash);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* Auto bidi must leave a fallback English paragraph readable inside an RTL
   * locale. The overlay uses this while independently right-aligning its box. */
  request = Request("Video");
  request.direction = kArTextDirection_LeftToRight;
  request.language_bcp47 = "ar";
  request.language_bcp47_bytes = 2;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  const uint64_t latin_ltr_hash = BitmapHash(&bitmap);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request.direction = kArTextDirection_Auto;
  CHECK(ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(BitmapHash(&bitmap) == latin_ltr_hash);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.font_stack_id = "unknown";
  request.font_stack_id_bytes = strlen("unknown");
  CHECK(!ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(strstr(error, "font stack") != NULL);

  request = Request("Texte trop grand");
  request.maximum_height = 1;
  CHECK(!ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, error, sizeof(error)));
  CHECK(strstr(error, "bounds") != NULL);

  ArSdlTextRasterizer_Destroy(&adapter);
  CHECK(ArSdlTextRasterizer_Get(&adapter) == NULL);
}

int main(void) {
  TestRasterization();
  if (g_failures) {
    fprintf(stderr, "%d SDL text rasterizer test(s) failed\n", g_failures);
    return 1;
  }
  puts("SDL text rasterizer tests passed");
  return 0;
}
