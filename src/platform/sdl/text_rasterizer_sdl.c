#include "platform/sdl/text_rasterizer_sdl.h"
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

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

enum {
  kMaximumFallbackFonts = 16,
  kMaximumCachedFontSizes = 32,
  kGlyphCoverageCacheCapacity = 1024,
  kMissingGlyphWarningCapacity = 64,
};

typedef struct CachedFontSet {
  TTF_Font *primary;
  TTF_Font **fallbacks;
  int pixels;
  uint64_t last_use;
} CachedFontSet;

typedef struct SdlTextRasterizerState {
  char *font_stack_id;
  char *primary_path;
  char **fallback_paths;
  size_t fallback_count;
  uint64_t font_revision;
  CachedFontSet *font_sets;
  size_t font_set_capacity;
  uint64_t clock;
  bool ttf_initialized;
  uint32_t coverage_keys[kGlyphCoverageCacheCapacity];
  bool coverage_values[kGlyphCoverageCacheCapacity];
  uint32_t warned_scalars[kMissingGlyphWarningCapacity];
  size_t warning_count;
  bool warnings_saturated;
} SdlTextRasterizerState;

typedef struct SdlTextBitmapToken {
  SDL_Surface *surface;
  ArTextRevealCluster *reveal_clusters;
} SdlTextBitmapToken;

static char *CopyString(const char *source) {
  if (!source || !source[0]) return NULL;
  const size_t length = strlen(source);
  char *copy = (char *)malloc(length + 1u);
  if (copy) memcpy(copy, source, length + 1u);
  return copy;
}

static void CloseFontSet(CachedFontSet *set, size_t fallback_count) {
  if (!set) return;
  if (set->primary) {
    TTF_ClearFallbackFonts(set->primary);
    TTF_CloseFont(set->primary);
  }
  for (size_t i = 0; i < fallback_count; ++i) {
    if (set->fallbacks && set->fallbacks[i])
      TTF_CloseFont(set->fallbacks[i]);
  }
  free(set->fallbacks);
  memset(set, 0, sizeof(*set));
}

static bool OpenFontSet(SdlTextRasterizerState *state, CachedFontSet *set,
                        int pixels, char *error, size_t error_capacity) {
  CachedFontSet opened = {0};
  opened.pixels = pixels;
  opened.primary = TTF_OpenFont(state->primary_path, (float)pixels);
  if (!opened.primary) {
    SetError(error, error_capacity, SDL_GetError());
    return false;
  }
  if (state->fallback_count) {
    opened.fallbacks = (TTF_Font **)calloc(
        state->fallback_count, sizeof(*opened.fallbacks));
    if (!opened.fallbacks) {
      CloseFontSet(&opened, state->fallback_count);
      SetError(error, error_capacity, "out of memory opening fallback fonts");
      return false;
    }
  }
  for (size_t i = 0; i < state->fallback_count; ++i) {
    opened.fallbacks[i] = TTF_OpenFont(
        state->fallback_paths[i], (float)pixels);
    if (!opened.fallbacks[i] ||
        !TTF_AddFallbackFont(opened.primary, opened.fallbacks[i])) {
      CloseFontSet(&opened, state->fallback_count);
      SetError(error, error_capacity, SDL_GetError());
      return false;
    }
  }
  *set = opened;
  return true;
}

static CachedFontSet *AcquireFontSet(
    SdlTextRasterizerState *state, int pixels,
    char *error, size_t error_capacity) {
  ++state->clock;
  if (!state->clock) state->clock = 1;
  for (size_t i = 0; i < state->font_set_capacity; ++i) {
    CachedFontSet *set = &state->font_sets[i];
    if (set->primary && set->pixels == pixels) {
      set->last_use = state->clock;
      return set;
    }
  }
  size_t victim = 0;
  for (size_t i = 0; i < state->font_set_capacity; ++i) {
    if (!state->font_sets[i].primary) {
      victim = i;
      break;
    }
    if (state->font_sets[i].last_use < state->font_sets[victim].last_use)
      victim = i;
  }
  CachedFontSet replacement = {0};
  if (!OpenFontSet(state, &replacement, pixels, error, error_capacity))
    return NULL;
  replacement.last_use = state->clock;
  CloseFontSet(&state->font_sets[victim], state->fallback_count);
  state->font_sets[victim] = replacement;
  return &state->font_sets[victim];
}

