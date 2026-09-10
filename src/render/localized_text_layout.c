#include "render/localized_text_layout.h"

#include <string.h>

bool ArLocalizedTextLayout_FitColumns(
    const int *minimum_widths, const int *preferred_widths, unsigned count,
    int width, int preferred_gap, int minimum_gap, ArTextTableColumns *out) {
  if (!minimum_widths || !preferred_widths || !out || !count ||
      count > kArTextTableMaximumColumns || width <= 0 || minimum_gap < 0 ||
      preferred_gap < minimum_gap)
    return false;
  int64_t minimum = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (minimum_widths[i] <= 0 || preferred_widths[i] <= 0) return false;
    minimum += minimum_widths[i];
  }
  if (minimum + (int64_t)(count - 1) * minimum_gap > width) return false;
  const int maximum_gap = count > 1 ? (int)((width - minimum) / (count - 1)) : 0;
  const int gap = preferred_gap < maximum_gap ? preferred_gap : maximum_gap;
  int remaining = (int)(width - minimum - (int64_t)(count - 1) * gap);
  int64_t weights[kArTextTableMaximumColumns] = {0}, weight_sum = 0;
  for (unsigned i = 0; i < count; ++i) {
    const int desired = preferred_widths[i] - (i + 1 < count ? gap : 0);
    weights[i] = desired > minimum_widths[i] ? desired - minimum_widths[i] : 0;
    weight_sum += weights[i];
  }
  /* If every preferred width is already full, distribute remaining slack
   * evenly rather than leaving the final column as an accidental sink. */
  if (!weight_sum) {
    for (unsigned i = 0; i < count; ++i) weights[i] = 1;
    weight_sum = count;
  }
  ArTextTableColumns result = {.gap = gap};
  int x = 0;
  for (unsigned i = 0; i < count; ++i) {
    const int extra = weight_sum ? (int)((int64_t)remaining * weights[i] / weight_sum) : 0;
    result.left[i] = x;
    result.right[i] = x + minimum_widths[i] + extra;
    x = result.right[i] + (i + 1 < count ? gap : 0);
    remaining -= extra;
    weight_sum -= weights[i];
  }
  *out = result;
  return true;
}

ArRenderRectI ArLocalizedTextLayout_UnionInk(ArRenderRectI a, ArRenderRectI b) {
  if (!a.w || !a.h) return b;
  if (!b.w || !b.h) return a;
  const int left = a.x < b.x ? a.x : b.x;
  const int top = a.y < b.y ? a.y : b.y;
  const int right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  const int bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return (ArRenderRectI){left, top, right - left, bottom - top};
}

bool ArLocalizedTextLayout_CenterBetween(
    ArRenderRectI left_label, ArRenderRectI right_label,
    ArRenderRectI *object) {
  if (!object || object->w <= 0 || left_label.w <= 0 || right_label.w <= 0)
    return false;
  const int left_edge = left_label.x + left_label.w;
  const int gap = right_label.x - left_edge;
  if (gap < object->w) return false;
  object->x = left_edge + (gap - object->w) / 2;
  return true;
}

bool ArLocalizedTextLayout_CenterInkVertically(
    ArRenderRectI reference, ArRenderRectI object_ink, int source_height,
    ArRenderRectI *object) {
  if (!object || reference.h <= 0 || object->h <= 0 || source_height <= 0 ||
      object_ink.y < 0 || object_ink.h <= 0 || object_ink.h > source_height ||
      object_ink.y > source_height - object_ink.h)
    return false;
  const int64_t source_center = (int64_t)object_ink.y * 2 + object_ink.h;
  const int64_t scaled_center =
      (source_center * object->h + source_height / 2) / source_height;
  const int64_t y = ((int64_t)reference.y * 2 + reference.h - scaled_center) / 2;
  if (y < INT32_MIN || y > INT32_MAX) return false;
  object->y = (int)y;
  return true;
}

