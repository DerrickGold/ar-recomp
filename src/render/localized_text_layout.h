#ifndef AR_RENDER_LOCALIZED_TEXT_LAYOUT_H
#define AR_RENDER_LOCALIZED_TEXT_LAYOUT_H

#include "render/text_surface_cache.h"
#include "localization/localization_frame.h"

/* Native-preferred logical cells, also the fixed contract for cursor-driven
 * menus. Reports may redistribute their widths using FitColumns. Returned
 * columns are relative to the owned native region. */
bool ArLocalizedTextLayout_TableColumns(
    ArLocalizationTextLayoutKind layout, unsigned line, unsigned field_index,
    unsigned field_count, unsigned *column, unsigned *next_column);
bool ArLocalizedTextLayout_TableNumeric(
    ArLocalizationTextLayoutKind layout, unsigned line,
    unsigned field_index, unsigned field_count);
/* Physical alignment within fixed cells, independent of shaping direction. */
ArTextHorizontalAlignment ArLocalizedTextLayout_TableAlignment(
    ArLocalizationTextLayoutKind layout, unsigned line, unsigned field_index,
    unsigned field_count, ArTextDirection direction);

enum { kArTextTableMaximumColumns = 5 };
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

/* Clip equal-sized source/destination rectangles without changing the device's
 * clip state (the caller may already be compositing through PPU windows). */
bool ArLocalizedTextLayout_Clip(
    ArRenderRectI viewport, ArRenderRectI *source, ArRenderRectI *destination);

#endif
