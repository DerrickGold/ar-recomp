#include "localization/text_rasterizer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define AR_MEMBER_END(type, member) \
  (offsetof(type, member) + sizeof(((type *)0)->member))

static bool ValidOps(const ArTextRasterizerOps *ops) {
  return ops &&
      ops->struct_size >= AR_MEMBER_END(
          ArTextRasterizerOps, release_bitmap) &&
      ops->abi_version == AR_TEXT_RASTERIZER_ABI_VERSION &&
      ops->rasterize && ops->release_bitmap;
}

bool ArTextRasterizer_Init(ArTextRasterizer *rasterizer,
                           const ArTextRasterizerOps *ops,
                           void *context,
                           uint64_t implementation_revision) {
  if (!rasterizer || !ValidOps(ops) || !implementation_revision) return false;
  *rasterizer = (ArTextRasterizer){
    .ops = ops,
    .context = context,
    .implementation_revision = implementation_revision,
  };
  return true;
}

void ArTextRasterizer_Reset(ArTextRasterizer *rasterizer) {
  if (rasterizer) memset(rasterizer, 0, sizeof(*rasterizer));
}

bool ArTextRasterizer_IsReady(const ArTextRasterizer *rasterizer) {
  return rasterizer && ValidOps(rasterizer->ops) &&
      rasterizer->implementation_revision != 0;
}

static bool ValidUtf8Buffer(const char *text, size_t size) {
  if (!text || !size) return false;
  size_t index = 0;
  while (index < size) {
    const uint8_t first = (uint8_t)text[index++];
    if (!first) return false;
    if (first < 0x80u) continue;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    unsigned continuation_count = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
      codepoint = first & 0x1fu;
      minimum = 0x80u;
      continuation_count = 1;
    } else if (first >= 0xe0u && first <= 0xefu) {
      codepoint = first & 0x0fu;
      minimum = 0x800u;
      continuation_count = 2;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      codepoint = first & 0x07u;
      minimum = 0x10000u;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > size - index) return false;
    for (unsigned i = 0; i < continuation_count; ++i) {
      const uint8_t next = (uint8_t)text[index++];
      if ((next & 0xc0u) != 0x80u) return false;
      codepoint = (codepoint << 6) | (next & 0x3fu);
    }
    if (codepoint < minimum || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu))
      return false;
  }
  return true;
}

