#include "platform/sdl/text_rasterizer_sdl.h"
#include "localization/text_boundaries.h"
#include "localization/unicode_grapheme.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AR_HAS_SDL3_TTF
#define AR_HAS_SDL3_TTF 0
#endif

#define AR_MEMBER_END(type, member) \
  (offsetof(type, member) + sizeof(((type *)0)->member))

static void SetError(char *error, size_t capacity, const char *message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
}

#if AR_HAS_SDL3_TTF

#include <math.h>

#include "platform/sdl/bidi_text_sdl.h"
#include "platform/sdl/text_fonts_sdl.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

enum {
  kGlyphCoverageCacheCapacity = 1024,
  kMissingGlyphWarningCapacity = 64,
};

typedef struct SdlTextRasterizerState {
  char *font_stack_id;
  ArSdlTextFonts fonts;
  uint64_t font_revision;
  bool ttf_initialized;
  uint32_t coverage_keys[kGlyphCoverageCacheCapacity];
  bool coverage_values[kGlyphCoverageCacheCapacity];
  uint32_t warned_scalars[kMissingGlyphWarningCapacity];
  ArFontResourceId warned_fonts[kMissingGlyphWarningCapacity];
  size_t warning_count;
  bool warnings_saturated;
} SdlTextRasterizerState;

typedef struct SdlTextBitmapToken {
  SDL_Surface *surface;
  ArTextRevealCluster *reveal_clusters;
  uint32_t *pixel_owners;
  ArTextLineMetrics *lines;
  ArTextFontUse *font_uses;
} SdlTextBitmapToken;

static char *CopyString(const char *source) {
  if (!source || !source[0]) return NULL;
  const size_t length = strlen(source);
  char *copy = (char *)malloc(length + 1u);
  if (copy) memcpy(copy, source, length + 1u);
  return copy;
}

static bool HasGlyph(void *context, uint32_t scalar, bool *provided,
                     char *error, size_t error_capacity) {
  SdlTextRasterizerState *state = context;
  const size_t slot = scalar % kGlyphCoverageCacheCapacity;
  if (state->coverage_keys[slot] == scalar + 1u) {
    *provided = state->coverage_values[slot];
    return true;
  }
  ArSdlTextFonts_BeginLayout(&state->fonts);
  ArSdlFontSet *set = ArSdlTextFonts_Acquire(&state->fonts, "body", 24, false,
                                             NULL, error, error_capacity);
  if (!set)
    return false;
  /* SDL_ttf's query traverses the configured fallback chain too. */
  *provided = TTF_FontHasGlyph(set->primary, scalar);
  state->coverage_keys[slot] = scalar + 1u;
  state->coverage_values[slot] = *provided;
  return true;
}

static void WarnMissingScalar(SdlTextRasterizerState *state, uint32_t scalar,
                              ArFontResourceId font) {
  if (state->warnings_saturated || !ArTextGlyphNeedsCoverage(scalar))
    return;
  for (size_t i = 0; i < state->warning_count; ++i)
    if (state->warned_scalars[i] == scalar && state->warned_fonts[i] == font)
      return;
  if (state->warning_count == kMissingGlyphWarningCapacity) {
    fprintf(stderr,
            "[localized-text] further missing-character warnings for font "
            "stack '%s' are suppressed\n",
            state->font_stack_id);
    state->warnings_saturated = true;
    return;
  }
  state->warned_scalars[state->warning_count] = scalar;
  state->warned_fonts[state->warning_count++] = font;
  fprintf(stderr,
          "[localized-text] font stack '%s' %s U+%04X "
          "(font resource %llu); a replacement box may be shown. Add a font "
          "covering this character to the affected role's fallback list in "
          "pack.ini, then reimport and reinstall the pack. See the "
          "language-pack authoring manual.\n",
          state->font_stack_id,
          font ? "could not fully shape a cluster containing"
               : "has no glyph for",
          (unsigned)scalar, (unsigned long long)font);
}

static void WarnMissingGlyphs(SdlTextRasterizerState *state, const char *text) {
  /* Only called on raster misses, never on cached menu/reveal frames. Keep
   * reports bounded for arbitrary authored/dynamic text and reset with stack.
   */
  if (state->warnings_saturated)
    return;
  const size_t length = strlen(text);
  size_t cursor = 0;
  while (cursor < length) {
    uint32_t scalar;
    if (!ArUnicode_DecodeScalar(text, length, cursor, &scalar, &cursor))
      return;
    bool provided = false;
    if (!ArTextGlyphNeedsCoverage(scalar) ||
        !HasGlyph(state, scalar, &provided, NULL, 0) || provided)
      continue;
    WarnMissingScalar(state, scalar, 0);
  }
}

static void WarnMissingStyledGlyphs(SdlTextRasterizerState *state,
                                    const char *text, size_t bytes,
                                    const ArTextFontUse *uses, size_t count) {
  for (size_t i = 0; i < count && !state->warnings_saturated; ++i) {
    if (!uses[i].missing)
      continue;
    for (size_t at = uses[i].start; at < uses[i].end;) {
      uint32_t scalar;
      if (!ArUnicode_DecodeScalar(text, bytes, at, &scalar, &at))
        return;
      WarnMissingScalar(state, scalar, uses[i].resource);
    }
  }
}

/* The band formula is portable (ArTextBitmap_ApplyStyleBands); this adapter
 * only supplies the surface it just rendered. */
static bool ApplyRetailTextBands(
    SDL_Surface *surface, const ArTextRevealCluster *clusters,
    size_t cluster_count, uint32_t band_rgb, uint32_t body_rgb) {
  if (!surface || surface->format != SDL_PIXELFORMAT_RGBA8888)
    return false;
  return ArTextBitmap_ApplyStyleBands(
      surface->pixels, surface->w, surface->h, surface->pitch,
      kArRenderPixelFormat_Rgba8888, clusters, cluster_count, band_rgb,
      body_rgb);
}

