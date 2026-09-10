#ifndef AR_RENDER_LOCALIZED_TEXT_LAYOUT_H
#define AR_RENDER_LOCALIZED_TEXT_LAYOUT_H

#include "render/text_surface_cache.h"
#include "localization/localization_frame.h"

/* Cell geometry arrives with the frame as an ArLocalizationTextGrid; this
 * module only shares measured space across a table's fitted columns. */
enum {
  kArTextTableMaximumColumns = 5,
  /* Upper bounds for a surface laid out on a uniform key pitch. */
  kArTextLayoutMaximumClusters = 1024,
  kArTextLayoutMaximumKeyColumns = 64,
};
typedef struct ArTextTableColumns {
  int left[kArTextTableMaximumColumns];
  int right[kArTextTableMaximumColumns];
  int gap;
} ArTextTableColumns;

/* Share space across an entire measured table. Keep preferred widths where
 * possible, reduce gutters before rejecting the font size, and never overlap
 * columns. All dimensions are output pixels; failure leaves output unchanged. */
bool ArLocalizedTextLayout_FitColumns(
    const int *minimum_widths, const int *preferred_widths, unsigned count,
    int width, int preferred_gap, int minimum_gap, ArTextTableColumns *out);
/* Smallest rectangle covering both, treating an empty one as absent. */
ArRenderRectI ArLocalizedTextLayout_UnionInk(ArRenderRectI a, ArRenderRectI b);
/* Center an object in the actual gap between two fitted labels. */
bool ArLocalizedTextLayout_CenterBetween(
    ArRenderRectI left_label, ArRenderRectI right_label,
    ArRenderRectI *object);
/* Align visible centers, retaining the object's size and horizontal anchor.
 * object_ink is source-local; reference is already in output coordinates. */
bool ArLocalizedTextLayout_CenterInkVertically(
    ArRenderRectI reference, ArRenderRectI object_ink, int source_height,
    ArRenderRectI *object);

/* Decorations follow shaped advances and font metrics, not square icon cells. */
bool ArLocalizedTextLayout_NameUnderline(
    const ArTextSurface *surface, const ArTextRevealCluster *cluster,
    ArRenderRectI text_destination, ArRenderRectI *underline);

/* Reveal whole shaped clusters through a logical UTF-8 boundary, retaining old
 * lines until new ink actually needs room. Offset is in software surface pixels. */
int ArLocalizedTextLayout_ScrollOffset(
    const ArTextSurface *surface, uint32_t revealed_utf8_bytes,
    int viewport_height, uint32_t *revealed_clusters);

/* Uniform key pitch. Fills `out_shifts` with the horizontal shift, in surface
 * pixels, that moves each cluster onto its key's column: the last
 * `trailing_lines` lines are read as rows of `columns` keys, and each key --
 * the run of clusters between two gutters of `separator` -- is centred on its
 * column. `first_key_center` and `key_pitch` describe those columns in surface
 * coordinates, so the caller reproduces the native cell pitch instead of
 * whatever width the shaper happened to produce. Clusters outside those lines,
 * and the gutters between keys, follow the key they belong to, each gutter
 * splitting between its two neighbours. Fails, leaving the text flowed, when a
 * keyed line does not hold exactly `columns` keys.
 *
 * Shaping is untouched: one pass still measures the whole text, so every key
 * keeps the same glyph size, and only placement is regularized. */
bool ArLocalizedTextLayout_KeyCellShifts(
    const ArTextSurface *surface, const char *utf8, size_t utf8_bytes,
    const char *separator, size_t separator_bytes, unsigned columns,
    unsigned trailing_lines, int first_key_center, int key_pitch,
    int *out_shifts, size_t capacity);

/* Clip equal-sized source/destination rectangles without changing the device's
 * clip state (the caller may already be compositing through PPU windows). */
bool ArLocalizedTextLayout_Clip(
    ArRenderRectI viewport, ArRenderRectI *source, ArRenderRectI *destination);

#endif