static bool ValidLanguage(const char *language, size_t size) {
  if (!language) return size == 0;
  if (size == 0 || size > 63 || memchr(language, 0, size)) return false;
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = (unsigned char)language[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return true;
}

bool ArTextRasterRequest_IsValid(const ArTextRasterRequest *request) {
  const ArTextRasterFlags known_flags =
      kArTextRasterFlag_WrapWords |
      kArTextRasterFlag_PreserveHardBreaks |
      kArTextRasterFlag_CropHorizontalWhitespace |
      kArTextRasterFlag_CropVerticalWhitespace |
      kArTextRasterFlag_Italic |
      kArTextRasterFlag_SlantAsciiNumerals |
      kArTextRasterFlag_IncludeRevealClusters;
  return request &&
      request->struct_size >= AR_MEMBER_END(
          ArTextRasterRequest, preferred_line_break_source_offset) &&
      request->abi_version == AR_TEXT_RASTER_REQUEST_ABI_VERSION &&
      ValidUtf8Buffer(request->utf8, request->utf8_bytes) &&
      ArTextBidiSpans_Valid(request->bidi_spans, request->bidi_span_count,
          request->utf8, request->utf8_bytes, request->bidi_source_offset) &&
      request->accent_end_utf8_byte <= request->utf8_bytes &&
      (!request->accent_end_utf8_byte ||
       request->accent_end_utf8_byte == request->utf8_bytes ||
       ((uint8_t)request->utf8[request->accent_end_utf8_byte] & 0xc0u) != 0x80u) &&
      request->font_stack_id && request->font_stack_id_bytes > 0 &&
      !memchr(request->font_stack_id, 0, request->font_stack_id_bytes) &&
      request->font_revision != 0 &&
      request->shadow_shape >= kArTextShadow_Diagonal &&
      request->shadow_shape <= kArTextShadow_Keyline &&
      request->font_pixels > 0 && request->font_pixels <= 4096 &&
      request->minimum_font_pixels > 0 &&
      request->minimum_font_pixels <= request->font_pixels &&
      request->maximum_width > 0 &&
      request->maximum_height > 0 &&
      request->direction >= kArTextDirection_Auto &&
      request->direction <= kArTextDirection_RightToLeft &&
      request->alignment >= kArTextHorizontalAlignment_Leading &&
      request->alignment <= kArTextHorizontalAlignment_Right &&
      ((!request->preferred_line_breaks &&
        request->preferred_line_break_capacity == 0 &&
        request->preferred_line_break_source_offset == 0) ||
       (request->preferred_line_breaks &&
        request->preferred_line_break_source_offset <=
            request->preferred_line_break_capacity &&
        request->utf8_bytes <= request->preferred_line_break_capacity -
            request->preferred_line_break_source_offset &&
        (request->flags & (kArTextRasterFlag_WrapWords |
                           kArTextRasterFlag_PreserveHardBreaks)) ==
            (kArTextRasterFlag_WrapWords |
             kArTextRasterFlag_PreserveHardBreaks))) &&
      (request->flags & ~known_flags) == 0 &&
      (request->filter == kArRenderFilter_Nearest ||
       request->filter == kArRenderFilter_Linear) &&
      request->pixelation >= kArTextPixelation_None &&
      request->pixelation <= kArTextPixelation_Mosaic &&
      ((request->pixelation == kArTextPixelation_None &&
        (request->pixelation_size == 0 ||
         request->pixelation_size == 1)) ||
       (request->pixelation != kArTextPixelation_None &&
        request->pixelation_size >= 2 &&
        request->pixelation_size <= 8)) &&
      ValidLanguage(request->language_bcp47,
                    request->language_bcp47_bytes);
}

static void SetError(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
}

bool ArTextGlyphNeedsCoverage(uint32_t scalar) {
  /* Unicode 17.0.0 DerivedCoreProperties.txt: Default_Ignorable_Code_Point.
   * See third_party/unicode/NOTICE. Keep real combining marks queryable.
   * U+FFFC is the renderer's typed inline-object placeholder, not font art. */
  static const uint32_t ranges[][2] = {
      {0x00ad, 0x00ad},   {0x034f, 0x034f},   {0x061c, 0x061c},
      {0x115f, 0x1160},   {0x17b4, 0x17b5},   {0x180b, 0x180f},
      {0x200b, 0x200f},   {0x202a, 0x202e},   {0x2060, 0x206f},
      {0x3164, 0x3164},   {0xfe00, 0xfe0f},   {0xfeff, 0xfeff},
      {0xffa0, 0xffa0},   {0xfff0, 0xfff8},   {0x1bca0, 0x1bca3},
      {0x1d173, 0x1d17a}, {0xe0000, 0xe0fff},
  };
  if (scalar == '\n' || scalar == '\r' || scalar == '\t' || scalar == 0x85 ||
      scalar == 0x2028 || scalar == 0x2029 || scalar == 0xfffc)
    return false;
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
    if (scalar < ranges[i][0])
      break;
    if (scalar <= ranges[i][1])
      return false;
  }
  return true;
}

