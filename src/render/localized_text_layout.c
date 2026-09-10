#include "render/localized_text_layout.h"

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

static bool TableColumns(ArLocalizationTextLayoutKind layout,
                        unsigned line, unsigned field_index, unsigned field_count,
                        unsigned *column, unsigned *next_column) {
  if (!column || !next_column || !field_count ||
      field_index >= field_count)
    return false;
  if (layout == kArLocalizationTextLayout_StatusCities) {
    /* City rows occupy five retail columns: name, population, growth,
     * level, and item count. Header rows reuse those anchors but allow the
     * Population heading and total to span adjacent data columns. */
    static const unsigned starts[] = {0, 10, 14, 20, 23, 26};
    if (field_count == 1) {
      *column = 0;
      *next_column = 26;
      return true;
    }
    static const unsigned two_field_slots[] = {0, 3, 5};
    static const unsigned three_field_slots[] = {0, 1, 3, 5};
    const unsigned *slots = NULL;
    if (field_count == 2) slots = two_field_slots;
    else if (field_count == 3) slots = three_field_slots;
    else if (field_count != 5)
      return false;
    if (slots) {
      *column = starts[slots[field_index]];
      *next_column = starts[slots[field_index + 1u]];
    } else {
      *column = starts[field_index];
      *next_column = starts[field_index + 1u];
    }
    return true;
  }
  if (layout == kArLocalizationTextLayout_StatusScore) {
    static const unsigned starts[] = {0, 15, 21, 26};
    if (field_count == 1) {
      *column = 0;
      *next_column = 26;
      return true;
    }
    if (field_count == 2) {
      static const unsigned slots[] = {0, 2, 3};
      *column = starts[slots[field_index]];
      *next_column = starts[slots[field_index + 1u]];
    } else if (field_count == 3) {
      *column = starts[field_index];
      *next_column = starts[field_index + 1u];
    } else {
      return false;
    }
    return true;
  }
  if (layout == kArLocalizationTextLayout_StatusMaster) {
    if (field_count == 1) {
      *column = line == 1 ? 9 : 0;
      *next_column = line >= 12 ? 5 : 12;
    } else if (field_count == 2 && (line == 7 || line == 9)) {
      *column = field_index ? 7 : 0;
      *next_column = field_index ? 12 : 7;
    } else if (field_count == 4 && (line == 3 || line == 5)) {
      static const unsigned starts[] = {0, 3, 7, 10, 12};
      *column = starts[field_index];
      *next_column = starts[field_index + 1];
    } else return false;
    return true;
  }
  if (layout == kArLocalizationTextLayout_FixedRows &&
      field_count == 1) {
    *column = 0;
    *next_column = 10;
    return true;
  }
  if (layout == kArLocalizationTextLayout_MessageSpeed) {
    if (line == 0 && field_count == 10) {
      *column = field_index;
      *next_column = field_index + 1;
    } else if (line == 2 && field_count == 3) {
      static const unsigned starts[] = {0, 4, 6, 10};
      *column = starts[field_index];
      *next_column = starts[field_index + 1];
    } else if (field_count == 1) {
      *column = 0;
      *next_column = 10;
    } else return false;
    return true;
  }
  return false;
}

bool ArLocalizedTextLayout_TableNumeric(
    ArLocalizationTextLayoutKind layout, unsigned line,
    unsigned field_index, unsigned field_count) {
  if (!field_count || field_index >= field_count) return false;
  return (layout == kArLocalizationTextLayout_StatusMaster &&
          (line == 1 || (field_index & 1u))) ||
      ((layout == kArLocalizationTextLayout_StatusCities ||
        layout == kArLocalizationTextLayout_StatusScore) &&
       ((line <= 1 && field_count > 1 && field_index + 1 == field_count) ||
        (line >= 7 && field_index > 0 &&
         !(layout == kArLocalizationTextLayout_StatusCities &&
           field_index == 2))));
}

bool ArLocalizedTextLayout_TableColumns(
    ArLocalizationTextLayoutKind layout, unsigned line, unsigned field_index,
    unsigned field_count, unsigned *column, unsigned *next_column) {
  if (!TableColumns(layout, line, field_index, field_count, column, next_column))
    return false;
  /* A right-aligned value must retain the native blank before its neighbor. */
  if (ArLocalizedTextLayout_TableNumeric(layout, line, field_index, field_count) &&
      field_index + 1 < field_count)
    --*next_column;
  return *next_column > *column;
}

ArTextHorizontalAlignment ArLocalizedTextLayout_TableAlignment(
    ArLocalizationTextLayoutKind layout, unsigned line, unsigned field_index,
    unsigned field_count, ArTextDirection direction) {
  if (layout == kArLocalizationTextLayout_MessageSpeed) {
    if (line == 0 && field_count == 10 && field_index < 10)
      return kArTextHorizontalAlignment_Center;
    if (line == 2 && field_count == 3)
      return field_index == 2 ? kArTextHorizontalAlignment_Trailing
                             : kArTextHorizontalAlignment_Leading;
  }
  return ArLocalizedTextLayout_TableNumeric(layout, line, field_index, field_count) ||
      direction == kArTextDirection_RightToLeft
      ? kArTextHorizontalAlignment_Trailing : kArTextHorizontalAlignment_Leading;
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