/* One retail pixel, in rendered pixels. The game's shadow is a single pixel of
 * an eight-pixel cell, so it has to scale with the type or it vanishes at one
 * size and swamps the letters at another. The size that matters is the one
 * this attempt rendered at, not the one the caller asked for: a label that had
 * to shrink to fit its cells would otherwise keep the larger type's shadow. */
static bool ApplyRetailTextShadow(
    SDL_Surface *surface, int font_pixels, const ArTextRasterRequest *request,
    uint32_t *owners) {
  if (!surface || surface->format != SDL_PIXELFORMAT_RGBA8888) return false;
  int step = (font_pixels + 4) / 8;
  if (step < 1) step = 1;
  return ArTextBitmap_ApplyOwnedShadow(
      surface->pixels, surface->w, surface->h, surface->pitch,
      owners, step, request->shadow_rgb, request->shadow_shape);
}

static inline bool PixelHasInk(const uint8_t *row, int x, Uint32 alpha_mask) {
  Uint32 pixel;
  memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
  return (pixel & alpha_mask) != 0;
}

static bool RowHasInk(const SDL_Surface *surface, int y, int from, int to,
                      Uint32 alpha_mask) {
  const uint8_t *row =
      (const uint8_t *)surface->pixels + (size_t)y * surface->pitch;
  for (int x = from; x < to; ++x)
    if (PixelHasInk(row, x, alpha_mask)) return true;
  return false;
}

static SDL_Surface *CropWhitespace(SDL_Surface *surface, ArTextRasterFlags flags,
                                    int *removed_left, int *removed_top) {
  if (removed_left) *removed_left = 0;
  if (removed_top) *removed_top = 0;
  const SDL_PixelFormatDetails *details = surface
      ? SDL_GetPixelFormatDetails(surface->format) : NULL;
  if (!surface || surface->format != SDL_PIXELFORMAT_RGBA8888 ||
      !surface->pixels || !details || details->bytes_per_pixel != 4u)
    return NULL;
  /* Every enhanced page raster crops, so this runs over the whole surface on
   * each text change. A per-pixel SDL_GetRGBA call here was the largest single
   * line of each page raster in a CPU sample of name-entry typing (M2,
   * 2026-09-17); the packed format's alpha mask gives the same zero test.
   * Blank edge rows are trimmed first, and an inked row only scans columns
   * outside the bounds found so far. */
  const Uint32 alpha_mask = details->Amask;
  const int width = surface->w;
  const int height = surface->h;
  int top = 0;
  while (top < height && !RowHasInk(surface, top, 0, width, alpha_mask)) ++top;
  if (top == height) return NULL;
  int bottom = height - 1;
  while (!RowHasInk(surface, bottom, 0, width, alpha_mask)) --bottom;
  int first = width;
  int last = -1;
  for (int y = top; y <= bottom; ++y) {
    const uint8_t *row =
        (const uint8_t *)surface->pixels + (size_t)y * surface->pitch;
    for (int x = 0; x < first; ++x) {
      if (PixelHasInk(row, x, alpha_mask)) {
        first = x;
        break;
      }
    }
    for (int x = width - 1; x > last; --x) {
      if (PixelHasInk(row, x, alpha_mask)) {
        last = x;
        break;
      }
    }
  }
  if (!(flags & kArTextRasterFlag_CropHorizontalWhitespace)) {
    first = 0;
    last = surface->w - 1;
  }
  if (!(flags & kArTextRasterFlag_CropVerticalWhitespace)) {
    top = 0;
    bottom = surface->h - 1;
  }
  if (first == 0 && last + 1 == surface->w &&
      top == 0 && bottom + 1 == surface->h) return surface;

  SDL_Surface *cropped = SDL_CreateSurface(
      last - first + 1, bottom - top + 1, SDL_PIXELFORMAT_RGBA8888);
  if (!cropped) return NULL;
  const SDL_Rect source = {first, top, cropped->w, cropped->h};
  const SDL_Rect destination = {0, 0, cropped->w, cropped->h};
  SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
  if (!SDL_BlitSurface(surface, &source, cropped, &destination)) {
    SDL_DestroySurface(cropped);
    return NULL;
  }
  if (removed_left) *removed_left = first;
  if (removed_top) *removed_top = top;
  return cropped;
}

