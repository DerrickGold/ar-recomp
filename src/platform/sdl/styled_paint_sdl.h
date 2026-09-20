#ifndef AR_PLATFORM_SDL_STYLED_PAINT_H
#define AR_PLATFORM_SDL_STYLED_PAINT_H

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