static bool HasGlyph(void *context, uint32_t scalar, bool *provided,
                     char *error, size_t error_capacity) {
  SdlTextRasterizerState *state = context;
  const size_t slot = scalar % kGlyphCoverageCacheCapacity;
  if (state->coverage_keys[slot] == scalar + 1u) {
    *provided = state->coverage_values[slot];
    return true;
  }
  CachedFontSet *set = NULL;
  for (size_t i = 0; i < state->font_set_capacity; ++i)
    if (state->font_sets[i].primary) {
      set = &state->font_sets[i];
      break;
    }
  if (!set)
    set = AcquireFontSet(state, 24, error, error_capacity);
  if (!set)
    return false;
  /* SDL_ttf's query traverses the configured fallback chain too. */
  *provided = TTF_FontHasGlyph(set->primary, scalar);
  state->coverage_keys[slot] = scalar + 1u;
  state->coverage_values[slot] = *provided;
  return true;
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
    size_t i = 0;
    while (i < state->warning_count && state->warned_scalars[i] != scalar)
      ++i;
    if (i < state->warning_count)
      continue;
    /* Name something the player actually has. tools/check_language_fonts.py
     * is a development reference and is not part of a distribution, so it is
     * not what a runtime warning should send anyone to. */
    if (state->warning_count == kMissingGlyphWarningCapacity) {
      fprintf(stderr,
              "[localized-text] further missing-character warnings for font "
              "stack '%s' are suppressed\n", state->font_stack_id);
      state->warnings_saturated = true;
      return;
    }
    state->warned_scalars[state->warning_count++] = scalar;
    fprintf(stderr,
            "[localized-text] font stack '%s' has no glyph for U+%04X; "
            "a replacement box may be shown. The language pack needs a font "
            "covering this character -- add one under Fonts in the builder's "
            "Languages workspace.\n",
            state->font_stack_id, (unsigned)scalar);
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
    SDL_Surface *surface, int font_pixels, uint32_t shadow_rgb) {
  if (!surface || surface->format != SDL_PIXELFORMAT_RGBA8888) return false;
  int step = (font_pixels + 4) / 8;
  if (step < 1) step = 1;
  return ArTextBitmap_ApplyStyleShadow(
      surface->pixels, surface->w, surface->h, surface->pitch,
      kArRenderPixelFormat_Rgba8888, step, step, shadow_rgb);
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
  int first = surface->w;
  int last = -1;
  int top = surface->h;
  int bottom = -1;
  for (int y = 0; y < surface->h; ++y) {
    const uint8_t *row =
        (const uint8_t *)surface->pixels + (size_t)y * surface->pitch;
    for (int x = 0; x < surface->w; ++x) {
      uint32_t pixel;
      uint8_t alpha;
      memcpy(&pixel, row + (size_t)x * 4u, sizeof(pixel));
      SDL_GetRGBA(pixel, details, NULL, NULL, NULL, NULL, &alpha);
      if (!alpha) continue;
      if (x < first) first = x;
      if (x > last) last = x;
      if (y < top) top = y;
      if (y > bottom) bottom = y;
    }
  }
  if (last < first) return NULL;
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
                                  int removed_top, int width, int height) {
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

static char *PrepareText(const ArTextRasterRequest *request) {
  char *text = (char *)malloc(request->utf8_bytes + 1u);
  if (!text) return NULL;
  memcpy(text, request->utf8, request->utf8_bytes);
  text[request->utf8_bytes] = '\0';
  if (!(request->flags & kArTextRasterFlag_PreserveHardBreaks)) {
    for (size_t i = 0; i < request->utf8_bytes; ++i) {
      if (text[i] == '\r' || text[i] == '\n') text[i] = ' ';
    }
  }
  return text;
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
  size_t reveal_cluster_count;
  int ascent;
  int descent;
  int line_advance;
} RasterAttempt;

static void DestroyRasterAttempt(RasterAttempt *attempt) {
  if (!attempt) return;
  SDL_DestroySurface(attempt->surface);
  free(attempt->reveal_clusters);
  memset(attempt, 0, sizeof(*attempt));
}

/* Every error exit classifies itself. Opening/sizing a font, rendering,
 * converting and cropping all allocate, so those failures are the ones a later
 * identical request can survive; the rest are properties of the request. */
static RasterAttemptResult RasterizeAtSize(
    SdlTextRasterizerState *state, const ArTextRasterRequest *request,
    const char *text, int font_pixels, RasterAttempt *attempt,
    ArTextRasterFailure *failure, char *error, size_t error_capacity) {
  memset(attempt, 0, sizeof(*attempt));
  /* Classification for the error exits below; the non-error returns clear it,
   * so a stale value never reaches a caller. */
  *failure = kArTextRasterFailure_Retryable;
  CachedFontSet *set = AcquireFontSet(
      state, font_pixels, error, error_capacity);
  if (!set || !ConfigureFontForRequest(
          set->primary, request, error, error_capacity))
    return kRasterAttempt_Error;
  const TTF_FontStyleFlags style = request->flags & kArTextRasterFlag_Italic
      ? TTF_STYLE_ITALIC : TTF_STYLE_NORMAL;
  TTF_SetFontStyle(set->primary, style);
  for (size_t i = 0; i < state->fallback_count; ++i)
    TTF_SetFontStyle(set->fallbacks[i], style);

  /* Measure before rendering. A fixed field disables wrapping, so an accepted
   * but far too long label would otherwise be rasterized at its full width and
   * only then rejected. Cropping can only shrink the result, so the early exit
   * is limited to the axes this request is not asking to crop, plus an
   * absolute ceiling that no request may exceed. */
  int measured_w = 0, measured_h = 0;
  const bool measured =
      request->flags & kArTextRasterFlag_WrapWords
      ? TTF_GetStringSizeWrapped(set->primary, text, request->utf8_bytes,
                                 request->maximum_width, &measured_w,
                                 &measured_h)
      : TTF_GetStringSize(set->primary, text, request->utf8_bytes, &measured_w,
                          &measured_h);
  if (measured) {
    const bool crops_width =
        (request->flags & kArTextRasterFlag_CropHorizontalWhitespace) != 0;
    const bool crops_height =
        (request->flags & kArTextRasterFlag_CropVerticalWhitespace) != 0;
    if ((!crops_width && measured_w > request->maximum_width) ||
        (!crops_height && measured_h > request->maximum_height)) {
      DestroyRasterAttempt(attempt);
      *failure = kArTextRasterFailure_None;
      return kRasterAttempt_ExceedsBounds;
    }
    if (measured_w > 0 && measured_h > 0 &&
        (int64_t)measured_w * measured_h * 4 > kMaximumRasterBytes) {
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity,
               "rasterized text would exceed the per-request size ceiling");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }

  const SDL_Color white = {255, 255, 255, 255};
  SDL_Surface *rendered =
      request->flags & kArTextRasterFlag_WrapWords
      ? TTF_RenderText_Blended_Wrapped(
            set->primary, text, request->utf8_bytes, white,
            request->maximum_width)
      : TTF_RenderText_Blended(
            set->primary, text, request->utf8_bytes, white);
  const bool need_cluster_layout =
      request->style_id == kArTextStyle_RetailBlueWhiteBands ||
      request->style_id == kArTextStyle_RetailPaletteBands ||
      (request->flags & kArTextRasterFlag_IncludeRevealClusters);
  if (rendered && need_cluster_layout &&
      !BuildRevealClusters(
          set->primary, text, request,
          &attempt->reveal_clusters, &attempt->reveal_cluster_count,
          error, error_capacity)) {
    SDL_DestroySurface(rendered);
    rendered = NULL;
  }
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
  if ((request->style_id == kArTextStyle_RetailBlueWhiteBands ||
       request->style_id == kArTextStyle_RetailPaletteBands) &&
      !ApplyRetailTextBands(
          attempt->surface, attempt->reveal_clusters,
          attempt->reveal_cluster_count,
          request->style_id == kArTextStyle_RetailPaletteBands ? request->band_rgb : 0x9cceff,
          request->style_id == kArTextStyle_RetailPaletteBands ? request->body_rgb : 0xffffff)) {
    *failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity, "cannot apply retail text style");
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  /* After the bands, never before: the band pass recolours every pixel it
   * finds ink on, which would turn the shadow into more letter. */
  if (request->shadow_enabled &&
      !ApplyRetailTextShadow(attempt->surface, font_pixels,
                             request->shadow_rgb)) {
    *failure = kArTextRasterFailure_Deterministic;
    SetError(error, error_capacity, "cannot apply retail text shadow");
    DestroyRasterAttempt(attempt);
    return kRasterAttempt_Error;
  }
  if (!(request->flags & kArTextRasterFlag_IncludeRevealClusters)) {
    free(attempt->reveal_clusters);
    attempt->reveal_clusters = NULL;
    attempt->reveal_cluster_count = 0;
  }

  int removed_left = 0;
  int removed_top = 0;
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
    attempt->reveal_cluster_count = ClampRevealClusters(
        attempt->reveal_clusters, attempt->reveal_cluster_count,
        removed_left, removed_top, attempt->surface->w, attempt->surface->h);
    if (!attempt->reveal_cluster_count) {
      *failure = kArTextRasterFailure_Deterministic;
      SetError(error, error_capacity,
               "text reveal metadata lies outside the rasterized surface");
      DestroyRasterAttempt(attempt);
      return kRasterAttempt_Error;
    }
  }
  attempt->ascent = TTF_GetFontAscent(set->primary) - removed_top;
  attempt->descent = TTF_GetFontDescent(set->primary);
  attempt->line_advance = TTF_GetFontLineSkip(set->primary);
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
    *out_failure = kArTextRasterFailure_Retryable;
    SetError(error, error_capacity,
             "out of memory retaining rasterized text");
    return false;
  }
  token->surface = surface;
  token->reveal_clusters = reveal_clusters;

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
  };
  return true;
}