static bool BuildRevealClusters(
    TTF_Font *font, const char *text,
    const ArTextRasterRequest *request,
    ArTextRevealCluster **out_clusters, size_t *out_count,
    char *error, size_t error_capacity) {
  *out_clusters = NULL;
  *out_count = 0;
  TTF_Text *layout = TTF_CreateText(
      NULL, font, text, request->utf8_bytes);
  if (!layout) {
    SetError(error, error_capacity, SDL_GetError());
    return false;
  }
  if ((request->flags & kArTextRasterFlag_WrapWords) &&
      !TTF_SetTextWrapWidth(layout, request->maximum_width)) {
    SetError(error, error_capacity, SDL_GetError());
    TTF_DestroyText(layout);
    return false;
  }
  if (!request->utf8_bytes ||
      request->utf8_bytes > SIZE_MAX / sizeof(ArTextRevealCluster)) {
    SetError(error, error_capacity, "text has no addressable shaped clusters");
    TTF_DestroyText(layout);
    return false;
  }
  ArTextRevealCluster *clusters = (ArTextRevealCluster *)calloc(
      request->utf8_bytes, sizeof(*clusters));
  if (!clusters) {
    SetError(error, error_capacity,
             "out of memory creating text reveal metadata");
    TTF_DestroyText(layout);
    return false;
  }

  TTF_SubString substring;
  if (!TTF_GetTextSubString(layout, 0, &substring)) {
    SetError(error, error_capacity, SDL_GetError());
    free(clusters);
    TTF_DestroyText(layout);
    return false;
  }
  size_t count = 0;
  for (size_t visited = 0; visited < request->utf8_bytes; ++visited) {
    if (substring.offset < 0 || substring.length <= 0) break;
    const size_t end =
        (size_t)substring.offset + (size_t)substring.length;
    if (end > request->utf8_bytes) {
      free(clusters);
      TTF_DestroyText(layout);
      SetError(error, error_capacity,
               "text shaper returned an invalid cluster range");
      return false;
    }
    if (substring.line_index >= 0 && substring.rect.w > 0 &&
        substring.rect.h > 0) {
      clusters[count++] = (ArTextRevealCluster){
        .end_utf8_byte = end,
        .line_index = substring.line_index,
        .x = substring.rect.x,
        .y = substring.rect.y,
        .width = substring.rect.w,
        .height = substring.rect.h,
      };
    }
    if (substring.flags & TTF_SUBSTRING_TEXT_END) break;
    TTF_SubString next;
    if (!TTF_GetNextTextSubString(layout, &substring, &next)) {
      free(clusters);
      TTF_DestroyText(layout);
      SetError(error, error_capacity, SDL_GetError());
      return false;
    }
    if (next.offset == substring.offset &&
        next.length == substring.length) {
      free(clusters);
      TTF_DestroyText(layout);
      SetError(error, error_capacity,
               "text shaper did not advance to the next cluster");
      return false;
    }
    substring = next;
  }
  TTF_DestroyText(layout);
  if (!count) {
    free(clusters);
    SetError(error, error_capacity,
             "text shaper returned no revealable clusters");
    return false;
  }
  *out_clusters = clusters;
  *out_count = count;
  return true;
}

static size_t ClampRevealClusters(ArTextRevealCluster *clusters,
                                  size_t count, int removed_left,
                                  int removed_top, int width, int height,
                                  uint32_t *remap) {
  size_t kept = 0;
  for (size_t index = 0; index < count; ++index) {
    ArTextRevealCluster cluster = clusters[index];
    int left = cluster.x - removed_left;
    int top = cluster.y - removed_top;
    int right = left + cluster.width;
    int bottom = top + cluster.height;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) continue;
    cluster.x = left;
    cluster.y = top;
    cluster.width = right - left;
    cluster.height = bottom - top;
    if (remap) remap[index+1] = (uint32_t)kept+1;
    clusters[kept++] = cluster;
  }
  return kept;
}

static bool ConfigureFontForRequest(
    TTF_Font *font, const ArTextRasterRequest *request,
    char *error, size_t error_capacity) {
  TTF_Direction direction = TTF_DIRECTION_INVALID;
  if (request->direction == kArTextDirection_LeftToRight)
    direction = TTF_DIRECTION_LTR;
  else if (request->direction == kArTextDirection_RightToLeft)
    direction = TTF_DIRECTION_RTL;
  if (!TTF_SetFontDirection(font, direction)) {
    SetError(error, error_capacity, SDL_GetError());
    return false;
  }

  TTF_HorizontalAlignment alignment = TTF_HORIZONTAL_ALIGN_LEFT;
  if (request->alignment == kArTextHorizontalAlignment_Center)
    alignment = TTF_HORIZONTAL_ALIGN_CENTER;
  else if (request->alignment == kArTextHorizontalAlignment_Right)
    alignment = TTF_HORIZONTAL_ALIGN_RIGHT;
  else if (request->alignment == kArTextHorizontalAlignment_Left)
    alignment = TTF_HORIZONTAL_ALIGN_LEFT;
  else if (request->alignment == kArTextHorizontalAlignment_Trailing)
    alignment = request->direction == kArTextDirection_RightToLeft
        ? TTF_HORIZONTAL_ALIGN_LEFT : TTF_HORIZONTAL_ALIGN_RIGHT;
  else if (request->direction == kArTextDirection_RightToLeft)
    alignment = TTF_HORIZONTAL_ALIGN_RIGHT;
  TTF_SetFontWrapAlignment(font, alignment);

  char language[64];
  const char *language_pointer = NULL;
  if (request->language_bcp47_bytes) {
    memcpy(language, request->language_bcp47,
           request->language_bcp47_bytes);
    language[request->language_bcp47_bytes] = '\0';
    language_pointer = language;
  }
  if (!TTF_SetFontLanguage(font, language_pointer) && language_pointer) {
    SetError(error, error_capacity, SDL_GetError());
    return false;
  }
  return true;
}

static void ResetPreparedText(const ArTextRasterRequest *request, char *text) {
  memcpy(text, request->utf8, request->utf8_bytes);
  text[request->utf8_bytes] = '\0';
  if (!(request->flags & kArTextRasterFlag_PreserveHardBreaks)) {
    for (size_t i = 0; i < request->utf8_bytes; ++i) {
      if (text[i] == '\r' || text[i] == '\n') text[i] = ' ';
    }
  }
}

static char *PrepareText(const ArTextRasterRequest *request) {
  char *text = (char *)malloc(request->utf8_bytes + 1u);
  if (!text) return NULL;
  ResetPreparedText(request, text);
  return text;
}

/* Native extraction can identify a likely intentional row break, but only the
 * final font stack and projected output bounds can decide whether retaining it
 * still improves the proportional layout. The byte remains one byte (space or
 * LF), so reveal offsets and typed bidi spans keep their stable coordinates. */
