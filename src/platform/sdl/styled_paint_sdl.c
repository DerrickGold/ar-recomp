#if defined(AR_HAS_SDL3_TTF) && AR_HAS_SDL3_TTF
#include "platform/sdl/styled_paint_sdl.h"
#include "localization/text_rasterizer.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct InkRows {
  int first, last;
} InkRows;

static uint32_t ReadPixel(const SDL_Surface *surface, int x, int y) {
  uint32_t pixel;
  memcpy(&pixel,
         (const uint8_t *)surface->pixels + (size_t)y * surface->pitch +
             (size_t)x * 4,
         4);
  return pixel;
}
static void WritePixel(SDL_Surface *surface, int x, int y, uint32_t pixel) {
  memcpy((uint8_t *)surface->pixels + (size_t)y * surface->pitch +
             (size_t)x * 4,
         &pixel, 4);
}

static bool PaintInks(SDL_Surface *surface, const uint32_t *owners,
                      const ArSdlClusterPaint *paint, size_t count) {
  InkRows *rows = malloc(count * sizeof(*rows));
  if (!rows)
    return false;
  for (size_t i = 0; i < count; ++i)
    rows[i] = (InkRows){INT_MAX, -1};
  for (int y = 0; y < surface->h; ++y)
    for (int x = 0; x < surface->w; ++x) {
      const uint32_t owner = owners[(size_t)y * surface->w + x];
      if (owner > count) {
        free(rows);
        return false;
      }
      if (!owner)
        continue;
      InkRows *row = &rows[owner - 1];
      if (y < row->first)
        row->first = y;
      row->last = y;
    }
  for (int y = 0; y < surface->h; ++y)
    for (int x = 0; x < surface->w; ++x) {
      const uint32_t owner = owners[(size_t)y * surface->w + x];
      if (!owner)
        continue;
      const InkRows *row = &rows[owner - 1];
      const ArTextRunAppearance *appearance = &paint[owner - 1].appearance;
      const uint32_t color =
          ArTextStyle_BandColor(appearance->band_rgb, appearance->body_rgb,
                                y - row->first, row->last - row->first);
      WritePixel(surface, x, y,
                 (color << 8) | (ReadPixel(surface, x, y) & 255));
    }
  free(rows);
  return true;
}

static bool SlantNumerals(SDL_Surface *surface, uint32_t *owners,
                          const ArTextRevealCluster *clusters, size_t count,
                          const ArSdlClusterPaint *paint, const char *text,
                          size_t bytes) {
  bool needed = false;
  for (size_t i = 0; i < count; ++i)
    needed |=
        paint[i].appearance.slant_ascii_numerals && !paint[i].appearance.italic;
  if (!needed)
    return true;
  /* Keep source offsets unchanged while suppressing numeral detection in
   * scopes whose policy is upright. Values and markup are never reparsed. */
  char *eligible = malloc(bytes);
  if (!eligible)
    return false;
  memcpy(eligible, text, bytes);
  for (size_t i = 0; i < count; ++i) {
    if (paint[i].appearance.slant_ascii_numerals && !paint[i].appearance.italic)
      continue;
    for (size_t j = paint[i].source_start;
         j < clusters[i].end_utf8_byte && j < bytes; ++j)
      if (eligible[j] >= '0' && eligible[j] <= '9')
        eligible[j] = 'X';
  }
  ArTextBitmap bitmap = {.pixels = surface->pixels,
                         .width = surface->w,
                         .height = surface->h,
                         .pitch_bytes = surface->pitch,
                         .format = kArRenderPixelFormat_Rgba8888,
                         .reveal_clusters = clusters,
                         .reveal_cluster_count = count,
                         .pixel_owners = owners};
  const bool ok = ArTextBitmap_SlantAsciiNumerals(&bitmap, eligible, bytes);
  free(eligible);
  return ok;
}

static bool PaintShadows(SDL_Surface *surface, uint32_t *owners,
                         const ArSdlClusterPaint *paint) {
  /* Emit behind the original ink. Separate coverage avoids one treatment's
   * shadow becoming another treatment's source, even at intersecting runs. */
  const size_t area = (size_t)surface->w * surface->h;
  uint32_t *shadow = calloc(area, sizeof(*shadow));
  uint32_t *shadow_owner = calloc(area, sizeof(*shadow_owner));
  if (!shadow || !shadow_owner) {
    free(shadow);
    free(shadow_owner);
    return false;
  }
  for (int y = 0; y < surface->h; ++y)
    for (int x = 0; x < surface->w; ++x) {
      const uint32_t owner = owners[(size_t)y * surface->w + x];
      if (!owner)
        continue;
      const ArSdlClusterPaint *style = &paint[owner - 1];
      if (!style->appearance.shadow_enabled)
        continue;
      const int step =
          (style->pixels + 4) / 8 > 0 ? (style->pixels + 4) / 8 : 1;
      const uint32_t alpha = ReadPixel(surface, x, y) & 255;
      for (unsigned sample = 0;
           sample < (style->appearance.keyline_shadow ? 2u : 1u); ++sample) {
        const int dx = style->appearance.keyline_shadow && sample ? 0 : step;
        const int dy = style->appearance.keyline_shadow && !sample ? 0 : step;
        if (x + dx >= surface->w || y + dy >= surface->h) {
          free(shadow);
          free(shadow_owner);
          return SDL_SetError("styled shadow lies outside the measured bounds");
        }
        const size_t target = (size_t)(y + dy) * surface->w + x + dx;
        if (!owners[target] && alpha > (shadow[target] & 255)) {
          shadow[target] = (style->appearance.shadow_rgb << 8) | alpha;
          shadow_owner[target] = owner;
        }
      }
    }
  for (int y = 0; y < surface->h; ++y)
    for (int x = 0; x < surface->w; ++x) {
      const size_t at = (size_t)y * surface->w + x;
      if (shadow[at]) {
        WritePixel(surface, x, y, shadow[at]);
        owners[at] = shadow_owner[at];
      }
    }
  free(shadow);
  free(shadow_owner);
  return true;
}

bool ArSdlStyledPaint_Apply(SDL_Surface *surface, uint32_t *owners,
                            const ArTextRevealCluster *clusters, size_t count,
                            const ArSdlClusterPaint *paint, const char *text,
                            size_t bytes) {
  if (!surface || !owners || !clusters || !count || !paint)
    return false;
  if (!PaintInks(surface, owners, paint, count) ||
      !SlantNumerals(surface, owners, clusters, count, paint, text, bytes))
    return false;
  for (size_t i = 0; i < count; ++i)
    if (paint[i].appearance.shadow_enabled)
      return PaintShadows(surface, owners, paint);
  return true;
}
#endif