bool ArTextRasterizer_HasGlyph(const ArTextRasterizer *rasterizer,
                               uint32_t scalar, bool *provided, char *error,
                               size_t error_capacity) {
  if (error && error_capacity)
    error[0] = 0;
  if (provided)
    *provided = false;
  if (!provided || scalar > 0x10ffff ||
      (scalar >= 0xd800 && scalar <= 0xdfff) ||
      !ArTextRasterizer_IsReady(rasterizer)) {
    SetError(error, error_capacity, "invalid glyph coverage query");
    return false;
  }
  if (rasterizer->ops->struct_size <
          AR_MEMBER_END(ArTextRasterizerOps, has_glyph) ||
      !rasterizer->ops->has_glyph) {
    SetError(error, error_capacity,
             "glyph coverage is unavailable on this backend");
    return false;
  }
  if (!ArTextGlyphNeedsCoverage(scalar)) {
    *provided = true;
    return true;
  }
  return rasterizer->ops->has_glyph(rasterizer->context, scalar, provided,
                                    error, error_capacity);
}

static bool BitmapValid(const ArTextBitmap *bitmap,
                        const ArTextRasterRequest *request) {
  if (!bitmap ||
      bitmap->struct_size < AR_MEMBER_END(ArTextBitmap, paragraph_direction) ||
      bitmap->abi_version != AR_TEXT_BITMAP_ABI_VERSION ||
      bitmap->paragraph_direction < kArTextDirection_Auto ||
      bitmap->paragraph_direction > kArTextDirection_RightToLeft ||
      !bitmap->pixels || bitmap->width <= 0 || bitmap->height <= 0 ||
      bitmap->reveal_cluster_count > 65536 ||
      (bitmap->reveal_cluster_count && !bitmap->reveal_clusters) ||
      bitmap->width > request->maximum_width ||
      bitmap->height > request->maximum_height ||
      ((request->flags & kArTextRasterFlag_IncludeRevealClusters) &&
       (!bitmap->reveal_clusters || !bitmap->reveal_cluster_count)))
    return false;
  for (size_t index = 0; index < bitmap->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &bitmap->reveal_clusters[index];
    const size_t previous_end = index
        ? bitmap->reveal_clusters[index - 1u].end_utf8_byte : 0u;
    if (!cluster->end_utf8_byte ||
        cluster->end_utf8_byte <= previous_end ||
        cluster->end_utf8_byte > request->utf8_bytes ||
        (cluster->end_utf8_byte < request->utf8_bytes &&
         ((uint8_t)request->utf8[cluster->end_utf8_byte] & 0xc0u) == 0x80u) ||
        cluster->line_index < 0 || cluster->x < 0 || cluster->y < 0 ||
        cluster->width <= 0 || cluster->height <= 0 ||
        cluster->x > bitmap->width - cluster->width ||
        cluster->y > bitmap->height - cluster->height)
      return false;
  }
  int bytes_per_pixel = 0;
  switch (bitmap->format) {
    case kArRenderPixelFormat_Argb8888:
    case kArRenderPixelFormat_Abgr8888:
    case kArRenderPixelFormat_Rgba8888: bytes_per_pixel = 4; break;
    case kArRenderPixelFormat_Rgb565:
    case kArRenderPixelFormat_Rgba4444: bytes_per_pixel = 2; break;
    case kArRenderPixelFormat_A8: bytes_per_pixel = 1; break;
    default: return false;
  }
  if (bitmap->width > INT32_MAX / bytes_per_pixel ||
      bitmap->pitch_bytes < bitmap->width * bytes_per_pixel ||
      (size_t)bitmap->height > SIZE_MAX / (size_t)bitmap->pitch_bytes)
    return false;
  if (bitmap->pixel_owners) {
    if (bitmap->format != kArRenderPixelFormat_Rgba8888 ||
        !bitmap->reveal_cluster_count ||
        (uint64_t)bitmap->width * bitmap->height > (UINT64_C(64) << 20) / 8)
      return false;
    for (int y = 0; y < bitmap->height; ++y)
      for (int x = 0; x < bitmap->width; ++x) {
        uint32_t pixel;
        memcpy(&pixel, (const uint8_t *)bitmap->pixels +
            (size_t)y * bitmap->pitch_bytes + (size_t)x * 4, 4);
        const uint32_t owner = bitmap->pixel_owners[(size_t)y * bitmap->width + x];
        if (owner > bitmap->reveal_cluster_count ||
            (owner != 0) != ((pixel & 255) != 0))
          return false;
      }
  }
  return true;
}