static bool ResolvePreferredLineBreaks(
    TTF_Font *font, const ArTextRasterRequest *request, char *text,
    int wrap_width) {
  if (!request->preferred_line_breaks) return true;
  size_t segment = 0;
  for (size_t i = 0; i < request->utf8_bytes; ++i) {
    if (text[i] == '\r' || text[i] == '\n') {
      segment = i + 1u;
      continue;
    }
    if (text[i] != ' ' ||
        !ArTextBoundary_Get(request->preferred_line_breaks,
                            request->preferred_line_break_source_offset + i))
      continue;
    int width = 0, height = 0;
    if (!TTF_GetStringSize(font, text + segment, i - segment,
                           &width, &height))
      return false;
    if (width <= wrap_width) {
      text[i] = '\n';
      segment = i + 1u;
    }
  }
  return true;
}

/* Matches kArTextSurfaceMaximumRequestBytes: no single request is worth more
 * than this many pixel bytes, whatever bounds it asks for. */
enum { kMaximumRasterBytes = INT64_C(64) << 20 };

typedef enum RasterAttemptResult {
  kRasterAttempt_Error = 0,
  kRasterAttempt_ExceedsBounds,
  kRasterAttempt_Success,
} RasterAttemptResult;

typedef struct RasterAttempt {
  SDL_Surface *surface;
  ArTextRevealCluster *reveal_clusters;
  uint32_t *pixel_owners;
  size_t reveal_cluster_count;
  int ascent;
  int descent;
  int line_advance;
  int font_pixels;
  int crop_left;
  int crop_top;
  ArTextDirection paragraph_direction;
  ArSdlClusterPaint *paint;
  ArTextLineMetrics *lines;
  size_t line_count;
  ArTextFontUse *font_uses;
  size_t font_use_count;
} RasterAttempt;

static void DestroyRasterAttempt(RasterAttempt *attempt) {
  if (!attempt) return;
  SDL_DestroySurface(attempt->surface);
  free(attempt->reveal_clusters);
  free(attempt->pixel_owners);
  free(attempt->paint);
  free(attempt->lines);
  free(attempt->font_uses);
  memset(attempt, 0, sizeof(*attempt));
}

static bool StyledPadding(SdlTextRasterizerState *state,
                          const ArTextRasterRequest *request, int base_pixels,
                          int *right, int *bottom, ArTextRasterFailure *failure,
                          char *error, size_t capacity) {
  *right = *bottom = 0;
  for (size_t i = 0; i <= request->appearance_span_count; ++i) {
    const ArTextRunAppearance *appearance = request->appearance;
    if (i) {
      const ArTextAppearanceSpan *span = &request->appearance_spans[i - 1];
      if (span->end <= request->appearance_source_offset ||
          span->start >=
              request->appearance_source_offset + request->utf8_bytes)
        continue;
      appearance = &span->appearance;
    }
    const int pixels =
        (int)(((int64_t)base_pixels * appearance->scale_basis + 5000) / 10000);
    ArSdlFontSet *set =
        ArSdlTextFonts_Acquire(&state->fonts, appearance->font_role, pixels,
                               appearance->italic, failure, error, capacity);
    if (!set)
      return false;
    const int shadow = appearance->shadow_enabled
                           ? (pixels + 4) / 8 > 0 ? (pixels + 4) / 8 : 1
                           : 0;
    int height = TTF_GetFontHeight(set->primary);
    for (size_t f = 0; f < set->role->fallback_count; ++f)
      if (TTF_GetFontHeight(set->fallbacks[f]) > height)
        height = TTF_GetFontHeight(set->fallbacks[f]);
    const int slant = appearance->slant_ascii_numerals && !appearance->italic
                          ? (height + 3) / 4
                          : 0;
    if (shadow + slant > *right)
      *right = shadow + slant;
    if (shadow > *bottom)
      *bottom = shadow;
  }
  return true;
}

/* Every error exit classifies itself. Opening/sizing a font, rendering,
 * converting and cropping all allocate, so those failures are the ones a later
 * identical request can survive; the rest are properties of the request. */