static void ReleaseBitmap(void *context, ArTextBitmap *bitmap) {
  (void)context;
  if (bitmap && bitmap->token) {
    SdlTextBitmapToken *token = (SdlTextBitmapToken *)bitmap->token;
    SDL_DestroySurface(token->surface);
    free(token->reveal_clusters);
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
  for (size_t i = 0; i < state->font_set_capacity; ++i)
    CloseFontSet(&state->font_sets[i], state->fallback_count);
  free(state->font_sets);
  for (size_t i = 0; i < state->fallback_count; ++i)
    free(state->fallback_paths[i]);
  free(state->fallback_paths);
  free(state->font_stack_id);
  free(state->primary_path);
  if (state->ttf_initialized) TTF_Quit();
  free(state);
}

bool ArSdlTextRasterizer_Init(
    ArSdlTextRasterizer *adapter,
    const ArSdlTextRasterizerConfig *config,
    char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = '\0';
  if (!adapter || !config ||
      config->struct_size < AR_MEMBER_END(
          ArSdlTextRasterizerConfig, cached_size_capacity) ||
      config->abi_version != AR_SDL_TEXT_RASTERIZER_CONFIG_ABI_VERSION ||
      !config->font_stack_id || !config->font_stack_id[0] ||
      !config->primary_font_path || !config->primary_font_path[0] ||
      !config->font_revision ||
      config->fallback_font_count > kMaximumFallbackFonts ||
      (config->fallback_font_count && !config->fallback_font_paths) ||
      !config->cached_size_capacity ||
      config->cached_size_capacity > kMaximumCachedFontSizes) {
    SetError(error, error_capacity, "invalid SDL text rasterizer config");
    return false;
  }
  memset(adapter, 0, sizeof(*adapter));
  SdlTextRasterizerState *state =
      (SdlTextRasterizerState *)calloc(1, sizeof(*state));
  if (!state) {
    SetError(error, error_capacity, "out of memory creating text rasterizer");
    return false;
  }
  state->font_stack_id = CopyString(config->font_stack_id);
  state->primary_path = CopyString(config->primary_font_path);
  state->fallback_count = config->fallback_font_count;
  state->font_revision = config->font_revision;
  state->font_set_capacity = config->cached_size_capacity;
  state->font_sets = (CachedFontSet *)calloc(
      state->font_set_capacity, sizeof(*state->font_sets));
  if (state->fallback_count)
    state->fallback_paths = (char **)calloc(
        state->fallback_count, sizeof(*state->fallback_paths));
  bool copied = state->font_stack_id && state->primary_path &&
      state->font_sets &&
      (!state->fallback_count || state->fallback_paths);
  for (size_t i = 0; copied && i < state->fallback_count; ++i) {
    state->fallback_paths[i] = CopyString(config->fallback_font_paths[i]);
    copied = state->fallback_paths[i] != NULL;
  }
  if (!copied || !TTF_Init()) {
    SetError(error, error_capacity,
             copied ? SDL_GetError() : "out of memory copying font stack");
    DestroyState(state);
    return false;
  }
  state->ttf_initialized = true;
  if (!ArTextRasterizer_Init(
          &adapter->rasterizer, &kOps, state, UINT64_C(3))) {
    DestroyState(state);
    SetError(error, error_capacity, "cannot initialize text rasterizer ABI");
    return false;
  }
  adapter->implementation = state;
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
  if (adapter) memset(adapter, 0, sizeof(*adapter));
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

void ArSdlTextBackend_Init(ArTextBackend *backend) {
  if (!backend) return;
  *backend = (ArTextBackend){
    .ops = &kBackendOps,
  };
}