ArRenderRectI ArTextBitmap_InkBounds(const ArTextBitmap *bitmap,
                                    ArRenderRectI region) {
  if (!bitmap || !bitmap->pixels || bitmap->width <= 0 || bitmap->height <= 0 ||
      region.x < 0 || region.y < 0 || region.w <= 0 || region.h <= 0 ||
      region.w > bitmap->width || region.h > bitmap->height ||
      region.x > bitmap->width - region.w ||
      region.y > bitmap->height - region.h)
    return (ArRenderRectI){0};
  int bytes = 0;
  uint32_t alpha_mask = 0;
  switch (bitmap->format) {
    case kArRenderPixelFormat_Argb8888:
    case kArRenderPixelFormat_Abgr8888:
      bytes = 4; alpha_mask = UINT32_C(0xff000000); break;
    case kArRenderPixelFormat_Rgba8888:
      bytes = 4; alpha_mask = UINT32_C(0xff); break;
    case kArRenderPixelFormat_Rgba4444:
      bytes = 2; alpha_mask = UINT32_C(0xf); break;
    case kArRenderPixelFormat_Rgb565: bytes = 2; break;
    case kArRenderPixelFormat_A8: bytes = 1; alpha_mask = UINT32_C(0xff); break;
    default: return (ArRenderRectI){0};
  }
  if (bitmap->width > INT32_MAX / bytes ||
      bitmap->pitch_bytes < bitmap->width * bytes ||
      (size_t)bitmap->height > SIZE_MAX / (size_t)bitmap->pitch_bytes)
    return (ArRenderRectI){0};
  if (!alpha_mask) return region;
  int left = region.x + region.w, right = region.x;
  int top = region.y + region.h, bottom = region.y;
  for (int y = region.y; y < region.y + region.h; ++y) {
    const uint8_t *row = (const uint8_t *)bitmap->pixels +
        (size_t)y * (size_t)bitmap->pitch_bytes;
    for (int x = region.x; x < region.x + region.w; ++x) {
      const uint8_t *source = row + (size_t)x * (size_t)bytes;
      uint32_t pixel;
      if (bytes == 4) memcpy(&pixel, source, sizeof(pixel));
      else if (bytes == 2) {
        uint16_t packed;
        memcpy(&packed, source, sizeof(packed));
        pixel = packed;
      } else pixel = *source;
      if (!(pixel & alpha_mask)) continue;
      if (x < left) left = x;
      if (x + 1 > right) right = x + 1;
      if (y < top) top = y;
      if (y + 1 > bottom) bottom = y + 1;
    }
  }
  return right > left && bottom > top
      ? (ArRenderRectI){left, top, right - left, bottom - top}
      : (ArRenderRectI){0};
}

bool ArTextRasterizer_Rasterize(const ArTextRasterizer *rasterizer,
                                const ArTextRasterRequest *request,
                                ArTextBitmap *out_bitmap,
                                ArTextRasterFailure *out_failure,
                                char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  ArTextRasterFailure failure = kArTextRasterFailure_Deterministic;
  if (out_failure) *out_failure = failure;
  if (out_bitmap) {
    memset(out_bitmap, 0, sizeof(*out_bitmap));
    out_bitmap->struct_size = sizeof(*out_bitmap);
    out_bitmap->abi_version = AR_TEXT_BITMAP_ABI_VERSION;
  }
  if (!ArTextRasterizer_IsReady(rasterizer) || !out_bitmap ||
      !ArTextRasterRequest_IsValid(request)) {
    SetError(error, error_capacity, "invalid text rasterization request");
    return false;
  }
  if (!rasterizer->ops->rasterize(rasterizer->context, request, out_bitmap,
                                  &failure, error, error_capacity)) {
    rasterizer->ops->release_bitmap(rasterizer->context, out_bitmap);
    memset(out_bitmap, 0, sizeof(*out_bitmap));
    /* A backend that reports failure without classifying it is treated as
     * deterministic: that keeps a genuinely impossible request from being
     * retried forever, and no backend loses recovery it used to have. */
    if (out_failure)
      *out_failure = failure == kArTextRasterFailure_None
          ? kArTextRasterFailure_Deterministic : failure;
    return false;
  }
  if (!BitmapValid(out_bitmap, request)) {
    rasterizer->ops->release_bitmap(rasterizer->context, out_bitmap);
    memset(out_bitmap, 0, sizeof(*out_bitmap));
    SetError(error, error_capacity,
             "text rasterizer returned an invalid bitmap");
    return false;
  }
  return true;
}