static RasterAttemptResult RasterizeAtSize(
    SdlTextRasterizerState *state, const ArTextRasterRequest *request,
    char *text, int font_pixels, RasterAttempt *attempt,
    ArTextRasterFailure *failure, char *error, size_t error_capacity) {
  memset(attempt, 0, sizeof(*attempt));
  /* Classification for the error exits below; the non-error returns clear it,
   * so a stale value never reaches a caller. */
  *failure = kArTextRasterFailure_Retryable;
  ArSdlTextFonts_BeginLayout(&state->fonts);
  const ArTextRunAppearance *appearance = request->appearance;
  const int strut_pixels =
      appearance
          ? (int)(((int64_t)font_pixels * appearance->scale_basis + 5000) /
                  10000)
          : font_pixels;
  ArSdlFontSet *set = ArSdlTextFonts_Acquire(
      &state->fonts, appearance ? appearance->font_role : "body", strut_pixels,
      appearance ? appearance->italic
                 : (request->flags & kArTextRasterFlag_Italic) != 0,
      failure, error, error_capacity);
  if (!set)
    return kRasterAttempt_Error;
  *failure = kArTextRasterFailure_Retryable;
  if (!ConfigureFontForRequest(set->primary, request, error, error_capacity))
    return kRasterAttempt_Error;
  int shadow_step = !request->shadow_enabled    ? 0
                    : (font_pixels + 4) / 8 > 0 ? (font_pixels + 4) / 8
                                                : 1;
  bool slant_numerals = false;
  if (!appearance && (request->flags & kArTextRasterFlag_SlantAsciiNumerals) &&
      !(request->flags & kArTextRasterFlag_Italic))
    for (size_t i = 0; i < request->utf8_bytes; ++i)
      slant_numerals |= text[i] >= '0' && text[i] <= '9';
  int right_padding =
      shadow_step +
      (slant_numerals ? (TTF_GetFontHeight(set->primary) + 3) / 4 : 0);
  if (appearance &&
      !StyledPadding(state, request, font_pixels, &right_padding, &shadow_step,
                     failure, error, error_capacity))
    return kRasterAttempt_Error;
  *failure = kArTextRasterFailure_Retryable;
  const bool need_owners =
      appearance || slant_numerals ||
      (request->flags & kArTextRasterFlag_IncludeRevealClusters);
  const int bytes_per_sample = appearance || slant_numerals ? 16
                               : need_owners                ? 8
                                                            : 4;
  const int wrap_width = request->maximum_width - right_padding;
  if (wrap_width <= 0) {
    *failure = kArTextRasterFailure_None;
    return kRasterAttempt_ExceedsBounds;
  }
  ResetPreparedText(request, text);
  if (!appearance &&
      !ResolvePreferredLineBreaks(set->primary, request, text, wrap_width)) {
    SetError(error, error_capacity, SDL_GetError());
    return kRasterAttempt_Error;
  }

  /* Measure before rendering. A fixed field disables wrapping, so an accepted
   * but far too long label would otherwise be rasterized at its full width and
   * only then rejected. Cropping can only shrink the result, so the early exit
   * is limited to the axes this request is not asking to crop, plus an
   * absolute ceiling that no request may exceed. */
  int measured_w = 0, measured_h = 0;
  ArTextRasterRequest shaping = *request;
  shaping.maximum_width = wrap_width;
  ArSdlBidiLayout *bidi = NULL;
  /* SDL's single-line API ignores LF even when the caller preserves it.
   * Use the shared line shaper for hard breaks without automatic wrapping. */
  const bool unwrapped_lines =
      !(request->flags & kArTextRasterFlag_WrapWords) &&
      memchr(text, '\n', request->utf8_bytes);
  if (appearance || request->bidi_span_count || unwrapped_lines ||
      ArSdlBidiText_NeedsLayout(text, request->utf8_bytes,
                                request->direction)) {
    bidi = appearance
               ? ArSdlBidiText_CreateStyled(&state->fonts, set->primary, text,
                                            &shaping, font_pixels, failure)
               : ArSdlBidiText_Create(set->primary, text, &shaping, failure);
    if (!bidi) {
      SetError(error, error_capacity, SDL_GetError());
      return kRasterAttempt_Error;
    }
    ArSdlBidiText_GetSize(bidi, &measured_w, &measured_h);
  }
  attempt->paragraph_direction = bidi ? ArSdlBidiText_GetDirection(bidi)
                                      : kArTextDirection_LeftToRight;
  const bool measured =
      bidi ? true : request->flags & kArTextRasterFlag_WrapWords
      ? TTF_GetStringSizeWrapped(set->primary, text, request->utf8_bytes,
                                 wrap_width, &measured_w,
                                 &measured_h)
      : TTF_GetStringSize(set->primary, text, request->utf8_bytes, &measured_w,
                          &measured_h);
  if (measured) {
    const bool crops_width =
        (request->flags & kArTextRasterFlag_CropHorizontalWhitespace) != 0;
    const bool crops_height =
        (request->flags & kArTextRasterFlag_CropVerticalWhitespace) != 0;
    if ((!crops_width && (int64_t)measured_w+right_padding > request->maximum_width) ||
        (!crops_height && (int64_t)measured_h+shadow_step > request->maximum_height)) {
      ArSdlBidiText_Destroy(bidi);
      DestroyRasterAttempt(attempt);
      *failure = kArTextRasterFailure_None;
      return kRasterAttempt_ExceedsBounds;
    }
    if (measured_w > 0 && measured_h > 0 &&
        ((int64_t)measured_w+right_padding) * ((int64_t)measured_h+shadow_step) *
          bytes_per_sample > kMaximumRasterBytes) {
      ArSdlBidiText_Destroy(bidi);
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity,
               "rasterized text would exceed the per-request size ceiling");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }

  const SDL_Color white = {255, 255, 255, 255};
  SDL_Surface *rendered =
      bidi ? ArSdlBidiText_Render(bidi, &attempt->reveal_clusters,
                                 &attempt->reveal_cluster_count, failure)
      : request->flags & kArTextRasterFlag_WrapWords
      ? TTF_RenderText_Blended_Wrapped(
            set->primary, text, request->utf8_bytes, white,
            wrap_width)
      : TTF_RenderText_Blended(
            set->primary, text, request->utf8_bytes, white);
  const bool need_cluster_layout =
      request->accent_end_utf8_byte || slant_numerals ||
      request->style_id == kArTextStyle_RetailBlueWhiteBands ||
      request->style_id == kArTextStyle_RetailPaletteBands ||
      (request->flags & kArTextRasterFlag_IncludeRevealClusters);
  if (rendered && !bidi && need_cluster_layout &&
      !BuildRevealClusters(
          set->primary, text, &shaping,
          &attempt->reveal_clusters, &attempt->reveal_cluster_count,
          error, error_capacity)) {
    SDL_DestroySurface(rendered);
    rendered = NULL;
  }
  if (rendered && appearance) {
    attempt->paint =
        calloc(attempt->reveal_cluster_count, sizeof(*attempt->paint));
    attempt->lines = ArSdlBidiText_CopyLines(bidi, &attempt->line_count);
    if (!attempt->paint || !attempt->lines ||
        !ArSdlBidiText_CopyFontUses(bidi, &attempt->font_uses,
                                    &attempt->font_use_count)) {
      SDL_DestroySurface(rendered);
      rendered = NULL;
    } else
      ArSdlBidiText_DescribePaint(bidi, attempt->reveal_clusters,
                                  attempt->reveal_cluster_count,
                                  attempt->paint);
  }
  ArSdlBidiText_Destroy(bidi);
  if (!rendered) {
    if (!error || !error_capacity || !error[0])
      SetError(error, error_capacity, SDL_GetError());
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }

  attempt->surface = SDL_ConvertSurface(
      rendered, SDL_PIXELFORMAT_RGBA8888);
  SDL_DestroySurface(rendered);
  if (!attempt->surface) {
    SetError(error, error_capacity, SDL_GetError());
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  if (right_padding || shadow_step) {
    SDL_Surface *original = attempt->surface;
    if ((int64_t)original->w+right_padding > INT32_MAX ||
        (int64_t)original->h+shadow_step > INT32_MAX ||
        ((int64_t)original->w+right_padding)*((int64_t)original->h+shadow_step)*
          bytes_per_sample > kMaximumRasterBytes) {
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity, "styled text exceeds the per-request size ceiling");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    SDL_Surface *padded = SDL_CreateSurface(original->w+right_padding,
        original->h+shadow_step, SDL_PIXELFORMAT_RGBA8888);
    if (!padded) {
      SetError(error, error_capacity, SDL_GetError());
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    memset(padded->pixels, 0, (size_t)padded->pitch*padded->h);
    for (int y=0; y<original->h; ++y)
      memcpy((uint8_t *)padded->pixels+(size_t)y*padded->pitch,
             (uint8_t *)original->pixels+(size_t)y*original->pitch, (size_t)original->w*4);
    SDL_DestroySurface(original);
    attempt->surface=padded;
  }
  if (need_owners) {
    if (!attempt->reveal_cluster_count) {
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity, "text contains no rasterizable ink");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    const ArTextBitmap ink = {.pixels=attempt->surface->pixels,
        .width=attempt->surface->w, .height=attempt->surface->h,
        .pitch_bytes=attempt->surface->pitch, .format=kArRenderPixelFormat_Rgba8888,
        .reveal_clusters=attempt->reveal_clusters, .reveal_cluster_count=attempt->reveal_cluster_count};
    attempt->pixel_owners=ArTextBitmap_BuildOwnership(&ink);
    if (!attempt->pixel_owners) {
      SetError(error, error_capacity, "cannot allocate text effect ownership");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }
  if (appearance) {
    for (size_t i = 0;
         request->accent_end_utf8_byte && i < attempt->reveal_cluster_count;
         ++i)
      if (attempt->paint[i].source_start < request->accent_end_utf8_byte &&
          attempt->reveal_clusters[i].end_utf8_byte >=
              request->accent_end_utf8_byte) {
        attempt->paint[i].appearance.band_rgb = request->accent_rgb;
        attempt->paint[i].appearance.body_rgb = request->accent_rgb;
      }
    if (!ArSdlStyledPaint_Apply(attempt->surface, attempt->pixel_owners,
                                attempt->reveal_clusters,
                                attempt->reveal_cluster_count, attempt->paint,
                                text, request->utf8_bytes)) {
      SetError(error, error_capacity, "cannot paint styled text");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    free(attempt->paint);
    attempt->paint = NULL;
  }
  if (!appearance &&
      (request->style_id == kArTextStyle_RetailBlueWhiteBands ||
       request->style_id == kArTextStyle_RetailPaletteBands) &&
      !ApplyRetailTextBands(attempt->surface, attempt->reveal_clusters,
                            attempt->reveal_cluster_count,
                            request->style_id == kArTextStyle_RetailPaletteBands
                                ? request->band_rgb
                                : 0x9cceff,
                            request->style_id == kArTextStyle_RetailPaletteBands
                                ? request->body_rgb
                                : 0xffffff)) {
    *failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity, "cannot apply retail text style");
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  if (!appearance &&
      !ArTextBitmap_ApplyClusterAccent(
          attempt->surface->pixels, attempt->surface->w, attempt->surface->h,
          attempt->surface->pitch, kArRenderPixelFormat_Rgba8888,
          attempt->reveal_clusters, attempt->reveal_cluster_count,
          request->accent_end_utf8_byte, request->accent_rgb)) {
    *failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity, "cannot apply cluster accent");
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  /* After the bands, never before: the band pass recolours every pixel it
   * finds ink on, which would turn the shadow into more letter. */
  if (slant_numerals) {
    ArTextBitmap ink = {.pixels = attempt->surface->pixels,
        .width = attempt->surface->w, .height = attempt->surface->h,
        .pitch_bytes = attempt->surface->pitch, .format = kArRenderPixelFormat_Rgba8888,
        .reveal_clusters = attempt->reveal_clusters,
        .reveal_cluster_count = attempt->reveal_cluster_count,
        .pixel_owners = attempt->pixel_owners};
    if (!ArTextBitmap_SlantAsciiNumerals(&ink, text, request->utf8_bytes)) {
      SetError(error, error_capacity, "cannot apply numeral style");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }
  if (!appearance && request->shadow_enabled &&
      !ApplyRetailTextShadow(attempt->surface, font_pixels, request,
                             attempt->pixel_owners)) {
    *failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity, "cannot apply retail text shadow");
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  if (!(request->flags & kArTextRasterFlag_IncludeRevealClusters)) {
    free(attempt->pixel_owners);
    attempt->pixel_owners = NULL;
    free(attempt->reveal_clusters);
    attempt->reveal_clusters = NULL;
    attempt->reveal_cluster_count = 0;
  }

  int removed_left = 0;
  int removed_top = 0;
  const int owner_width = attempt->surface->w;
  if (request->flags & (kArTextRasterFlag_CropHorizontalWhitespace |
                        kArTextRasterFlag_CropVerticalWhitespace)) {
    SDL_Surface *cropped = CropWhitespace(
        attempt->surface, request->flags, &removed_left, &removed_top);
    if (!cropped) {
      SetError(error, error_capacity, "cannot crop rasterized text");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    if (cropped != attempt->surface)
      SDL_DestroySurface(attempt->surface);
    attempt->surface = cropped;
  }
  if (attempt->surface->w <= 0 || attempt->surface->h <= 0 ||
      attempt->surface->w > request->maximum_width ||
      attempt->surface->h > request->maximum_height) {
    DestroyRasterAttempt(attempt);
    *failure = kArTextRasterFailure_None;
    return kRasterAttempt_ExceedsBounds;
  }

  if (attempt->reveal_clusters) {
    uint32_t *remap = attempt->pixel_owners ? calloc(attempt->reveal_cluster_count+1, sizeof(*remap)) : NULL;
    if (attempt->pixel_owners && !remap) {
      SetError(error, error_capacity, "cannot crop text effect ownership");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
    attempt->reveal_cluster_count = ClampRevealClusters(
        attempt->reveal_clusters, attempt->reveal_cluster_count,
        removed_left, removed_top, attempt->surface->w, attempt->surface->h, remap);
    if (remap) {
      for (int y=0; y<attempt->surface->h; ++y)
        for (int x=0; x<attempt->surface->w; ++x)
          attempt->pixel_owners[(size_t)y*attempt->surface->w+x] = remap[
              attempt->pixel_owners[(size_t)(y+removed_top)*owner_width+x+removed_left]];
      free(remap);
    }
    if (!attempt->reveal_cluster_count) {
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity,
               "text reveal metadata lies outside the rasterized surface");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }
  attempt->ascent = TTF_GetFontAscent(set->primary) - removed_top;
  for (size_t i = 0; i < attempt->line_count; ++i) {
    attempt->lines[i].top -= removed_top;
    attempt->lines[i].baseline -= removed_top;
  }
  if (attempt->line_count)
    attempt->ascent = attempt->lines[0].baseline;
  attempt->descent = TTF_GetFontDescent(set->primary);
  attempt->line_advance = TTF_GetFontLineSkip(set->primary);
  attempt->font_pixels = font_pixels;
  attempt->crop_left = removed_left;
  attempt->crop_top = removed_top;
  *failure = kArTextRasterFailure_None;
  return kRasterAttempt_Success;
}

static bool Rasterize(void *context, const ArTextRasterRequest *request,
                      ArTextBitmap *out_bitmap, ArTextRasterFailure *out_failure,
                      char *error, size_t error_capacity) {
  SdlTextRasterizerState *state = (SdlTextRasterizerState *)context;
  *out_failure = kArTextRasterFailure_Deterministic;
  if (request->font_revision != state->font_revision ||
      strlen(state->font_stack_id) != request->font_stack_id_bytes ||
      memcmp(state->font_stack_id, request->font_stack_id,
             request->font_stack_id_bytes) != 0) {
    SetError(error, error_capacity, "unknown or stale SDL text font stack");
    return false;
  }
  if (request->style_id != kArTextStyle_PlainWhite &&
      request->style_id != kArTextStyle_RetailBlueWhiteBands &&
      request->style_id != kArTextStyle_RetailPaletteBands) {
    SetError(error, error_capacity, "unsupported SDL text style");
    return false;
  }

  char *text = PrepareText(request);
  if (!text) {
    *out_failure = kArTextRasterFailure_Retryable;
    SetError(error, error_capacity, "out of memory preparing UTF-8 text");
    return false;
  }
  RasterAttempt best = {0};
  RasterAttempt requested = {0};
  const RasterAttemptResult requested_result = RasterizeAtSize(
      state, request, text, request->font_pixels,
      &requested, out_failure, error, error_capacity);
  if (requested_result == kRasterAttempt_Error) {
    free(text);
    return false;
  }
  if (!request->appearance)
    WarnMissingGlyphs(state, text);
  if (requested_result == kRasterAttempt_Success) {
    best = requested;
  } else {
    int low = request->minimum_font_pixels;
    int high = request->font_pixels - 1;
    while (low <= high) {
      const int candidate = low + (high - low) / 2;
      RasterAttempt attempt = {0};
      const RasterAttemptResult result = RasterizeAtSize(
          state, request, text, candidate, &attempt, out_failure, error,
          error_capacity);
      if (result == kRasterAttempt_Error) {
        DestroyRasterAttempt(&best);
        free(text);
        return false;
      }
      if (result == kRasterAttempt_ExceedsBounds) {
        high = candidate - 1;
        continue;
      }
      DestroyRasterAttempt(&best);
      best = attempt;
      low = candidate + 1;
    }
  }
  if (request->appearance && best.surface)
    WarnMissingStyledGlyphs(state, text, request->utf8_bytes, best.font_uses,
                            best.font_use_count);
  free(text);
  if (!best.surface) {
    *out_failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity,
             "rasterized text exceeds requested bounds at minimum font size");
    return false;
  }

  SDL_Surface *surface = best.surface;
  ArTextRevealCluster *reveal_clusters = best.reveal_clusters;
  const size_t reveal_cluster_count = best.reveal_cluster_count;
  SdlTextBitmapToken *token = (SdlTextBitmapToken *)calloc(1, sizeof(*token));
  if (!token) {
    SDL_DestroySurface(surface);
    free(reveal_clusters);
    free(best.pixel_owners);
    free(best.lines);
    free(best.font_uses);
    *out_failure = kArTextRasterFailure_Retryable;
    SetError(error, error_capacity,
             "out of memory retaining rasterized text");
    return false;
  }
  token->surface = surface;
  token->reveal_clusters = reveal_clusters;
  token->pixel_owners = best.pixel_owners;
  token->lines = best.lines;
  token->font_uses = best.font_uses;

  *out_bitmap = (ArTextBitmap){
      .struct_size = sizeof(*out_bitmap),
      .abi_version = AR_TEXT_BITMAP_ABI_VERSION,
      .pixels = surface->pixels,
      .width = surface->w,
      .height = surface->h,
      .pitch_bytes = surface->pitch,
      .format = kArRenderPixelFormat_Rgba8888,
      .ascent = best.ascent,
      .descent = best.descent,
      .line_advance = best.line_advance,
      .reveal_clusters = reveal_clusters,
      .reveal_cluster_count = reveal_cluster_count,
      .token = (uintptr_t)token,
      .pixel_owners = best.pixel_owners,
      .paragraph_direction = best.paragraph_direction,
      .font_pixels = best.font_pixels,
      .crop_left = best.crop_left,
      .crop_top = best.crop_top,
      .lines = best.lines,
      .line_count = best.line_count,
      .font_uses = best.font_uses,
      .font_use_count = best.font_use_count,
  };
  return true;
}

static void ReleaseBitmap(void *context, ArTextBitmap *bitmap) {
  (void)context;
  if (bitmap && bitmap->token) {
    SdlTextBitmapToken *token = (SdlTextBitmapToken *)bitmap->token;
    SDL_DestroySurface(token->surface);
    free(token->reveal_clusters);
    free(token->pixel_owners);
    free(token->lines);
    free(token->font_uses);
    free(token);
  }
  if (bitmap) memset(bitmap, 0, sizeof(*bitmap));
}

static const ArTextRasterizerOps kOps = {
    .struct_size = sizeof(ArTextRasterizerOps),
    .abi_version = AR_TEXT_RASTERIZER_ABI_VERSION,
    .rasterize = Rasterize,
    .has_glyph = HasGlyph,
    .release_bitmap = ReleaseBitmap,
};

static void DestroyState(SdlTextRasterizerState *state) {
  if (!state) return;
  ArSdlTextFonts_Destroy(&state->fonts);
  free(state->font_stack_id);
  if (state->ttf_initialized) TTF_Quit();
  free(state);
}

bool ArSdlTextRasterizer_Init(
    ArSdlTextRasterizer *adapter,
    const ArSdlTextRasterizerConfig *config,
    char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = '\0';
  if (!adapter || !ArTextBackendConfig_IsValid(config) ||
      config->cached_size_capacity > kArSdlMaximumFontVariants) {
    SetError(error, error_capacity, "invalid SDL text rasterizer config");
    return false;
  }
  SdlTextRasterizerState *state =
      (SdlTextRasterizerState *)calloc(1, sizeof(*state));
  if (!state) {
    SetError(error, error_capacity, "out of memory creating text rasterizer");
    return false;
  }
  state->font_stack_id = CopyString(config->font_stack_id);
  state->font_revision = config->font_revision;
  if (!state->font_stack_id) {
    SetError(error, error_capacity, "out of memory copying font stack");
    DestroyState(state);
    return false;
  }
  const bool acquired =
      ArSdlTextFonts_Init(&state->fonts, config, error, error_capacity);
  if (!acquired || !TTF_Init()) {
    if (acquired) SetError(error, error_capacity, SDL_GetError());
    DestroyState(state);
    return false;
  }
  state->ttf_initialized = true;
  ArSdlTextRasterizer replacement = {0};
  if (!ArTextRasterizer_Init(&replacement.rasterizer, &kOps, state,
                             UINT64_C(6))) {
    DestroyState(state);
    SetError(error, error_capacity, "cannot initialize text rasterizer ABI");
    return false;
  }
  replacement.implementation = state;
  ArSdlTextRasterizer_Destroy(adapter);
  *adapter = replacement;
  return true;
}

void ArSdlTextRasterizer_Destroy(ArSdlTextRasterizer *adapter) {
  if (!adapter) return;
  DestroyState((SdlTextRasterizerState *)adapter->implementation);
  memset(adapter, 0, sizeof(*adapter));
}

#else

bool ArSdlTextRasterizer_Init(
    ArSdlTextRasterizer *adapter,
    const ArSdlTextRasterizerConfig *config,
    char *error, size_t error_capacity) {
  (void)config;
  (void)adapter;
  SetError(error, error_capacity,
           "SDL3_ttf text rasterizer is not available in this build");
  return false;
}

void ArSdlTextRasterizer_Destroy(ArSdlTextRasterizer *adapter) {
  if (adapter) memset(adapter, 0, sizeof(*adapter));
}

#endif

const ArTextRasterizer *ArSdlTextRasterizer_Get(
    const ArSdlTextRasterizer *adapter) {
  return adapter && ArTextRasterizer_IsReady(&adapter->rasterizer)
      ? &adapter->rasterizer : NULL;
}

#if AR_HAS_SDL3_TTF
static bool CreateBackendInstance(
    void *context, ArTextBackendInstance *instance,
    const ArTextBackendConfig *config,
    char *error, size_t error_capacity) {
  (void)context;
  ArSdlTextRasterizer adapter = {0};
  if (!ArSdlTextRasterizer_Init(
          &adapter, config, error, error_capacity))
    return false;
  instance->implementation = adapter.implementation;
  instance->rasterizer = adapter.rasterizer;
  return true;
}

static void DestroyBackendInstance(
    void *context, ArTextBackendInstance *instance) {
  (void)context;
  if (!instance) return;
  ArSdlTextRasterizer adapter = {
    .implementation = instance->implementation,
    .rasterizer = instance->rasterizer,
  };
  ArSdlTextRasterizer_Destroy(&adapter);
  instance->implementation = NULL;
  ArTextRasterizer_Reset(&instance->rasterizer);
}

static const ArTextBackendOps kBackendOps = {
  .struct_size = sizeof(kBackendOps),
  .abi_version = AR_TEXT_BACKEND_ABI_VERSION,
  .create = CreateBackendInstance,
  .destroy = DestroyBackendInstance,
};
#endif

void ArSdlTextBackend_Init(ArTextBackend *backend) {
  if (!backend) return;
#if AR_HAS_SDL3_TTF
  *backend = (ArTextBackend){
    .ops = &kBackendOps,
  };
#else
  *backend = (ArTextBackend){0};
#endif
}
