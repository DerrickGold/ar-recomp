#ifndef AR_DIORAMA_APERTURE_H
#define AR_DIORAMA_APERTURE_H

#include <stdbool.h>

#include "render/render_types.h"

/* Constrains a projected regular grid to a second grid's outer aperture.
 * Boundary vertices match the aperture exactly. `transition_cells` blends
 * the correction through the requested number of interior grid rings so a
 * forward-shifted plane keeps its centre parallax without folding its edge.
 * Texture coordinates and colours are intentionally untouched. */
bool DioramaAperture_ConstrainGrid(
    ArRenderVertex2D *grid, const ArRenderVertex2D *aperture,
    int subdiv_x, int subdiv_y, float transition_cells);

#endif /* AR_DIORAMA_APERTURE_H */
