#ifndef AR_LOCALIZATION_TEXT_RASTERIZER_H
#define AR_LOCALIZATION_TEXT_RASTERIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/render_types.h"

#define AR_TEXT_RASTERIZER_ABI_VERSION UINT32_C(1)
#define AR_TEXT_RASTER_REQUEST_ABI_VERSION UINT32_C(5)
#define AR_TEXT_BITMAP_ABI_VERSION UINT32_C(2)

enum { kArTextRasterErrorCapacity = 256 };

typedef enum ArTextDirection {
  kArTextDirection_Auto = 0,
  kArTextDirection_LeftToRight,
  kArTextDirection_RightToLeft,
} ArTextDirection;

typedef enum ArTextHorizontalAlignment {
  kArTextHorizontalAlignment_Leading = 0,
  kArTextHorizontalAlignment_Center,
  kArTextHorizontalAlignment_Trailing,
} ArTextHorizontalAlignment;

typedef uint32_t ArTextRasterFlags;
enum {
  kArTextRasterFlag_WrapWords = UINT32_C(1) << 0,
  kArTextRasterFlag_PreserveHardBreaks = UINT32_C(1) << 1,
  kArTextRasterFlag_CropHorizontalWhitespace = UINT32_C(1) << 2,
  /* Ask the backend to return shaped cluster rectangles. The cached page can
   * then be revealed without reshaping, rerasterizing, or moving glyphs. */
  kArTextRasterFlag_IncludeRevealClusters = UINT32_C(1) << 3,
  /* Tight fixed-row labels may discard transparent top/bottom padding. This
   * is not appropriate for baseline-aligned fields or scrolling dialogue. */
  kArTextRasterFlag_CropVerticalWhitespace = UINT32_C(1) << 4,
};

typedef enum ArTextStyle {
  kArTextStyle_PlainWhite = 0,
  kArTextStyle_RetailBlueWhiteBands = 1,
} ArTextStyle;

/* Renderer-neutral enhanced-font treatment. LowResolution changes the source
 * rasterization size before nearest upscaling; Mosaic rasterizes at full size
 * and groups the result into output-pixel blocks. */
typedef enum ArTextPixelation {
  kArTextPixelation_None = 0,
  kArTextPixelation_LowResolution,
  kArTextPixelation_Mosaic,
} ArTextPixelation;

/* Immutable, renderer-neutral input. The caller owns all string pointers for
 * the duration of rasterize(). `font_stack_id` names a registered ordered
 * family stack; it is not an SDL/system-font handle. */
typedef struct ArTextRasterRequest {
  size_t struct_size;
  uint32_t abi_version;
  const char *utf8;
  size_t utf8_bytes;
  const char *font_stack_id;
  size_t font_stack_id_bytes;
  uint64_t source_revision;
  uint64_t font_revision;
  uint32_t style_id;
  ArTextRasterFlags flags;
  ArTextDirection direction;
  ArTextHorizontalAlignment alignment;
  /* Requested size and the smallest acceptable automatic fit size. Backends
   * may reduce only when the requested text exceeds its immutable bounds. */
  int font_pixels;
  int minimum_font_pixels;
  int maximum_width;
  int maximum_height;
  ArRenderFilter filter;
  /* Optional BCP 47 shaping language (for example "fr" or "ar"). A null
   * pointer and zero bytes select backend auto/default behavior. */
  const char *language_bcp47;
  size_t language_bcp47_bytes;
  ArTextPixelation pixelation;
  /* Zero is the no-effect value. Active treatments use a final-output block
   * size from 2 through 8 pixels. */
  int pixelation_size;
} ArTextRasterRequest;

typedef struct ArTextRevealCluster {
  /* UTF-8 byte offset immediately after this complete shaped cluster. */
  size_t end_utf8_byte;
  int line_index;
  int x;
  int y;
  int width;
  int height;
} ArTextRevealCluster;

/* Backend-owned CPU bitmap. It remains valid until release_bitmap(). Pixel
 * format names use the portable render-device vocabulary, so upload code does
 * not need a native graphics header. */
typedef struct ArTextBitmap {
  size_t struct_size;
  uint32_t abi_version;
  const void *pixels;
  int width;
  int height;
  int pitch_bytes;
  ArRenderPixelFormat format;
  int ascent;
  int descent;
  int line_advance;
  const ArTextRevealCluster *reveal_clusters;
  size_t reveal_cluster_count;
  uintptr_t token;
} ArTextBitmap;

typedef struct ArTextRasterizerOps {
  size_t struct_size;
  uint32_t abi_version;
  bool (*rasterize)(void *context, const ArTextRasterRequest *request,
                    ArTextBitmap *out_bitmap,
                    char *error, size_t error_capacity);
  void (*release_bitmap)(void *context, ArTextBitmap *bitmap);
  /* Optional ABI-1 tail: scalar coverage of the registered ordered stack.
   * Success with provided=false means a missing glyph; false means the query
   * failed/unavailable. This does not certify cluster shaping or ligatures. */
  bool (*has_glyph)(void *context, uint32_t scalar, bool *provided, char *error,
                    size_t error_capacity);
} ArTextRasterizerOps;

typedef struct ArTextRasterizer {
  const ArTextRasterizerOps *ops;
  void *context;
  /* Bump when shaping/rasterization behavior changes without a request or font
   * revision change. This value participates in every cache key. */
  uint64_t implementation_revision;
} ArTextRasterizer;

bool ArTextRasterizer_Init(ArTextRasterizer *rasterizer,
                           const ArTextRasterizerOps *ops,
                           void *context,
                           uint64_t implementation_revision);
void ArTextRasterizer_Reset(ArTextRasterizer *rasterizer);
bool ArTextRasterizer_IsReady(const ArTextRasterizer *rasterizer);
bool ArTextRasterRequest_IsValid(const ArTextRasterRequest *request);
bool ArTextRasterizer_Rasterize(const ArTextRasterizer *rasterizer,
                                const ArTextRasterRequest *request,
                                ArTextBitmap *out_bitmap,
                                char *error, size_t error_capacity);
void ArTextRasterizer_ReleaseBitmap(const ArTextRasterizer *rasterizer,
                                    ArTextBitmap *bitmap);

/* Layout controls and Unicode default-ignorables do not require standalone
 * glyphs. Combining accents and ordinary spaces still require coverage. */
bool ArTextGlyphNeedsCoverage(uint32_t scalar);
bool ArTextRasterizer_HasGlyph(const ArTextRasterizer *rasterizer,
                               uint32_t scalar, bool *provided, char *error,
                               size_t error_capacity);

/* Bounds of nontransparent pixels within a bitmap-local rectangle. Empty for
 * transparent/invalid input; RGB565 is opaque. Does not alter font metrics. */
ArRenderRectI ArTextBitmap_InkBounds(const ArTextBitmap *bitmap,
                                    ArRenderRectI region);

#endif /* AR_LOCALIZATION_TEXT_RASTERIZER_H */
