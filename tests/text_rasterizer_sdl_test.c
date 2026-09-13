#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "platform/sdl/text_rasterizer_sdl.h"
#include "host/font_resources.h"
#include "localization/text_boundaries.h"

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
static ArHostFontResources s_font_store;
static ArFontResourceId s_test_font;

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

static void TestPreferredLineBreaks(const ArTextRasterizer *rasterizer) {
  static const struct {
    const char *text;
    const char *resolved;
    const char *marker;
    int width;
  } cases[] = {
      {"Sir Derrick continues the adventure.",
       "Sir Derrick\ncontinues the adventure.", " continues", 300},
      {"This preceding segment has already wrapped across the window short tail.",
       "This preceding segment has already wrapped across the window short tail.",
       " short", 140},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const size_t at = (size_t)(strstr(cases[i].text, cases[i].marker) -
                               cases[i].text);
    uint8_t preferred[AR_TEXT_BOUNDARY_BYTES(128)] = {0};
    ArTextBoundary_Set(preferred, at, true);
    ArTextRasterRequest request = Request(cases[i].text);
    request.flags |= kArTextRasterFlag_IncludeRevealClusters;
    request.maximum_width = cases[i].width;
    request.maximum_height = 400;
    request.preferred_line_breaks = preferred;
    request.preferred_line_break_capacity = 128;
    ArTextBitmap actual = {0}, reference = {0};
    char error[256] = {0};
    CHECK(ArTextRasterizer_Rasterize(
        rasterizer, &request, &actual, NULL, error, sizeof(error)));
    request.utf8 = cases[i].resolved;
    request.utf8_bytes = strlen(cases[i].resolved);
    request.preferred_line_breaks = NULL;
    request.preferred_line_break_capacity = 0;
    CHECK(ArTextRasterizer_Rasterize(
        rasterizer, &request, &reference, NULL, error, sizeof(error)));
    if (actual.pixels && reference.pixels)
      CHECK(BitmapHash(&actual) == BitmapHash(&reference));
    if (actual.pixels) ArTextRasterizer_ReleaseBitmap(rasterizer, &actual);
    if (reference.pixels)
      ArTextRasterizer_ReleaseBitmap(rasterizer, &reference);
  }
}

static uint32_t Pixel(const ArTextBitmap *bitmap, int x, int y) {
  uint32_t pixel = 0;
  if (x >= 0 && y >= 0 && x < bitmap->width && y < bitmap->height)
    memcpy(&pixel, (const uint8_t *)bitmap->pixels +
        (size_t)y * bitmap->pitch_bytes + (size_t)x * 4, 4);
  return pixel;
}

static int ClusterX(const ArTextBitmap *bitmap, size_t end) {
  for (size_t i = 0; i < bitmap->reveal_cluster_count; ++i)
    if (bitmap->reveal_clusters[i].end_utf8_byte == end)
      return bitmap->reveal_clusters[i].x;
  return -1;
}

static int ClusterLine(const ArTextBitmap *bitmap, size_t end) {
  for (size_t i = 0; i < bitmap->reveal_cluster_count; ++i)
    if (bitmap->reveal_clusters[i].end_utf8_byte == end)
      return bitmap->reveal_clusters[i].line_index;
  return -1;
}

/* Direction is a paragraph base, not permission to reverse digits/Latin or
 * Arabic in an LTR sentence. These expectations do not depend on pixel hashes
 * or the implementation's own bidi resolver. */
static void TestBidiOrder(const ArTextRasterizer *rasterizer) {
  static const char *const samples[] = {
      "القوة 123", "مرحبا ABC", "Video ABC 123", "Sir مرحبا 123",
      "مرحبا ABC 123", "مرحبا \xe2\x81\xa8" "ABC" "\xe2\x81\xa9 123",
      "مرحبا\nABC 123",
      "שלום ABC 123", "فارسی ABC 123", "اردو ABC 123",
  };
  for (int direction = kArTextDirection_Auto;
       direction <= kArTextDirection_RightToLeft; ++direction) {
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
      ArTextRasterRequest request = Request(samples[i]);
      request.direction = (ArTextDirection)direction;
      request.flags = kArTextRasterFlag_WrapWords |
          kArTextRasterFlag_PreserveHardBreaks | kArTextRasterFlag_IncludeRevealClusters;
      request.maximum_width = 600;
      ArTextBitmap bitmap = {0};
      char error[256] = {0};
      const bool ok = ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap,
                                                NULL, error, sizeof(error));
      CHECK(ok);
      if (!ok) continue;
      CHECK(bitmap.paragraph_direction == (direction == kArTextDirection_Auto
          ? ((i == 2 || i == 3) ? kArTextDirection_LeftToRight : kArTextDirection_RightToLeft)
          : (ArTextDirection)direction));
      size_t previous = 0;
      for (size_t c = 0; c < bitmap.reveal_cluster_count; ++c) {
        CHECK(bitmap.reveal_clusters[c].end_utf8_byte > previous);
        previous = bitmap.reveal_clusters[c].end_utf8_byte;
      }
      const char *latin[] = {strstr(samples[i], "123"), strstr(samples[i], "ABC")};
      for (size_t j = 0; j < 2; ++j) {
        if (!latin[j]) continue;
        const size_t start = (size_t)(latin[j] - samples[i]);
        CHECK(ClusterX(&bitmap, start + 1) >= 0);
        CHECK(ClusterX(&bitmap, start + 1) < ClusterX(&bitmap, start + 2));
        CHECK(ClusterX(&bitmap, start + 2) < ClusterX(&bitmap, start + 3));
      }
      const char *arabic = strstr(samples[i], "مرحبا");
      if (arabic) {
        const size_t start = (size_t)(arabic - samples[i]);
        CHECK(ClusterX(&bitmap, start + 2) > ClusterX(&bitmap, start + 4));
        CHECK(ClusterX(&bitmap, start + 4) > ClusterX(&bitmap, start + 6));
      }
      const char *hebrew = strstr(samples[i], "שלום");
      if (hebrew) {
        const size_t start = (size_t)(hebrew - samples[i]);
        CHECK(ClusterX(&bitmap, start + 2) > ClusterX(&bitmap, start + 4));
        CHECK(ClusterX(&bitmap, start + 4) > ClusterX(&bitmap, start + 6));
        CHECK(ClusterX(&bitmap, start + 6) > ClusterX(&bitmap, start + 8));
      }
      ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
    }
  }
}

