#ifndef AR_STYLED_PAINT_SDL_H
#define AR_STYLED_PAINT_SDL_H
/* ArSdlStyledPaint: applies each text cluster's run appearance to a rendered
 * SDL text surface and records which cluster owns each pixel.
 * Phase: present (text rendering). */

#include "localization/text_rasterizer.h"
#include <SDL3/SDL.h>

typedef struct ArSdlClusterPaint {
  ArTextRunAppearance appearance;
  size_t source_start;
  int pixels;
} ArSdlClusterPaint;

bool ArSdlStyledPaint_Apply(SDL_Surface *surface, uint32_t *owners,
                            const ArTextRevealCluster *clusters, size_t count,
                            const ArSdlClusterPaint *paint, const char *text,
                            size_t bytes);

#endif
