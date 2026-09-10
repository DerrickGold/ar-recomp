#ifndef AR_RENDER_TEXT_CELL_COMPOSITE_H
#define AR_RENDER_TEXT_CELL_COMPOSITE_H

#include <stddef.h>
#include <stdint.h>

#include "render/hud_layout.h"
#include "localization/text_cell_record.h"

enum {
  kArTextCellMaximumProjectedRegions = 4,
  kArTextCellMaximumChunkPieces = 64,
};

/* Project a tilemap claim through the PPU's cyclic BG scroll into visible
 * authentic-screen pixels. The +1 vertical fetch phase matches native tiled
 * BG sampling. Wrapped regions are returned as independent rectangles. */
size_t ArTextCellComposite_ProjectRegion(
    ArTextCellRegion region, unsigned map_width_tiles,
    unsigned map_height_tiles, uint16_t h_scroll, uint16_t v_scroll,
    unsigned visible_width, unsigned visible_height,
    ArRenderRectI projected[kArTextCellMaximumProjectedRegions]);

/* Remove every projected authentic-screen mask from one HUD chunk. Source and
 * destination edges are transformed from the chunk's immutable screen-space
 * mapping, so independently rounded pieces remain seam-free. SIZE_MAX means
 * invalid input/capacity failure; zero is a valid fully masked chunk. */
size_t ArTextCellComposite_SubtractMasks(
    const HudPresentationChunk *chunk,
    const ArRenderRectI *masks, size_t mask_count,
    HudPresentationChunk *pieces, size_t piece_capacity);

/* Project the visible intersection of an authentic-screen rectangle into the
 * output coordinates of a HUD chunk. */
bool ArTextCellComposite_ProjectToOutput(
    const HudPresentationChunk *chunk, ArRenderRectI screen_region,
    ArRenderRectI *output_region);

#endif /* AR_RENDER_TEXT_CELL_COMPOSITE_H */