static void TestValueIsolation(const ArTextRasterizer *rasterizer) {
  static const struct { const char *text, *marked, *value; ArTextDirection direction; } cases[] = {
    {"مرحبا A-B 123!", "مرحبا \xe2\x81\xa8" "A-B" "\xe2\x81\xa9 123!", "A-B", kArTextDirection_Auto},
    {"مرحبا -123.45!", "مرحبا \xe2\x81\xa6" "-123.45" "\xe2\x81\xa9!", "-123.45", kArTextDirection_LeftToRight},
    {"Sir (مرحبا), 42", "Sir (\xe2\x81\xa8مرحبا\xe2\x81\xa9), 42", "مرحبا", kArTextDirection_Auto},
    {"Sir (שָׁלוֹם), 42", "Sir (\xe2\x81\xa8שָׁלוֹם\xe2\x81\xa9), 42", "שָׁלוֹם", kArTextDirection_Auto},
    {"Sir فارسی, 42", "Sir \xe2\x81\xa8فارسی\xe2\x81\xa9, 42", "فارسی", kArTextDirection_Auto},
    {"Sir اردو, 42", "Sir \xe2\x81\xa8اردو\xe2\x81\xa9, 42", "اردو", kArTextDirection_Auto},
    {"Sir ABC\nDEF!", "Sir \xe2\x81\xa8" "ABC\xe2\x81\xa9\n\xe2\x81\xa8" "DEF\xe2\x81\xa9!", "ABC\nDEF", kArTextDirection_Auto},
    /* Inserted text cannot close its host isolate or leave a nested isolate
     * open across the value boundary. Only the private layout copy changes. */
    {"مرحبا A\xe2\x81\xa9-B!", "مرحبا \xe2\x81\xa8" "A\xe2\x81\xa0-B\xe2\x81\xa9!", "A\xe2\x81\xa9-B", kArTextDirection_Auto},
    {"Sir A\xe2\x81\xa7مرحبا!", "Sir \xe2\x81\xa8" "A\xe2\x81\xa7مرحبا\xe2\x81\xa9\xe2\x81\xa9!", "A\xe2\x81\xa7مرحبا", kArTextDirection_Auto},
    {"Sir A\xe2\x81\xa7مرحبا\xe2\x81\xa9-B!", "Sir \xe2\x81\xa8" "A\xe2\x81\xa7مرحبا\xe2\x81\xa9-B\xe2\x81\xa9!", "A\xe2\x81\xa7مرحبا\xe2\x81\xa9-B", kArTextDirection_Auto},
  };
  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c)
  for (int direction = kArTextDirection_Auto; direction <= kArTextDirection_RightToLeft; ++direction)
  for (int width = 100; width <= 500; width += 200) {
    ArTextRasterRequest request = Request(cases[c].text);
    const size_t start = (size_t)(strstr(cases[c].text, cases[c].value) - cases[c].text);
    ArTextBidiSpan span = {(uint32_t)start, (uint32_t)(start+strlen(cases[c].value)), cases[c].direction};
    request.bidi_spans = &span; request.bidi_span_count = 1;
    request.direction = direction;
    request.flags |= kArTextRasterFlag_IncludeRevealClusters | kArTextRasterFlag_SlantAsciiNumerals;
    request.shadow_enabled = true;
    request.maximum_width = width; request.maximum_height = 600;
    ArTextBitmap actual, reference;
    char error[256] = {0};
    bool ok = ArTextRasterizer_Rasterize(rasterizer, &request, &actual, NULL, error, sizeof(error));
    CHECK(ok);
    if (!ok) { fprintf(stderr,"value isolation: %s\n",error); continue; }
    ArTextRasterRequest manual = request;
    manual.utf8 = cases[c].marked; manual.utf8_bytes = strlen(manual.utf8);
    manual.bidi_spans = NULL; manual.bidi_span_count = 0;
    ok = ArTextRasterizer_Rasterize(rasterizer, &manual, &reference, NULL, error, sizeof(error));
    CHECK(ok);
    if (ok) {
      CHECK(BitmapHash(&actual) == BitmapHash(&reference));
      if (BitmapHash(&actual) != BitmapHash(&reference)) fprintf(stderr,
          "isolation pixels c=%zu d=%d w=%d size=%dx%d vs %dx%d\n",c,direction,width,
          actual.width,actual.height,reference.width,reference.height);
      ArTextRasterizer_ReleaseBitmap(rasterizer,&reference);
    }
    else fprintf(stderr,"manual isolate c=%zu d=%d w=%d: %s\n",c,direction,width,error);
    size_t last = 0;
    for (size_t i = 0; i < actual.reveal_cluster_count; ++i) {
      const ArTextRevealCluster *cluster = &actual.reveal_clusters[i];
      CHECK(cluster->end_utf8_byte > last && cluster->end_utf8_byte <= request.utf8_bytes);
      last = cluster->end_utf8_byte;
    }
    CHECK(last == request.utf8_bytes);
    ArTextRasterizer_ReleaseBitmap(rasterizer,&actual);
  }
  /* A fitted table cell can borrow a source span crossing its view boundary. */
  ArTextBidiSpan spans[] = {{0,3,kArTextDirection_Auto},{3,6,kArTextDirection_LeftToRight}};
  ArTextRasterRequest request = Request("A12");
  request.flags = kArTextRasterFlag_IncludeRevealClusters;
  request.bidi_spans = spans; request.bidi_span_count = 2; request.bidi_source_offset = 2;
  request.direction = kArTextDirection_RightToLeft;
  ArTextBitmap bitmap; char error[256];
  bool ok = ArTextRasterizer_Rasterize(rasterizer,&request,&bitmap,NULL,error,sizeof(error));
  CHECK(ok);
  if (ok) {
    CHECK(ClusterX(&bitmap,2) < ClusterX(&bitmap,3));
    CHECK(bitmap.reveal_cluster_count == 3);
    ArTextRasterizer_ReleaseBitmap(rasterizer,&bitmap);
  }
}