void ArTextRasterizer_ReleaseBitmap(const ArTextRasterizer *rasterizer,
                                    ArTextBitmap *bitmap) {
  if (!bitmap) return;
  if (ArTextRasterizer_IsReady(rasterizer) &&
      (bitmap->pixels || bitmap->token))
    rasterizer->ops->release_bitmap(rasterizer->context, bitmap);
  memset(bitmap, 0, sizeof(*bitmap));
}

static float RetailBlueWeight(float position) {
  if (position <= 0.22f || position >= 0.78f) return 1.0f;
  if (position < 0.36f) return (0.36f - position) / 0.14f;
  if (position > 0.64f) return (position - 0.64f) / 0.14f;
  return 0.0f;
}

static bool ApplyShadow(void *pixels, int width, int height,
                                   int pitch_bytes, ArRenderPixelFormat format,
                                   int offset_x, int offset_y,
                                   uint32_t shadow_rgb, uint32_t *owners,
                                   bool keyline) {
  if (!pixels || format != kArRenderPixelFormat_Rgba8888 || width <= 0 ||
      height <= 0 || width > INT32_MAX / 4 || pitch_bytes < width * 4 ||
      (!offset_x && !offset_y))
    return false;
  if (!keyline && (offset_x <= -width || offset_x >= width ||
      offset_y <= -height || offset_y >= height))
    return true; /* Shifted clear of the surface: nothing to lay down. */

  /* Traverse against the offset, like an overlapping memmove. Every source
   * pixel is then read before its destination can overwrite it: when Y moves,
   * its source row has not been visited; on the same row X provides the order.
   * This prevents shadows casting more shadows without a coverage allocation
   * (and therefore without a transient OOM being mistaken for invalid input). */
  const int y_step = offset_y > 0 ? -1 : 1;
  const int x_step = offset_x > 0 ? -1 : 1;
  const int y_end = y_step < 0 ? -1 : height;
  const int x_end = x_step < 0 ? -1 : width;
  const uint8_t red = (uint8_t)((shadow_rgb >> 16) & 255);
  const uint8_t green = (uint8_t)((shadow_rgb >> 8) & 255);
  const uint8_t blue = (uint8_t)(shadow_rgb & 255);
  for (int y = y_step < 0 ? height - 1 : 0; y != y_end; y += y_step) {
    /* Clip before subtraction so even extreme valid dimensions cannot
     * overflow a signed source-row index. */
    uint8_t *row = (uint8_t *)pixels + (size_t)y * (size_t)pitch_bytes;
    for (int x = x_step < 0 ? width - 1 : 0; x != x_end; x += x_step) {
      uint32_t pixel;
      memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
      if (pixel & UINT32_C(0xff)) continue; /* Never overwrite a letterform. */
      uint8_t alpha = 0;
      uint32_t owner = 0;
      for (unsigned sample = 0; sample < (keyline ? 2u : 1u); ++sample) {
        const int64_t sx = (int64_t)x - (keyline && sample ? 0 : offset_x);
        const int64_t sy = (int64_t)y - (keyline && !sample ? 0 : offset_y);
        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
        uint32_t source;
        memcpy(&source, (const uint8_t *)pixels + (size_t)sy*pitch_bytes + (size_t)sx*4, 4);
        if ((source & 255u) <= alpha) continue;
        alpha = source & 255u;
        if (owners) owner = owners[(size_t)sy*width + (size_t)sx];
      }
      if (!alpha) continue;
      pixel = ((uint32_t)red << 24) | ((uint32_t)green << 16) |
              ((uint32_t)blue << 8) | alpha;
      memcpy(row + (size_t)x * 4u, &pixel, sizeof(pixel));
      if (owners) owners[(size_t)y*width + x] = owner;
    }
  }
  return true;
}

