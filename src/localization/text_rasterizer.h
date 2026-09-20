#ifndef AR_LOCALIZATION_TEXT_RASTERIZER_H
#define AR_LOCALIZATION_TEXT_RASTERIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/font_resource.h"
#include "localization/text_bidi.h"
#include "localization/text_template.h"
#include "render/render_types.h"

#define AR_TEXT_RASTERIZER_ABI_VERSION UINT32_C(2)
#define AR_TEXT_RASTER_REQUEST_ABI_VERSION UINT32_C(14)
#define AR_TEXT_BITMAP_ABI_VERSION UINT32_C(7)

enum { kArTextRasterErrorCapacity = 256 };

typedef enum ArTextHorizontalAlignment {
  /* Logical paragraph edges, resolved once by the text backend. */
  kArTextHorizontalAlignment_Leading = 0,
  kArTextHorizontalAlignment_Center,
  kArTextHorizontalAlignment_Trailing,
  /* Fixed game geometry (selectors/HUD columns) must not follow bidi. */
  kArTextHorizontalAlignment_Left,
  kArTextHorizontalAlignment_Right,
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
  kArTextRasterFlag_Italic = UINT32_C(1) << 5,
  /* Synthetic oblique for complete ASCII-numeral clusters in a mixed run.
   * Keeps labels, non-ASCII numerals and joined/combining clusters unchanged.
   * Whole-field Italic takes precedence; effects never reshape the string. */
  kArTextRasterFlag_SlantAsciiNumerals = UINT32_C(1) << 6,
};

typedef enum ArTextStyle {
  kArTextStyle_PlainWhite = 0,
  kArTextStyle_RetailBlueWhiteBands = 1,
  kArTextStyle_RetailPaletteBands = 2,
} ArTextStyle;

typedef enum ArTextShadowShape {
  kArTextShadow_Diagonal = 0,
  kArTextShadow_Keyline, /* union of right and lower edges, not a full outline */
} ArTextShadowShape;

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
  /* RGB endpoints for RetailPaletteBands, supplied by the game adapter. */
  uint32_t band_rgb;
  uint32_t body_rgb;
  /* Third retail ink: the shade the game draws beside every stroke, which is
   * what gives its lettering weight against a busy background. Disabled
   * leaves the text unshaded; RGB zero is a valid black shadow. */
  uint32_t shadow_rgb;
  bool shadow_enabled;
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
  /* Optional solid accent on the shaped cluster containing this complete
   * UTF-8 grapheme end. Zero disables it. Never splits a ligature/combining
   * sequence, and is independent of the paragraph's visual direction. */
  uint32_t accent_end_utf8_byte;
  uint32_t accent_rgb;
  ArTextShadowShape shadow_shape;
  const ArTextBidiSpan *bidi_spans;
  size_t bidi_span_count;
  /* Grid/row requests borrow the owning source's spans without allocating a
   * list per cell. Backend clips to this view and returns view-local offsets. */
  uint32_t bidi_source_offset;
  /* Optional global one-bit-per-byte map. A set bit on an ASCII space in
   * this request asks the backend to keep that native row break only when the
   * preceding segment still fits on one proportional line. Other set bits are
   * ignored. The source offset indexes this request into the borrowed map. */
  const uint8_t *preferred_line_breaks;
  /* Number of byte positions addressable by the borrowed one-bit map. */
  size_t preferred_line_break_capacity;
  uint32_t preferred_line_break_source_offset;
  /* A line cut out of a larger page, rasterized on its own so editing it does
   * not rebuild the page. Zero fits between minimum_font_pixels and
   * font_pixels as usual. Non-zero rasterizes at exactly this size, counted
   * in rasterized pixels (after any low-resolution reduction): the size the
   * page was fitted at, which the line alone would not choose. font_pixels
   * still selects the low-resolution scale. */
  int raster_font_pixels;
  /* Mosaic blocks of a line drawn over its page must fall where the page's
   * blocks fell. When set, a block boundary passes through bitmap coordinate
   * (pixelation_grid_x, pixelation_grid_y), in output pixels relative to the
   * uncropped layout origin, and the surface is padded out to whole blocks
   * (see ArTextSurface.origin_x). No effect without an active mosaic. */
  bool align_pixelation_grid;
  int pixelation_grid_x;
  int pixelation_grid_y;
  /* Optional template-resolved appearance. Null keeps the legacy whole-text
   * fields above. Spans are sorted, nonoverlapping, and use the owning source's
   * UTF-8 offsets, just like bidi spans; gaps inherit this default appearance.
   */
  const ArTextRunAppearance *appearance;
  const ArTextAppearanceSpan *appearance_spans;
  size_t appearance_span_count;
  uint32_t appearance_source_offset;
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

typedef struct ArTextLineMetrics {
  /* Relative to the bitmap origin; top can be negative after ink cropping.
   * Height includes the minimum strut and leading reserved for this line. */
  int top, baseline, height;
} ArTextLineMetrics;

/* Actual font used for a shaped logical range, after fallback and fitting.
 * Overlapping ranges can use different fonts for a combining cluster. IDs
 * identify registered bytes; no backend font handles escape. font_pixels is
 * before any host low-resolution enlargement. */
