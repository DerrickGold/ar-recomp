#include "localization/text_rasterizer.h"

#include <stddef.h>
#include <stdio.h>
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
      kArTextRasterFlag_IncludeRevealClusters;
  return request &&
      request->struct_size >= AR_MEMBER_END(
          ArTextRasterRequest, pixelation_size) &&
      request->abi_version == AR_TEXT_RASTER_REQUEST_ABI_VERSION &&
      ValidUtf8Buffer(request->utf8, request->utf8_bytes) &&
      request->font_stack_id && request->font_stack_id_bytes > 0 &&
      !memchr(request->font_stack_id, 0, request->font_stack_id_bytes) &&
      request->font_revision != 0 &&
      request->font_pixels > 0 && request->minimum_font_pixels > 0 &&
      request->minimum_font_pixels <= request->font_pixels &&
      request->maximum_width > 0 &&
      request->maximum_height > 0 &&
      request->direction >= kArTextDirection_Auto &&
      request->direction <= kArTextDirection_RightToLeft &&
      request->alignment >= kArTextHorizontalAlignment_Leading &&
      request->alignment <= kArTextHorizontalAlignment_Trailing &&
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

static bool BitmapValid(const ArTextBitmap *bitmap,
                        const ArTextRasterRequest *request) {
  if (!bitmap ||
      bitmap->struct_size < AR_MEMBER_END(ArTextBitmap, token) ||
      bitmap->abi_version != AR_TEXT_BITMAP_ABI_VERSION ||
      !bitmap->pixels || bitmap->width <= 0 || bitmap->height <= 0 ||
      bitmap->width > request->maximum_width ||
      bitmap->height > request->maximum_height ||
      ((request->flags & kArTextRasterFlag_IncludeRevealClusters) &&
       (!bitmap->reveal_clusters || !bitmap->reveal_cluster_count)))
    return false;
  for (size_t index = 0; index < bitmap->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &bitmap->reveal_clusters[index];
    if (!cluster->end_utf8_byte ||
        cluster->end_utf8_byte > request->utf8_bytes ||
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
  return bitmap->width <= INT32_MAX / bytes_per_pixel &&
      bitmap->pitch_bytes >= bitmap->width * bytes_per_pixel;
}

bool ArTextRasterizer_Rasterize(const ArTextRasterizer *rasterizer,
                                const ArTextRasterRequest *request,
                                ArTextBitmap *out_bitmap,
                                char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
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
  if (!rasterizer->ops->rasterize(
          rasterizer->context, request, out_bitmap, error, error_capacity)) {
    rasterizer->ops->release_bitmap(rasterizer->context, out_bitmap);
    memset(out_bitmap, 0, sizeof(*out_bitmap));
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