bool ArTextBitmap_ApplyStyleShadow(void *pixels, int width, int height,
    int pitch_bytes, ArRenderPixelFormat format, int dx, int dy, uint32_t rgb) {
  return ApplyShadow(pixels, width, height, pitch_bytes, format, dx, dy, rgb, NULL, false);
}

bool ArTextBitmap_ApplyOwnedShadow(void *pixels, int width, int height,
    int pitch_bytes, uint32_t *owners, int step, uint32_t rgb,
    ArTextShadowShape shape) {
  if (step <= 0 || shape < kArTextShadow_Diagonal || shape > kArTextShadow_Keyline) return false;
  return ApplyShadow(pixels, width, height, pitch_bytes, kArRenderPixelFormat_Rgba8888,
                     step, step, rgb, owners, shape == kArTextShadow_Keyline);
}

uint32_t *ArTextBitmap_BuildOwnership(const ArTextBitmap *b) {
  if (!b || !b->pixels || b->format != kArRenderPixelFormat_Rgba8888 ||
      b->width <= 0 || b->height <= 0 || b->width > INT32_MAX/4 ||
      b->pitch_bytes < b->width*4 || !b->reveal_clusters ||
      !b->reveal_cluster_count || b->reveal_cluster_count > 65536 ||
      (uint64_t)b->width*b->height > (UINT64_C(64)<<20)/8) return NULL;
  uint32_t *owners = calloc((size_t)b->width*b->height, sizeof(*owners));
  if (!owners) return NULL;
  for (size_t i = 0; i < b->reveal_cluster_count; ++i) {
    const ArTextRevealCluster *c = &b->reveal_clusters[i];
    const int left = c->x > 0 ? c->x : 0, top = c->y > 0 ? c->y : 0;
    const int64_t right = (int64_t)c->x+c->width, bottom = (int64_t)c->y+c->height;
    for (int y = top; y < b->height && y < bottom; ++y)
      for (int x = left; x < b->width && x < right; ++x) {
        uint32_t p;
        memcpy(&p, (const uint8_t *)b->pixels+(size_t)y*b->pitch_bytes+x*4u, 4);
        /* A shared ink pixel cannot be separated after whole-run shaping.
         * Give overlaps to the later logical cluster: delaying shared ink is
         * safe, whereas the earlier owner could reveal its neighbour early. */
        if (p & 255) owners[(size_t)y * b->width + x] = (uint32_t)i + 1;
      }
  }
  /* Shapers can return advance boxes excluding an antialiased overhang.
   * Assign only those uncovered ink pixels to the closest shaped box. This
   * does not reshape/split a grapheme, and never assigns transparent padding. */
  for (int y=0; y<b->height; ++y) for (int x=0; x<b->width; ++x) {
    const size_t at=(size_t)y*b->width+x;
    if (owners[at]) continue;
    uint32_t p;
    memcpy(&p, (const uint8_t *)b->pixels+(size_t)y*b->pitch_bytes+x*4u, 4);
    if (!(p&255)) continue;
    int64_t best=INT64_MAX;
    for (size_t i=0; i<b->reveal_cluster_count; ++i) {
      const ArTextRevealCluster *c=&b->reveal_clusters[i];
      const int64_t right=(int64_t)c->x+c->width-1, bottom=(int64_t)c->y+c->height-1;
      const int64_t dx=x<c->x ? (int64_t)c->x-x : x>right ? x-right : 0;
      const int64_t dy=y<c->y ? (int64_t)c->y-y : y>bottom ? y-bottom : 0;
      const int64_t distance=dx+dy;
      if (distance <= best) {best=distance;owners[at]=(uint32_t)i+1;}
    }
  }
  return owners;
}