bool ArLocalizedTextLayout_NameUnderline(
    const ArTextSurface *surface, const ArTextRevealCluster *cluster,
    ArRenderRectI text_destination, ArRenderRectI *underline) {
  if (!surface || !cluster || !underline || cluster->width <= 0 ||
      surface->line_advance <= 0 || cluster->line_index < 0)
    return false;
  int thickness = surface->line_advance / 16;
  if (thickness < 1) thickness = 1;
  int inset = cluster->width / 10;
  /* A blank logical row after the name reserves space below the descenders.
   * Its baseline is identical for I, W, g, combining accents and empty slots. */
  *underline = (ArRenderRectI){
      text_destination.x + cluster->x + inset,
      text_destination.y + cluster->line_index * surface->line_advance +
          surface->ascent - surface->descent + thickness,
      cluster->width - 2 * inset, 2 * thickness};
  return true;
}

int ArLocalizedTextLayout_ScrollOffset(
    const ArTextSurface *surface, uint32_t revealed_utf8_bytes,
    int viewport_height, uint32_t *revealed_clusters) {
  if (revealed_clusters) *revealed_clusters = 0;
  if (!surface || !revealed_clusters || viewport_height <= 0 ||
      surface->line_advance <= 0 || !surface->reveal_clusters)
    return 0;
  int bottom = 0;
  for (size_t index = 0; index < surface->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *cluster = &surface->reveal_clusters[index];
    if (cluster->end_utf8_byte > revealed_utf8_bytes) break;
    *revealed_clusters = (uint32_t)(index + 1u);
    if (cluster->y + cluster->height > bottom)
      bottom = cluster->y + cluster->height;
  }
  if (bottom <= viewport_height) return 0;
  const int advance = surface->line_advance;
  return ((bottom - viewport_height + advance - 1) / advance) * advance;
}

/* A cluster is a gutter when its own bytes are exactly the blank the surface
 * declared between keys. Ink cannot decide this: the finish and backspace keys
 * are native artwork drawn over placeholder characters that put no ink on the
 * surface, and grouping by ink alone drops them from their row. */
static bool ClusterIsSeparator(const ArTextSurface *surface, size_t index,
                               size_t previous_end, const char *utf8,
                               size_t utf8_bytes, const char *separator,
                               size_t separator_bytes) {
  const size_t end = surface->reveal_clusters[index].end_utf8_byte;
  return end <= utf8_bytes && end >= previous_end &&
      end - previous_end == separator_bytes &&
      memcmp(utf8 + previous_end, separator, separator_bytes) == 0;
}

/* Where a key sits on the surface. Its ink when it has any, and the shaped
 * advance box when it does not, so an artwork key still claims its column. */
static ArRenderRectI ClusterExtent(const ArTextSurface *surface, size_t index) {
  const ArRenderRectI ink = surface->cluster_ink_bounds[index];
  if (ink.w > 0 && ink.h > 0) return ink;
  const ArTextRevealCluster *cluster = &surface->reveal_clusters[index];
  return (ArRenderRectI){cluster->x, cluster->y, cluster->width,
                         cluster->height};
}

