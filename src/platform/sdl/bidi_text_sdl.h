#ifndef AR_PLATFORM_SDL_BIDI_TEXT_H
#define AR_PLATFORM_SDL_BIDI_TEXT_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "localization/text_rasterizer.h"

/* Private CPU text-layout adapter. Text and cluster offsets remain logical
 * UTF-8; only shaped run positions are reordered. No game/render ABI imports
 * the bidi implementation or its font handles. */
typedef struct ArSdlBidiLayout ArSdlBidiLayout;

bool ArSdlBidiText_NeedsLayout(const char *text, size_t length,
                              ArTextDirection direction);
ArSdlBidiLayout *ArSdlBidiText_Create(TTF_Font *font, const char *text,
                                      const ArTextRasterRequest *request,
                                      ArTextRasterFailure *failure);
void ArSdlBidiText_GetSize(const ArSdlBidiLayout *layout, int *width, int *height);
ArTextDirection ArSdlBidiText_GetDirection(const ArSdlBidiLayout *layout);
/* Render and reveal rectangles come from the same immutable shaped runs.
 * Returned allocations belong to the caller, as with the ordinary SDL path. */
SDL_Surface *ArSdlBidiText_Render(const ArSdlBidiLayout *layout,
    ArTextRevealCluster **clusters, size_t *cluster_count,
    ArTextRasterFailure *failure);
void ArSdlBidiText_Destroy(ArSdlBidiLayout *layout);

#endif