bool ArTextBitmap_SlantAsciiNumerals(ArTextBitmap *b,
                                    const char *utf8, size_t bytes) {
  if (!b || !utf8 || !b->pixel_owners || !b->reveal_clusters ||
      !b->reveal_cluster_count || b->reveal_cluster_count > 65536 ||
      b->format != kArRenderPixelFormat_Rgba8888 || !b->pixels || b->width <= 0 ||
      b->width > INT32_MAX / 4 || b->pitch_bytes < b->width * 4 ||
      b->height <= 0 || (uint64_t)b->width * b->height > (UINT64_C(64) << 20) / 16)
    return false;
  const size_t area = (size_t)b->width * b->height;
  int *bottom = malloc((b->reveal_cluster_count + 1u) * sizeof(*bottom));
  uint32_t *pixels = calloc(area, sizeof(*pixels));
  uint32_t *owners = calloc(area, sizeof(*owners));
  if (!bottom || !pixels || !owners) {
    free(bottom); free(pixels); free(owners);
    return false;
  }
  bottom[0] = -1;
  size_t start = 0;
  for (size_t i = 0; i < b->reveal_cluster_count; ++i) {
    const size_t end = b->reveal_clusters[i].end_utf8_byte;
    bool number = start < end && end <= bytes;
    /* A skipped hard-break substring is not part of the next ink cluster. */
    while (start < end && start < bytes &&
           (utf8[start] == '\n' || utf8[start] == '\r')) ++start;
    number &= start < end;
    for (size_t j = start; number && j < end; ++j)
      number = utf8[j] >= '0' && utf8[j] <= '9';
    bottom[i + 1u] = number ? 0 : -1;
    start = end;
  }
  for (int y = 0; y < b->height; ++y)
    for (int x = 0; x < b->width; ++x) {
      const uint32_t owner = b->pixel_owners[(size_t)y * b->width + x];
      if (owner > b->reveal_cluster_count) {
        free(bottom); free(pixels); free(owners);
        return false;
      }
      if (owner && bottom[owner] >= 0) bottom[owner] = y;
    }
  /* Preserve the upright ink first. A slanted bearing may extend behind its
   * neighbour but may not overwrite that neighbour's letterform. */
  for (int pass = 0; pass < 2; ++pass)
    for (int y = 0; y < b->height; ++y)
      for (int x = 0; x < b->width; ++x) {
        const uint32_t owner = b->pixel_owners[(size_t)y * b->width + x];
        if (!owner || (bottom[owner] >= 0) != (pass != 0)) continue;
        const int target_x = x + (pass ? (bottom[owner] - y + 2) / 4 : 0);
        if (target_x >= b->width) {
          free(bottom); free(pixels); free(owners);
          return false;
        }
        const size_t at = (size_t)y * b->width + target_x;
        uint32_t pixel;
        memcpy(&pixel, (const uint8_t *)b->pixels +
            (size_t)y * b->pitch_bytes + (size_t)x * 4, 4);
        if (owners[at] && (bottom[owners[at]] < 0 ||
                          (pixels[at] & 255) >= (pixel & 255))) continue;
        pixels[at] = pixel;
        owners[at] = owner;
      }
  for (int y = 0; y < b->height; ++y)
    memcpy((uint8_t *)b->pixels + (size_t)y * b->pitch_bytes,
           pixels + (size_t)y * b->width, (size_t)b->width * 4);
  memcpy((void *)b->pixel_owners, owners, area * sizeof(*owners));
  free(bottom); free(pixels); free(owners);
  return true;
}