static void TestBidiSeparators(const ArTextRasterizer *rasterizer) {
  const char *separators[] = {"\n", "\r", "\r\n", "\xc2\x85", "\xe2\x80\xa8", "\xe2\x80\xa9"};
  const char *prefixes[] = {"مرحبا", "ABC"};
  for (size_t p = 0; p < 2; ++p)
  for (size_t s = 0; s < sizeof(separators)/sizeof(separators[0]); ++s)
  for (int d = kArTextDirection_Auto; d <= kArTextDirection_RightToLeft; ++d) {
    char text[128], manual[128], error[256];
    snprintf(text, sizeof(text), "%s%sXYZ 123", prefixes[p], separators[s]);
    snprintf(manual, sizeof(manual), "%s\nXYZ 123", prefixes[p]);
    ArTextRasterRequest request = Request(text);
    request.direction = d; request.maximum_width = 500;
    request.flags |= kArTextRasterFlag_IncludeRevealClusters;
    ArTextBitmap actual, reference;
    bool ok = ArTextRasterizer_Rasterize(rasterizer, &request, &actual, NULL, error, sizeof(error));
    CHECK(ok); if (!ok) continue;
    request.utf8 = manual; request.utf8_bytes = strlen(manual);
    /* Compare separator handling within the run-layout backend, not hinting
     * differences in SDL_ttf's ordinary ASCII multiline fast path. A source
     * range outside this sliced view adds no isolate or changed text. */
    const ArTextBidiSpan outside = {200, 201, kArTextDirection_Auto};
    if (s) { request.bidi_spans = &outside; request.bidi_span_count = 1; }
    /* U+2028 is a line break, not a new bidi paragraph: retain its base. */
    if (s == 4 && d == kArTextDirection_Auto)
      request.direction = p ? kArTextDirection_LeftToRight : kArTextDirection_RightToLeft;
    ok = ArTextRasterizer_Rasterize(rasterizer, &request, &reference, NULL, error, sizeof(error));
    CHECK(ok);
    if (ok) {
      CHECK(BitmapHash(&actual) == BitmapHash(&reference));
      if (BitmapHash(&actual) != BitmapHash(&reference)) fprintf(stderr, "separator p=%zu s=%zu d=%d: %dx%d vs %dx%d\n", p, s, d, actual.width, actual.height, reference.width, reference.height);
      ArTextRasterizer_ReleaseBitmap(rasterizer, &reference);
    }
    CHECK(actual.reveal_cluster_count > 0);
    if (actual.reveal_cluster_count) {
      const ArTextRevealCluster *last = &actual.reveal_clusters[actual.reveal_cluster_count-1];
      CHECK(last->line_index == 1 && last->end_utf8_byte == strlen(text));
    }
    ArTextRasterizer_ReleaseBitmap(rasterizer, &actual);
  }
}

static void TestBidiLines(const ArTextRasterizer *rasterizer) {
  char error[256];
  for (int alignment = kArTextHorizontalAlignment_Leading;
       alignment <= kArTextHorizontalAlignment_Right; ++alignment) {
    ArTextRasterRequest request = Request("مرحبا بكم\nمرحبا");
    request.flags = kArTextRasterFlag_WrapWords |
        kArTextRasterFlag_PreserveHardBreaks | kArTextRasterFlag_IncludeRevealClusters;
    request.direction = kArTextDirection_RightToLeft;
    request.alignment = (ArTextHorizontalAlignment)alignment;
    request.maximum_width = 350;
    ArTextBitmap bitmap = {0};
    bool ok = ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error));
    CHECK(ok);
    if (!ok) continue;
    int left[2] = {350, 350}, right[2] = {0, 0};
    for (size_t i = 0; i < bitmap.reveal_cluster_count; ++i) {
      const ArTextRevealCluster *c = &bitmap.reveal_clusters[i];
      CHECK(c->line_index < 2);
      if (c->line_index >= 2) continue;
      if (c->x < left[c->line_index]) left[c->line_index] = c->x;
      if (c->x + c->width > right[c->line_index]) right[c->line_index] = c->x + c->width;
    }
    for (int line = 0; line < 2; ++line) {
      if (alignment == kArTextHorizontalAlignment_Leading || alignment == kArTextHorizontalAlignment_Right)
        CHECK(abs(right[line] - 350) <= 3);
      else if (alignment == kArTextHorizontalAlignment_Trailing || alignment == kArTextHorizontalAlignment_Left)
        CHECK(left[line] <= 3);
      else CHECK(abs(left[line] - (350 - right[line])) <= 3);
    }
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  }
  for (int width = 100; width <= 200; width += 25) {
    ArTextRasterRequest request = Request("مرحبا ABC 123 مرحبا ABC 123 مرحبا");
    request.flags = kArTextRasterFlag_WrapWords |
        kArTextRasterFlag_PreserveHardBreaks | kArTextRasterFlag_IncludeRevealClusters;
    request.direction = kArTextDirection_RightToLeft;
    request.maximum_width = width;
    request.maximum_height = 600;
    ArTextBitmap bitmap = {0};
    bool ok = ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error));
    CHECK(ok);
    if (!ok) continue;
    int previous_line = 0;
    size_t previous_end = 0;
    for (size_t i = 0; i < bitmap.reveal_cluster_count; ++i) {
      const ArTextRevealCluster *c = &bitmap.reveal_clusters[i];
      CHECK(c->end_utf8_byte > previous_end);
      CHECK(c->line_index >= previous_line);
      CHECK(c->x >= 0 && c->x + c->width <= width);
      previous_line = c->line_index;
      previous_end = c->end_utf8_byte;
    }
    CHECK(previous_line > 0);
    CHECK(previous_end == request.utf8_bytes);
    for (const char *digits = strstr(request.utf8, "123"); digits;
         digits = strstr(digits + 3, "123")) {
      size_t at = (size_t)(digits - request.utf8);
      CHECK(ClusterX(&bitmap, at + 1) < ClusterX(&bitmap, at + 2));
      CHECK(ClusterX(&bitmap, at + 2) < ClusterX(&bitmap, at + 3));
    }
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  }
  /* RTL word wrapping recognizes breakable Unicode spacing, rather than
   * splitting the following Latin word as if the spacing were a glyph. */
  ArTextRasterRequest spaced = Request("مرحبا\xe2\x80\x83" "ABCDEFGHIJ");
  spaced.flags |= kArTextRasterFlag_IncludeRevealClusters;
  spaced.direction = kArTextDirection_RightToLeft;
  spaced.maximum_width = 110;
  spaced.maximum_height = 600;
  ArTextBitmap spaced_bitmap = {0};
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &spaced, &spaced_bitmap,
                                    NULL, error, sizeof(error)));
  if (spaced_bitmap.pixels) {
    const size_t latin_first_end = strlen("مرحبا\xe2\x80\x83") + 1u;
    CHECK(ClusterLine(&spaced_bitmap, latin_first_end) >= 1);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &spaced_bitmap);
  }
}

