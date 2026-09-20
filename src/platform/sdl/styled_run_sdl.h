#ifndef AR_PLATFORM_SDL_STYLED_RUN_H
#define AR_PLATFORM_SDL_STYLED_RUN_H

#include "platform/sdl/text_fonts_sdl.h"
#include <SDL3_ttf/SDL_textengine.h>

/* A directional/script run shaped in its full context for every font variant
 * it uses. Paint never creates a shaping boundary. Font-changing boundaries
 * must be complete shaped clusters in both adjacent variants. */
typedef struct ArSdlStyledCluster {
  size_t start, end;
  SDL_Rect rect;
  int advance, pixels;
  ArTextRunAppearance appearance;
  const ArSdlFontRole *role; /* Pinned until layout destruction. */
  uint32_t font_mask, missing_font_mask;
} ArSdlStyledCluster;

typedef struct ArSdlStyledGlyph {
  TTF_CopyOperation copy;
  size_t cluster;
} ArSdlStyledGlyph;

typedef struct ArSdlStyledRun {
  ArSdlStyledCluster *clusters;
  ArSdlStyledGlyph *glyphs;
  size_t cluster_count, glyph_count;
  int width, ascent, descent;
} ArSdlStyledRun;

const ArTextRunAppearance *
ArSdlTextAppearance_At(const ArTextRasterRequest *request, size_t local_offset);
bool ArSdlStyledRun_Create(ArSdlStyledRun *run, ArSdlTextFonts *fonts,
                           const char *text, size_t bytes,
                           const size_t *logical_offsets,
                           const ArTextRasterRequest *request, int base_pixels,
                           bool rtl, Uint32 script,
                           ArTextRasterFailure *failure);
void ArSdlStyledRun_Destroy(ArSdlStyledRun *run);
/* Paint an alpha mask using the glyph operations returned by SDL's public
 * text-engine interface. The fonts stay pinned until layout destruction. */
bool ArSdlStyledRun_Draw(const ArSdlStyledRun *run, SDL_Surface *surface, int x,
                         int y);

#endif