bool ArTextBitmap_ApplyClusterAccent(void *pixels, int width, int height,
                                     int pitch_bytes, ArRenderPixelFormat format,
                                     const ArTextRevealCluster *clusters,
                                     size_t cluster_count, uint32_t utf8_end,
                                     uint32_t rgb) {
  if (!utf8_end) return true;
  if (!clusters || !cluster_count) return false;
  size_t selected = SIZE_MAX;
  for (size_t i = 0; i < cluster_count; ++i) {
    const size_t end = clusters[i].end_utf8_byte;
    if (end >= utf8_end && end < selected) selected = end;
  }
  if (selected == SIZE_MAX) return false;
  for (size_t i = 0; i < cluster_count; ++i)
    if (clusters[i].end_utf8_byte == selected &&
        !ArTextBitmap_ApplyStyleBands(pixels, width, height, pitch_bytes,
                                      format, &clusters[i], 1, rgb, rgb))
      return false;
  return true;
}

bool ArTextBitmap_ApplyStyleBands(void *pixels, int width, int height,
                                  int pitch_bytes, ArRenderPixelFormat format,
                                  const ArTextRevealCluster *clusters,
                                  size_t cluster_count, uint32_t band_rgb,
                                  uint32_t body_rgb) {
  if (!pixels || format != kArRenderPixelFormat_Rgba8888 || width <= 0 ||
      height <= 0 || width > INT32_MAX / 4 || pitch_bytes < width * 4 ||
      !clusters || !cluster_count)
    return false;

  /* Retail letters live in independent 8x8 tile cells. The white/blue split
   * therefore restarts for every character instead of following a shared
   * typographic baseline: the bottom of an M receives the same lower blue as
   * the descender of a g. A shaped cluster is the Unicode-safe equivalent of
   * that cell. It keeps combining marks and ligatures together while avoiding
   * naïve per-codepoint rendering for Arabic, Indic, and other joined scripts. */
  for (size_t cluster_index = 0; cluster_index < cluster_count;
       ++cluster_index) {
    const ArTextRevealCluster *cluster = &clusters[cluster_index];
    int left = cluster->x;
    int top = cluster->y;
    int right = cluster->x + cluster->width;
    int bottom = cluster->y + cluster->height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) continue;

    int first = bottom;
    int last = -1;
    for (int y = top; y < bottom; ++y) {
      const uint8_t *row =
          (const uint8_t *)pixels + (size_t)y * (size_t)pitch_bytes;
      for (int x = left; x < right; ++x) {
        uint32_t pixel;
        memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
        if (!(pixel & UINT32_C(0xff))) continue;
        if (y < first) first = y;
        if (y > last) last = y;
      }
    }
    if (last < first) continue;
    const float denominator = last > first ? (float)(last - first) : 1.0f;
    for (int y = first; y <= last; ++y) {
      const float blue = RetailBlueWeight((float)(y - first) / denominator);
      uint8_t rgb[3];
      for (unsigned c = 0; c < 3; ++c) {
        const unsigned shift = 16 - c * 8;
        const float band = (float)((band_rgb >> shift) & 255);
        const float body = (float)((body_rgb >> shift) & 255);
        rgb[c] = (uint8_t)floorf(body + blue * (band - body) + 0.5f);
      }
      uint8_t *row =
          (uint8_t *)pixels + (size_t)y * (size_t)pitch_bytes;
      for (int x = left; x < right; ++x) {
        uint32_t pixel;
        memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
        const uint32_t alpha = pixel & UINT32_C(0xff);
        if (!alpha) continue;
        pixel = ((uint32_t)rgb[0] << 24) | ((uint32_t)rgb[1] << 16) |
            ((uint32_t)rgb[2] << 8) | alpha;
        memcpy(row + (size_t)x * 4u, &pixel, sizeof(pixel));
      }
    }
  }
  return true;
}