/* Compare against an independent, unclipped effect footprint. Every face and
 * shadow pixel must have exactly one shaped owner, including AA overhangs,
 * ligatures, accents and RTL text. Cropping must retain that contract. */
static void TestEffectOwnership(const ArTextRasterizer *rasterizer) {
  static const char *const samples[] = {
      "Fillmore", "Égj", "j", "0123456789/", "M g", "E\xcc\x81quipe",
      "office", "日本語", "العربية",
  };
  const int sizes[] = {8, 16, 24, 32, 40};
  char error[256];
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
      for (int italic = 0; italic <= 1; ++italic) {
        ArTextRasterRequest request = Request(samples[i]);
        request.font_pixels = request.minimum_font_pixels = sizes[s];
        request.maximum_width = 2048;
        request.maximum_height = 512;
        request.flags = kArTextRasterFlag_IncludeRevealClusters |
            (italic ? kArTextRasterFlag_Italic : 0u);
        request.direction = kArTextDirection_Auto;
        ArTextBitmap plain;
        if (!ArTextRasterizer_Rasterize(rasterizer, &request, &plain, NULL,
                                        error, sizeof(error))) {
          CHECK(false);
          continue;
        }
        const int step = (sizes[s] + 4) / 8 > 0 ? (sizes[s] + 4) / 8 : 1;
        request.shadow_enabled = true;
        for (int shape = kArTextShadow_Diagonal; shape <= kArTextShadow_Keyline; ++shape) {
          request.shadow_shape = (ArTextShadowShape)shape;
          for (int crop = 0; crop <= 1; ++crop) {
            if (crop) request.flags |= kArTextRasterFlag_CropHorizontalWhitespace |
                                       kArTextRasterFlag_CropVerticalWhitespace;
            ArTextBitmap styled;
            const bool ok = ArTextRasterizer_Rasterize(rasterizer, &request,
                &styled, NULL, error, sizeof(error));
            CHECK(ok);
            if (!ok) continue;
            CHECK(styled.pixel_owners != NULL);
            if (!crop) {
              CHECK(styled.width == plain.width + step);
              CHECK(styled.height == plain.height + step);
              CHECK(styled.ascent == plain.ascent);
              CHECK(styled.line_advance == plain.line_advance);
            }
            for (int y = 0; y < styled.height; ++y)
              for (int x = 0; x < styled.width; ++x) {
                const uint32_t pixel = Pixel(&styled, x, y);
                const uint32_t owner = styled.pixel_owners[(size_t)y * styled.width + x];
                CHECK((owner != 0) == ((pixel & 255) != 0));
                CHECK(owner <= styled.reveal_cluster_count);
                if (crop) continue;
                uint32_t expected = Pixel(&plain, x, y);
                if (!(expected & 255)) {
                  uint32_t alpha = Pixel(&plain, x - step,
                      shape == kArTextShadow_Diagonal ? y - step : y) & 255;
                  if (shape == kArTextShadow_Keyline) {
                    const uint32_t below = Pixel(&plain, x, y - step) & 255;
                    if (below > alpha) alpha = below;
                  }
                  expected = alpha;
                }
                CHECK((pixel & 255) == (expected & 255));
                if (expected & 255) CHECK(pixel == expected);
              }
            ArTextRasterizer_ReleaseBitmap(rasterizer, &styled);
          }
          request.flags &= ~(kArTextRasterFlag_CropHorizontalWhitespace |
                             kArTextRasterFlag_CropVerticalWhitespace);
        }
        ArTextRasterizer_ReleaseBitmap(rasterizer, &plain);
      }
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

static void TestNumeralStyling(const ArTextRasterizer *rasterizer) {
  const char *const samples[] = {"ACT 1", "Étage 9", "日本 2", "العربية 3", "A 1\xcc\x81"};
  char error[256];
  for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    ArTextRasterRequest request = Request(samples[i]);
    request.direction = kArTextDirection_Auto;
    request.flags = kArTextRasterFlag_IncludeRevealClusters;
    request.maximum_width = 2048;
    ArTextBitmap plain, slanted;
    CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &plain, NULL, error, sizeof(error)));
    request.flags |= kArTextRasterFlag_SlantAsciiNumerals;
    CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &slanted, NULL, error, sizeof(error)));
    CHECK(plain.reveal_cluster_count == slanted.reveal_cluster_count);
    CHECK(plain.ascent == slanted.ascent && plain.line_advance == slanted.line_advance);
    bool moved = false;
    for (size_t c = 0; c < plain.reveal_cluster_count; ++c) {
      CHECK(!memcmp(&plain.reveal_clusters[c], &slanted.reveal_clusters[c], sizeof(ArTextRevealCluster)));
      const bool number = i != 4 && plain.reveal_clusters[c].end_utf8_byte == request.utf8_bytes;
      int bottom = -1;
      for (int y = 0; y < plain.height; ++y)
        for (int x = 0; x < plain.width; ++x)
          if (plain.pixel_owners[(size_t)y * plain.width + x] == c + 1) bottom = y;
      for (int y = 0; y < plain.height; ++y)
        for (int x = 0; x < plain.width; ++x) {
          if (plain.pixel_owners[(size_t)y * plain.width + x] != c + 1) continue;
          const int shift = number ? (bottom - y + 2) / 4 : 0;
          if (shift) moved = true;
          CHECK(Pixel(&plain, x, y) == Pixel(&slanted, x + shift, y));
          CHECK(slanted.pixel_owners[(size_t)y * slanted.width + x + shift] == c + 1);
        }
    }
    CHECK(moved == (i != 4)); /* A digit+accent cluster is not split. */
    ArTextRasterizer_ReleaseBitmap(rasterizer, &plain);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &slanted);
    request.flags |= kArTextRasterFlag_Italic;
    CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &slanted, NULL, error, sizeof(error)));
    request.flags &= ~kArTextRasterFlag_SlantAsciiNumerals;
    CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &plain, NULL, error, sizeof(error)));
    CHECK(BitmapHash(&plain) == BitmapHash(&slanted)); /* Never double slant. */
    ArTextRasterizer_ReleaseBitmap(rasterizer, &plain);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &slanted);
  }
}