typedef struct ArTextFontUse {
  uint32_t start, end;
  ArFontResourceId resource;
  uint16_t font_pixels;
  bool missing;
} ArTextFontUse;

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
  /* Optional effect-aware ownership, one cluster index + 1 per pixel (zero
   * for transparency). Tightly packed width*height uint32_t values. Effects
   * carry their source owner; advances/reveal rectangles remain typographic.
   * Backend-owned and released with the bitmap. */
  const uint32_t *pixel_owners;
  /* Resolved base of the first paragraph, for direction-following placement
   * of a cropped single label. Subsequent auto paragraphs may differ and are
   * aligned internally by the backend. Auto means not supplied by a port. */
  ArTextDirection paragraph_direction;
  /* Size this bitmap was rasterized at after fitting, or zero when the
   * backend does not report it. */
  int font_pixels;
  /* Blank columns and rows whitespace cropping removed from the left and top
   * of the laid-out text, so the layout origin is (-crop_left, -crop_top). */
  int crop_left;
  int crop_top;
  const ArTextLineMetrics *lines;
  size_t line_count;
  const ArTextFontUse *font_uses;
  size_t font_use_count;
} ArTextBitmap;

/* Why a rasterization failed, so a caller can tell "this request can never
 * work" from "the backend was momentarily unable to serve it". Only the first
 * is safe to remember as the permanent answer for a request. */
typedef enum ArTextRasterFailure {
  kArTextRasterFailure_None = 0,
  /* Invalid or unsupported input, or content that cannot fit the requested
   * bounds. Retrying the identical request repeats the same work and fails
   * the same way. This is also the assumed classification when a backend
   * reports a failure without choosing one. */
  kArTextRasterFailure_Deterministic,
  /* Allocation, font resource or device failure. The identical request may
   * well succeed later, so it must not become a permanent answer. */
  kArTextRasterFailure_Retryable,
} ArTextRasterFailure;

typedef struct ArTextRasterizerOps {
  size_t struct_size;
  uint32_t abi_version;
  /* On failure, write the classification to `out_failure` (never NULL). */
  bool (*rasterize)(void *context, const ArTextRasterRequest *request,
                    ArTextBitmap *out_bitmap, ArTextRasterFailure *out_failure,
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
/* `out_failure` is optional and is only written on failure. An unclassified
 * backend failure reports kArTextRasterFailure_Deterministic. */
bool ArTextRasterizer_Rasterize(const ArTextRasterizer *rasterizer,
                                const ArTextRasterRequest *request,
                                ArTextBitmap *out_bitmap,
                                ArTextRasterFailure *out_failure,
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

/* Recolours each cluster's ink with the game's vertical two-tone band, in
 * place. Alpha is untouched, so nothing about the shaped coverage changes.
 *
 * This lives here rather than in a backend so a second shaper inherits the
 * appearance instead of reimplementing the formula. Only RGBA8888 is
 * supported today, which is what the rasterizers produce. */
/* Lays the game's third ink behind the glyphs: every transparent pixel that
 * has ink `offset_x`/`offset_y` before it becomes shadow at that ink's
 * coverage. Existing ink is never overwritten, so the letterforms are
 * untouched and only their surroundings gain weight. Runs after the bands, or
 * the recolouring would sweep the shadow up with the strokes. Allocation-free;
 * false means invalid arguments, never a transient resource failure. */
bool ArTextBitmap_ApplyStyleShadow(void *pixels, int width, int height,
                                   int pitch_bytes, ArRenderPixelFormat format,
                                   int offset_x, int offset_y,
                                   uint32_t shadow_rgb);

/* Allocates bounded, per-pixel ownership from a shaper's cluster rectangles.
 * Call before effects; clusters must describe the same complete shaped run.
 * The caller owns the result. No allocation takes place during reveal. */
uint32_t *ArTextBitmap_BuildOwnership(const ArTextBitmap *bitmap);
bool ArTextBitmap_ApplyOwnedShadow(void *pixels, int width, int height,
    int pitch_bytes, uint32_t *owners, int step, uint32_t rgb,
    ArTextShadowShape shape);

/* Cache-miss-only numeral treatment, before shadow. The caller reserves
 * ceil(maximum glyph ink height / 4) pixels on the right. Uses the existing
 * complete-run cluster ownership, leaving advances and baselines unchanged. */
bool ArTextBitmap_SlantAsciiNumerals(ArTextBitmap *bitmap,
                                    const char *utf8, size_t bytes);

bool ArTextBitmap_ApplyStyleBands(void *pixels, int width, int height,
                                  int pitch_bytes, ArRenderPixelFormat format,
                                  const ArTextRevealCluster *clusters,
                                  size_t cluster_count, uint32_t band_rgb,
                                  uint32_t body_rgb);

bool ArTextBitmap_ApplyClusterAccent(void *pixels, int width, int height,
                                     int pitch_bytes, ArRenderPixelFormat format,
                                     const ArTextRevealCluster *clusters,
                                     size_t cluster_count, uint32_t utf8_end,
                                     uint32_t rgb);

/* Shared band interpolation for uniform and mixed-appearance raster paths. */
uint32_t ArTextStyle_BandColor(uint32_t band_rgb, uint32_t body_rgb, int row,
                               int last_row);

#endif /* AR_LOCALIZATION_TEXT_RASTERIZER_H */