bool ArLocalizedTextLayout_KeyCellShifts(
    const ArTextSurface *surface, const char *utf8, size_t utf8_bytes,
    const char *separator, size_t separator_bytes, unsigned columns,
    unsigned trailing_lines, int first_key_center, int key_pitch,
    int *out_shifts, size_t capacity) {
  if (!surface || !surface->reveal_clusters || !surface->cluster_ink_bounds ||
      !utf8 || !utf8_bytes || !separator || !separator_bytes || !out_shifts ||
      !columns || columns > kArTextLayoutMaximumKeyColumns || !trailing_lines ||
      key_pitch <= 0 || surface->reveal_cluster_count > capacity ||
      surface->reveal_cluster_count > kArTextLayoutMaximumClusters)
    return false;
  const size_t count = surface->reveal_cluster_count;
  if (!count) return false;
  int last_line = -1;
  for (size_t index = 0; index < count; ++index) {
    if (surface->reveal_clusters[index].line_index > last_line)
      last_line = surface->reveal_clusters[index].line_index;
  }
  if (last_line < 0 || (unsigned)(last_line + 1) < trailing_lines) return false;
  /* Cluster ends are monotonic, so one sweep gives each cluster the byte it
   * starts at without searching the text again per line. */
  size_t starts[kArTextLayoutMaximumClusters];
  size_t previous_end = 0;
  for (size_t index = 0; index < count; ++index) {
    starts[index] = previous_end;
    const size_t end = surface->reveal_clusters[index].end_utf8_byte;
    previous_end = end > previous_end ? end : previous_end;
  }
  for (size_t index = 0; index < count; ++index) out_shifts[index] = 0;
  const int first_keyed = (last_line + 1) - (int)trailing_lines;
  for (int line = first_keyed; line <= last_line; ++line) {
    size_t line_first = 0;
    while (line_first < count &&
           surface->reveal_clusters[line_first].line_index != line)
      ++line_first;
    size_t line_end = line_first;
    while (line_end < count &&
           surface->reveal_clusters[line_end].line_index == line)
      ++line_end;

    /* Find the keys: runs of clusters between the gutters. */
    size_t key_first[kArTextLayoutMaximumKeyColumns];
    size_t key_end[kArTextLayoutMaximumKeyColumns];
    int key_shift[kArTextLayoutMaximumKeyColumns];
    unsigned key_count = 0;
    size_t index = line_first;
    while (index < line_end) {
      if (ClusterIsSeparator(surface, index, starts[index], utf8, utf8_bytes,
                             separator, separator_bytes)) {
        ++index;
        continue;
      }
      if (key_count >= columns) return false;
      const size_t first = index;
      ArRenderRectI extent = ClusterExtent(surface, index);
      int left = extent.x;
      int right = extent.x + extent.w;
      while (index < line_end &&
             !ClusterIsSeparator(surface, index, starts[index], utf8,
                                 utf8_bytes, separator, separator_bytes)) {
        extent = ClusterExtent(surface, index);
        if (extent.x < left) left = extent.x;
        if (extent.x + extent.w > right) right = extent.x + extent.w;
        ++index;
      }
      key_first[key_count] = first;
      key_end[key_count] = index;
      key_shift[key_count] =
          first_key_center + (int)key_count * key_pitch - (left + right) / 2;
      ++key_count;
    }
    if (key_count != columns) return false;

    /* Every cluster of the line moves with a key, gutters included, and each
     * gutter is split down the middle between the keys it separates. A glyph
     * whose ink overhangs its own advance box then stays inside one shifted
     * run, instead of being cut at the box edge and leaving a stray fragment
     * behind at the neighbour's offset. */
    for (unsigned key = 0; key < key_count; ++key) {
      const size_t from = key ? (key_end[key - 1u] + key_first[key]) / 2u
                              : line_first;
      const size_t to = key + 1u < key_count
          ? (key_end[key] + key_first[key + 1u]) / 2u
          : line_end;
      for (size_t member = from; member < to; ++member)
        out_shifts[member] = key_shift[key];
    }
  }
  return true;
}

bool ArLocalizedTextLayout_Clip(
    ArRenderRectI viewport, ArRenderRectI *source, ArRenderRectI *destination) {
  if (!source || !destination || viewport.w <= 0 || viewport.h <= 0 ||
      source->w != destination->w || source->h != destination->h)
    return false;
  const int left = destination->x > viewport.x ? destination->x : viewport.x;
  const int top = destination->y > viewport.y ? destination->y : viewport.y;
  const int right = destination->x + destination->w < viewport.x + viewport.w
      ? destination->x + destination->w : viewport.x + viewport.w;
  const int bottom = destination->y + destination->h < viewport.y + viewport.h
      ? destination->y + destination->h : viewport.y + viewport.h;
  if (right <= left || bottom <= top) return false;
  source->x += left - destination->x;
  source->y += top - destination->y;
  source->w = right - left;
  source->h = bottom - top;
  *destination = (ArRenderRectI){left, top, right - left, bottom - top};
  return true;
}