static void TestRasterization(void) {
  ArSdlTextRasterizer adapter = {0};
  char error[256];
  const ArFontResourceId fallback_fonts[] = {
    ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_JAPANESE_FONT_PATH, error, sizeof(error)),
    ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_ARABIC_FONT_PATH, error, sizeof(error)),
    ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_HEBREW_FONT_PATH, error, sizeof(error)),
  };
  const ArSdlTextRasterizerConfig config = {
    .struct_size = sizeof(ArSdlTextRasterizerConfig),
    .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
    .font_stack_id = "actraiser-default",
    .resources = ArHostFontResources_Provider(&s_font_store),
    .primary_font = s_test_font,
    .fallback_fonts = fallback_fonts,
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
  TestEffectOwnership(rasterizer);
  TestNumeralStyling(rasterizer);
  TestPreferredLineBreaks(rasterizer);

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
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
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

  /* Accented initials retain the complete shaped cluster, with all remaining
   * ink unchanged. The portable color pass also works with RTL/ligatures. */
  request = Request("- E\xcc\x81quipe -");
  request.style_id = kArTextStyle_PlainWhite;
  request.accent_end_utf8_byte = 5;
  request.accent_rgb = 0xff9400;
  CHECK(ArTextRasterizer_Rasterize(rasterizer,&request,&bitmap,NULL,error,sizeof(error)));
  bool gold = false, white = false;
  for (int y=0;y<bitmap.height;++y) for (int x=0;x<bitmap.width;++x) {
    uint32_t pixel;
    memcpy(&pixel,(const uint8_t *)bitmap.pixels+y*bitmap.pitch_bytes+x*4,4);
    if (!(pixel&255)) continue;
    gold |= pixel>>8 == 0xff9400;
    white |= pixel>>8 == 0xffffff;
  }
  CHECK(gold && white);
  ArTextRasterizer_ReleaseBitmap(rasterizer,&bitmap);
  request.accent_end_utf8_byte=4; // Inside the combining scalar.
  CHECK(!ArTextRasterRequest_IsValid(&request));
  uint32_t pixels_accent[] = {0xffffff80u,0xffffff40u,0xffffff20u};
  const ArTextRevealCluster accent_clusters[] = {{8,0,0,0,1,1},{5,0,1,0,1,1},{2,0,2,0,1,1}};
  CHECK(ArTextBitmap_ApplyClusterAccent(pixels_accent,3,1,12,
      kArRenderPixelFormat_Rgba8888,accent_clusters,3,4,0x123456));
  CHECK(pixels_accent[0]==0xffffff80u && pixels_accent[1]==0x12345640u && pixels_accent[2]==0xffffff20u);

  /* HUD gold uses the live palette; italic digits cannot contaminate a
   * subsequent upright request sharing the same cached font size. */
  request = Request("012345");
  request.style_id = kArTextStyle_RetailPaletteBands;
  request.band_rgb = 0xe08000;
  request.body_rgb = 0xffff80;
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  const uint64_t upright_hash = BitmapHash(&bitmap);
  /* Banded ink is a reviewed appearance; pin it so moving the formula behind
   * the portable contract cannot quietly change a pixel. */
  CHECK(request.style_id != kArTextStyle_RetailPaletteBands ||
        upright_hash == UINT64_C(0x3924da22796386bc));
  uint8_t r, g, b;
  CHECK(BottomClusterInk(&bitmap, 1, NULL, &r, &g, &b));
  CHECK(r == 0xe0 && g == 0x80 && b == 0);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request.flags |= kArTextRasterFlag_Italic;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(BitmapHash(&bitmap) != upright_hash);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request.flags &= ~kArTextRasterFlag_Italic;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(BitmapHash(&bitmap) == upright_hash);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* Tight menu rows fit visible ink, including accents/descenders, without
   * reducing the font just to accommodate transparent typographic padding. */
  request = Request("Égj");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  ArTextBitmap padded;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &padded, NULL, error, sizeof(error)));
  request.flags |= kArTextRasterFlag_CropVerticalWhitespace;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  const int removed_top = padded.ascent - bitmap.ascent;
  CHECK(removed_top >= 0 && bitmap.height < padded.height);
  CHECK(bitmap.width == padded.width && bitmap.line_advance == padded.line_advance);
  CHECK(bitmap.reveal_cluster_count == padded.reveal_cluster_count);
  for (int y = 0; y < padded.height; ++y) {
    const uint8_t *row = (const uint8_t *)padded.pixels + y * padded.pitch_bytes;
    if (y >= removed_top && y < removed_top + bitmap.height) {
      CHECK(!memcmp(row, (const uint8_t *)bitmap.pixels +
          (y - removed_top) * bitmap.pitch_bytes, (size_t)bitmap.width * 4));
    } else {
      for (int x = 0; x < padded.width; ++x) {
        uint32_t pixel;
        uint8_t alpha;
        memcpy(&pixel, row + x * 4, sizeof(pixel));
        SDL_GetRGBA(pixel, details, NULL, NULL, NULL, NULL, &alpha);
        CHECK(alpha == 0);
      }
    }
  }
  request.maximum_height = bitmap.height;
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &padded);
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* A user-selected accessibility size may make a long translated heading
   * wrap beyond its retail-height box. The backend chooses the largest size
   * that fits, but never below the caller's authored-size floor. */
  request = Request("Combattre les monstres");
  request.font_pixels = 40;
  request.minimum_font_pixels = 20;
  request.maximum_width = 240;
  request.maximum_height = 60;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(bitmap.width <= request.maximum_width);
  CHECK(bitmap.height <= request.maximum_height);
  CHECK(bitmap.line_advance >= request.minimum_font_pixels);
  CHECK(bitmap.line_advance < request.font_pixels * 2);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.minimum_font_pixels = request.font_pixels;
  request.maximum_height = 1;
  CHECK(!ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(strstr(error, "minimum font size") != NULL);

  /* The retail SNES font has one blue/white/blue palette cell per 8x8 tile,
   * not one shared vertical gradient per typographic line. Verify that a
   * baseline capital and a descender each finish in the exact lower blue even
   * though their visible bottoms occupy different output rows. */
  request = Request("Mg");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
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
    CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
    CHECK(bitmap.width > 0 && bitmap.width <= request.maximum_width);
    CHECK(bitmap.height > 0 && bitmap.height <= request.maximum_height);
    CHECK(bitmap.height >=
          bitmap.line_advance * status_fixtures[index].minimum_line_advances);
    ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  }

  request = Request("Ame\xCC\x81lie et crème brûlée");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
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
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  const uint64_t japanese_fallback_hash = BitmapHash(&bitmap);
  CHECK(bitmap.width > 0 && bitmap.height > 0);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request = Request("か\xE3\x82\x99"); /* decomposed U+304B + U+3099 */
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  request.language_bcp47 = "ja-JP";
  request.language_bcp47_bytes = strlen("ja-JP");
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
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
    .resources = ArHostFontResources_Provider(&s_font_store),
    .primary_font = s_test_font,
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
  bool provided = false;
  CHECK(ArTextRasterizer_HasGlyph(rasterizer, 0x65e5, &provided, error,
                                  sizeof(error)));
  CHECK(provided); /* Ordered Japanese fallback, without rasterizing. */
  CHECK(ArTextRasterizer_HasGlyph(primary_rasterizer, 0x65e5, &provided, error,
                                  sizeof(error)));
  CHECK(!provided);
  CHECK(ArTextRasterizer_HasGlyph(rasterizer, 0x0627, &provided, error,
                                  sizeof(error)));
  CHECK(provided); /* Arabic fallback. */
  CHECK(ArTextRasterizer_HasGlyph(rasterizer, 0x0301, &provided, error,
                                  sizeof(error)));
  CHECK(provided); /* Real combining marks must be checked. */
  CHECK(ArTextGlyphNeedsCoverage(0x0301) && ArTextGlyphNeedsCoverage(' '));
  CHECK(!ArTextGlyphNeedsCoverage(0x200d) && !ArTextGlyphNeedsCoverage(0xfe0f));
  CHECK(!ArTextGlyphNeedsCoverage(0xe0100) &&
        !ArTextGlyphNeedsCoverage(0xfffc));
  CHECK(!ArTextGlyphNeedsCoverage('\n') && !ArTextGlyphNeedsCoverage(0x034f));
  CHECK(ArTextRasterizer_HasGlyph(primary_rasterizer, 0x200d, &provided, error,
                                  sizeof(error)) &&
        provided);
  CHECK(ArTextRasterizer_HasGlyph(rasterizer, 0x10ffff, &provided, error,
                                  sizeof(error)) &&
        !provided);
  CHECK(!ArTextRasterizer_HasGlyph(rasterizer, 0xd800, &provided, error,
                                   sizeof(error)) &&
        !provided);
  CHECK(!ArTextRasterizer_HasGlyph(rasterizer, 0x110000, &provided, error,
                                   sizeof(error)));
  /* Appended optional ABI fields must never be read from an older provider. */
  ArTextRasterizerOps old_ops = *rasterizer->ops;
  old_ops.struct_size = offsetof(ArTextRasterizerOps, has_glyph);
  ArTextRasterizer old = *rasterizer;
  old.ops = &old_ops;
  CHECK(ArTextRasterizer_IsReady(&old));
  CHECK(!ArTextRasterizer_HasGlyph(&old, 'A', &provided, error, sizeof(error)));
  if (ArTextRasterizer_Rasterize(
          primary_rasterizer, &request, &bitmap, NULL, error,
          sizeof(error))) {
    CHECK(BitmapHash(&bitmap) != japanese_fallback_hash);
    ArTextRasterizer_ReleaseBitmap(primary_rasterizer, &bitmap);
  } else {
    CHECK(error[0] != 0);
  }
  ArSdlTextRasterizer_Destroy(&primary_only);

  TestBidiOrder(rasterizer);
  TestValueIsolation(rasterizer);
  TestBidiSeparators(rasterizer);
  TestBidiLines(rasterizer);

  /* Valid directional controls can shape to no ink. That is a permanent
   * property of this request, not a transient allocation failure. */
  request = Request("\xE2\x81\xA6\xE2\x81\xA9");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  ArTextRasterFailure no_ink_failure = kArTextRasterFailure_None;
  CHECK(!ArTextRasterizer_Rasterize(
      rasterizer, &request, &bitmap, &no_ink_failure, error, sizeof(error)));
  CHECK(no_ink_failure == kArTextRasterFailure_Deterministic);
  CHECK(strstr(error, "no rasterizable ink") != NULL);

  /* The complete word is shaped through the Arabic fallback. UAX #9 need
   * not change its visual order merely because the base direction changes:
   * European digits following Arabic resolve as a nested numeric run. */
  request = Request("القوة 123");
  request.flags |= kArTextRasterFlag_IncludeRevealClusters;
  request.direction = kArTextDirection_RightToLeft;
  request.language_bcp47 = "ar";
  request.language_bcp47_bytes = 2;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(bitmap.reveal_cluster_count > 0);
  bool saw_complete_rtl_run = false;
  for (size_t index = 0; index < bitmap.reveal_cluster_count; ++index)
    if (bitmap.reveal_clusters[index].end_utf8_byte == request.utf8_bytes)
      saw_complete_rtl_run = true;
  CHECK(saw_complete_rtl_run);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.direction = kArTextDirection_LeftToRight;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(ClusterX(&bitmap, request.utf8_bytes - 2) <
        ClusterX(&bitmap, request.utf8_bytes - 1));
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  /* Auto bidi must leave a fallback English paragraph readable inside an RTL
   * locale. The overlay uses this while independently right-aligning its box. */
  request = Request("Video");
  request.direction = kArTextDirection_LeftToRight;
  request.language_bcp47 = "ar";
  request.language_bcp47_bytes = 2;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  const uint64_t latin_ltr_hash = BitmapHash(&bitmap);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
  request.direction = kArTextDirection_Auto;
  CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(BitmapHash(&bitmap) == latin_ltr_hash);
  ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);

  request.font_stack_id = "unknown";
  request.font_stack_id_bytes = strlen("unknown");
  CHECK(!ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(strstr(error, "font stack") != NULL);

  request = Request("Texte trop grand");
  request.maximum_height = 1;
  CHECK(!ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(strstr(error, "bounds") != NULL);

  ArSdlTextRasterizer_Destroy(&adapter);
  CHECK(ArSdlTextRasterizer_Get(&adapter) == NULL);
}

typedef struct CountingResources {
  ArFontResources source;
  unsigned acquires, releases;
} CountingResources;

static bool CountAcquire(void *context, ArFontResourceId id,
                          ArFontResourceData *data, char *error, size_t capacity) {
  CountingResources *counts = context;
  ++counts->acquires;
  return counts->source.ops->acquire(counts->source.context, id, data, error, capacity);
}

static void CountRelease(void *context, ArFontResourceData *data) {
  CountingResources *counts = context;
  ++counts->releases;
  counts->source.ops->release(counts->source.context, data);
}

static void TestFontSnapshotLifetime(void) {
  static const ArFontResourceOps ops = {
      .struct_size = sizeof(ops), .abi_version = AR_FONT_RESOURCE_ABI_VERSION,
      .acquire = CountAcquire, .release = CountRelease};
  CountingResources counts = {.source = ArHostFontResources_Provider(&s_font_store)};
  ArFontResourceLease source = {0};
  char error[256];
  CHECK(ArFontResource_Acquire(&source, &counts.source, s_test_font, error, sizeof(error)));
  if (!source.data.bytes) return;
  FILE *file = fopen(AR_TEST_SNAPSHOT_FONT_PATH, "wb");
  CHECK(file);
  if (!file) { ArFontResource_Release(&source); return; }
  CHECK(fwrite(source.data.bytes, 1, source.data.size, file) == source.data.size);
  CHECK(!fclose(file));
  ArFontResource_Release(&source);
  const ArFontResourceId id = ArHostFontResources_RegisterFile(
      &s_font_store, AR_TEST_SNAPSHOT_FONT_PATH, error, sizeof(error));
  CHECK(id);
  ArSdlTextRasterizerConfig config = {
      .struct_size = sizeof(config), .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
      .font_stack_id = "snapshot", .resources = {&ops, &counts},
      .primary_font = id, .font_revision = 1, .cached_size_capacity = 1,
  };
  ArSdlTextRasterizer adapter = {0};
  CHECK(ArSdlTextRasterizer_Init(&adapter, &config, error, sizeof(error)));
  void *const original = adapter.implementation;
  CHECK(original && counts.acquires == 1 && !counts.releases);
  const ArFontResourceId invalid[] = {UINT64_MAX};
  config.fallback_fonts = invalid;
  config.fallback_font_count = 1;
  CHECK(!ArSdlTextRasterizer_Init(&adapter, &config, error, sizeof(error)));
  CHECK(adapter.implementation == original && counts.acquires == 3 && counts.releases == 1);

  /* No TTF font size has been opened yet. Replacing/removing the source and
   * retiring registration cannot change these already-acquired bytes. */
  file = fopen(AR_TEST_SNAPSHOT_FONT_PATH, "wb");
  CHECK(file);
  if (file) { CHECK(fputs("not a font", file) >= 0); CHECK(!fclose(file)); }
  CHECK(!remove(AR_TEST_SNAPSHOT_FONT_PATH));
  ArHostFontResources_Retire(&s_font_store, id);
  CHECK(!ArHostFontResources_Destroy(&s_font_store));
  for (int pass = 0; pass < 2; ++pass) {
    for (int size = 16; size <= 64; size += 16) {
      ArTextRasterRequest request = Request("Élise Mgj");
      request.font_stack_id = "snapshot";
      request.font_stack_id_bytes = strlen(request.font_stack_id);
      request.font_revision = 1;
      request.font_pixels = request.minimum_font_pixels = size;
      request.maximum_width = request.maximum_height = 1024;
      ArTextBitmap bitmap = {0};
      CHECK(ArTextRasterizer_Rasterize(ArSdlTextRasterizer_Get(&adapter),
          &request, &bitmap, NULL, error, sizeof(error)));
      CHECK(bitmap.pixels && bitmap.width > 0 && bitmap.height > 0);
      ArTextRasterizer_ReleaseBitmap(ArSdlTextRasterizer_Get(&adapter), &bitmap);
    }
  }
  CHECK(counts.acquires == 3 && counts.releases == 1); /* No per-size I/O or leases. */
  ArSdlTextRasterizer_Destroy(&adapter);
  ArSdlTextRasterizer_Destroy(&adapter);
  CHECK(counts.releases == 2);
}

static void TestMissingGlyphWarnings(void) {
  /* Capture stderr from a separate process: exercise the actual rendering
   * path, duplicate suppression, cap, and recreation without exposing backend
   * bookkeeping through a test-only production API. */
  for (int stack = 0; stack < 2; ++stack) {
    ArSdlTextRasterizer adapter = {0};
    const ArSdlTextRasterizerConfig config = {
        .struct_size = sizeof(config),
        .abi_version = AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION,
        .font_stack_id = "actraiser-default",
        .resources = ArHostFontResources_Provider(&s_font_store),
        .primary_font = s_test_font,
        .font_revision = 7,
        .cached_size_capacity = 1,
    };
    char error[256];
    CHECK(ArSdlTextRasterizer_Init(&adapter, &config, error, sizeof(error)));
    const ArTextRasterizer *rasterizer = ArSdlTextRasterizer_Get(&adapter);
    for (uint32_t scalar = 0xf0000; scalar < 0xf0043; ++scalar) {
      char text[8] = {0};
      SDL_UCS4ToUTF8(scalar, text);
      ArTextRasterRequest request = Request(text);
      for (int repeat = 0; repeat < 2; ++repeat) {
        ArTextBitmap bitmap = {0};
        CHECK(ArTextRasterizer_Rasterize(rasterizer, &request, &bitmap, NULL,
                                         error, sizeof(error)));
        ArTextRasterizer_ReleaseBitmap(rasterizer, &bitmap);
      }
    }
    ArSdlTextRasterizer_Destroy(&adapter);
  }
}

/* The shadow pass is pure pixel work with two hazards worth pinning: it must
 * read original coverage (or one shadow pixel seeds the next and the run
 * smears sideways), and it must never paint over a letterform. */
static void TestStyleShadow(void) {
  enum { kWidth = 6, kHeight = 3 };
  uint32_t pixels[kHeight][kWidth];
  memset(pixels, 0, sizeof(pixels));
  /* Two adjacent ink pixels, opaque white. */
  pixels[1][1] = UINT32_C(0xffffffff);
  pixels[1][2] = UINT32_C(0xffffffff);

  CHECK(ArTextBitmap_ApplyStyleShadow(
      pixels, kWidth, kHeight, kWidth * 4, kArRenderPixelFormat_Rgba8888,
      1, 1, UINT32_C(0x000000)));
  /* Shadow lands one right and one down of each ink pixel. */
  CHECK((pixels[2][2] & UINT32_C(0xff)) == 0xffu);
  CHECK((pixels[2][3] & UINT32_C(0xff)) == 0xffu);
  CHECK((pixels[2][2] >> 8) == 0u); /* black, not white */
  /* The letterform is untouched, and its own shadow did not cast another. */
  CHECK(pixels[1][1] == UINT32_C(0xffffffff));
  CHECK(pixels[1][2] == UINT32_C(0xffffffff));
  CHECK((pixels[2][4] & UINT32_C(0xff)) == 0u);
  CHECK((pixels[0][0] & UINT32_C(0xff)) == 0u);

  /* Rejected inputs leave the caller's pixels alone. */
  uint32_t guard[kHeight][kWidth];
  memcpy(guard, pixels, sizeof(guard));
  CHECK(!ArTextBitmap_ApplyStyleShadow(
      pixels, kWidth, kHeight, kWidth * 4, kArRenderPixelFormat_Rgba8888,
      0, 0, 0));
  CHECK(!ArTextBitmap_ApplyStyleShadow(
      NULL, kWidth, kHeight, kWidth * 4, kArRenderPixelFormat_Rgba8888,
      1, 1, 0));
  CHECK(!memcmp(guard, pixels, sizeof(guard)));
  /* An offset past the surface succeeds and changes nothing. */
  CHECK(ArTextBitmap_ApplyStyleShadow(
      pixels, kWidth, kHeight, kWidth * 4, kArRenderPixelFormat_Rgba8888,
      kWidth, 0, 0));
  CHECK(!memcmp(guard, pixels, sizeof(guard)));
}

static void TestStyleShadowTraversalMatchesSnapshot(void) {
  enum { kWidth = 7, kHeight = 5, kStride = 9 };
  uint32_t original[kHeight][kStride];
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kStride; ++x)
      original[y][x] = x >= kWidth ? UINT32_C(0xdeadbeef)
          : (x + y * 3) % 4 == 0 ? UINT32_C(0xffffff80)
          : (x + y) % 3 == 0 ? UINT32_C(0xffccffff) : 0;
  for (int dy = -kHeight; dy <= kHeight; ++dy) {
    for (int dx = -kWidth; dx <= kWidth; ++dx) {
      if (!dx && !dy) continue;
      uint32_t expected[kHeight][kStride], actual[kHeight][kStride];
      memcpy(expected, original, sizeof(expected));
      memcpy(actual, original, sizeof(actual));
      for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
          const int sx = x - dx, sy = y - dy;
          if ((original[y][x] & 255u) || sx < 0 || sx >= kWidth ||
              sy < 0 || sy >= kHeight) continue;
          const uint32_t alpha = original[sy][sx] & 255u;
          if (alpha) expected[y][x] = UINT32_C(0x12345600) | alpha;
        }
      }
      CHECK(ArTextBitmap_ApplyStyleShadow(actual, kWidth, kHeight,
          kStride * 4, kArRenderPixelFormat_Rgba8888, dx, dy, 0x123456));
      CHECK(!memcmp(expected, actual, sizeof(expected)));
    }
  }
}

int main(int argc, char **argv) {
  s_test_font = ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_FONT_PATH, NULL, 0);
  CHECK(s_test_font);
  if (!s_test_font) return 1;
  TestStyleShadow();
  TestStyleShadowTraversalMatchesSnapshot();
  if (argc == 2 && !strcmp(argv[1], "--missing-glyph-warnings"))
    TestMissingGlyphWarnings();
  else {
    TestRasterization();
    TestFontSnapshotLifetime();
  }
  CHECK(ArHostFontResources_Destroy(&s_font_store));
  if (g_failures) {
    fprintf(stderr, "%d SDL text rasterizer test(s) failed\n", g_failures);
    return 1;
  }
  puts("SDL text rasterizer tests passed");
  return 0;
}
