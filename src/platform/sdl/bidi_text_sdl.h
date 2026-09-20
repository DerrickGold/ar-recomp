#ifndef AR_PLATFORM_SDL_BIDI_TEXT_H
#define AR_PLATFORM_SDL_BIDI_TEXT_H

#include "localization/text_rasterizer.h"
#include "platform/sdl/styled_paint_sdl.h"
#include "platform/sdl/text_fonts_sdl.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

/* Private CPU text-layout adapter. Text and cluster offsets remain logical
 * UTF-8; only shaped run positions are reordered. No game/render ABI imports
 * the bidi implementation or its font handles. */
typedef struct ArSdlBidiLayout ArSdlBidiLayout;

bool ArSdlBidiText_NeedsLayout(const char *text, size_t length,
                              ArTextDirection direction);
ArSdlBidiLayout *ArSdlBidiText_Create(TTF_Font *font, const char *text,
                                      const ArTextRasterRequest *request,
                                      ArTextRasterFailure *failure);
ArSdlBidiLayout *ArSdlBidiText_CreateStyled(ArSdlTextFonts *fonts,
                                            TTF_Font *strut, const char *text,
                                            const ArTextRasterRequest *request,
                                            int base_pixels,
                                            ArTextRasterFailure *failure);
void ArSdlBidiText_GetSize(const ArSdlBidiLayout *layout, int *width, int *height);
ArTextDirection ArSdlBidiText_GetDirection(const ArSdlBidiLayout *layout);
/* Copies actual font selections before the layout releases its pinned fonts. */
bool ArSdlBidiText_CopyFontUses(const ArSdlBidiLayout *layout,
                                ArTextFontUse **uses, size_t *count);
/* One paint record per sorted reveal cluster. The caller allocates count
 * records. */
void ArSdlBidiText_DescribePaint(const ArSdlBidiLayout *layout,
                                 const ArTextRevealCluster *clusters,
                                 size_t count, ArSdlClusterPaint *paint);
ArTextLineMetrics *ArSdlBidiText_CopyLines(const ArSdlBidiLayout *layout,
                                           size_t *count);
/* Render and reveal rectangles come from the same immutable shaped runs.
 * Returned allocations belong to the caller, as with the ordinary SDL path. */
SDL_Surface *ArSdlBidiText_Render(const ArSdlBidiLayout *layout,
    ArTextRevealCluster **clusters, size_t *cluster_count,
    ArTextRasterFailure *failure);
void ArSdlBidiText_Destroy(ArSdlBidiLayout *layout);

#endif
