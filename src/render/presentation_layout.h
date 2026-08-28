#ifndef AR_PRESENTATION_LAYOUT_H
#define AR_PRESENTATION_LAYOUT_H

#include <stdbool.h>

#include "render_types.h"

/* Logical content axes used by platform presentation APIs. The CRT pixel
 * aspect expresses one SNES pixel as 7:6; square-pixel output stays 1:1. */
void ArPresentationLayout_ResolveLogicalSize(
    bool crt_pixel_aspect, int visible_width, int visible_height,
    int *logical_width, int *logical_height);

/* Resolve the physical output rectangle occupied by game content. Stretch is
 * the only policy that may change the content aspect ratio; widescreen is
 * represented by visible_width and does not bypass aspect correction. */
ArRenderRectI ArPresentationLayout_ResolveViewport(
    int output_width, int output_height, bool stretch,
    bool crt_pixel_aspect, int visible_width, int visible_height);

#endif /* AR_PRESENTATION_LAYOUT_H */
