#include "actraiser/actraiser_localization_grid.h"

#include <string.h>

/* Native cell geometry of ActRaiser's fixed menus, moved here from the
 * renderer. Columns are native cell units relative to the owned region. */

static bool MenuColumns(ActRaiserLocalizationMenu menu, unsigned line,
                        unsigned field_index, unsigned field_count,
                        unsigned *column, unsigned *next_column) {
  if (!column || !next_column || !field_count || field_index >= field_count)
    return false;
  if (!ArLanguageRowShape_Allows((ArLanguageRowShape)menu, line, field_count))
    return false;
  if (menu == kActRaiserLocalizationMenu_StatusCities) {
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
  if (menu == kActRaiserLocalizationMenu_StatusScore) {
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
  if (menu == kActRaiserLocalizationMenu_StatusMaster) {
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
  if (menu == kActRaiserLocalizationMenu_SoundTest) {
    if (field_count == 1) {
      *column = 0;
      *next_column = 10;
    } else if (field_count == 2) {
      /* Stable counter anchors despite proportional/translated label widths.
       * Preserve the modal's blank first column on its value rows. */
      *column = field_index ? 7 : 1;
      *next_column = field_index ? 9 : 6;
    } else return false;
    return true;
  }
  if (menu == kActRaiserLocalizationMenu_FixedRows && field_count == 1) {
    *column = 0;
    *next_column = 10;
    return true;
  }
  if (menu == kActRaiserLocalizationMenu_MessageSpeed || menu == kActRaiserLocalizationMenu_MessageSpeedJP) {
    const bool short_range = menu == kActRaiserLocalizationMenu_MessageSpeedJP;
    if (line == 0 && field_count == (short_range ? 8u : 10u)) {
      *column = field_index + short_range;
      *next_column = *column + 1;
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

/* A value cell: right aligned, and it keeps the native blank before its
 * neighbour. */
static bool MenuValueCell(ActRaiserLocalizationMenu menu, unsigned line,
                          unsigned field_index, unsigned field_count) {
  if (!field_count || field_index >= field_count) return false;
  return (menu == kActRaiserLocalizationMenu_StatusMaster &&
          (line == 1 || (field_index & 1u))) ||
      (menu == kActRaiserLocalizationMenu_SoundTest && field_index == 1) ||
      ((menu == kActRaiserLocalizationMenu_StatusCities ||
        menu == kActRaiserLocalizationMenu_StatusScore) &&
       ((line <= 1 && field_count > 1 && field_index + 1 == field_count) ||
        (line >= 7 && field_index > 0 &&
         !(menu == kActRaiserLocalizationMenu_StatusCities &&
           field_index == 2))));
}

/* Physical cell positions that ignore paragraph direction: the speed scale's
 * ticks and the labels anchored on either side of its arrow. */
static bool MenuPhysicalCell(ActRaiserLocalizationMenu menu, unsigned line,
                             unsigned field_count) {
  return (menu == kActRaiserLocalizationMenu_MessageSpeed || menu == kActRaiserLocalizationMenu_MessageSpeedJP) &&
      ((line == 0 && field_count == (menu == kActRaiserLocalizationMenu_MessageSpeedJP ? 8u : 10u)) ||
       (line == 2 && field_count == 3));
}

static unsigned MenuSharedColumns(ActRaiserLocalizationMenu menu) {
  return menu == kActRaiserLocalizationMenu_StatusCities ? 5
      : menu == kActRaiserLocalizationMenu_StatusScore ? 3 : 0;
}

/* Column headings and data rows share one fitted set of column widths; the
 * title and totals above them do not. */
static bool MenuSharedRow(ActRaiserLocalizationMenu menu, unsigned line,
                          unsigned field_count) {
  const unsigned columns = MenuSharedColumns(menu);
  return columns && field_count == columns && (line == 3 || line >= 7);
}

/* The native divider under the report headings. The adapter preserves this
 * row's artwork, gaps and palette; it is neither localizable text nor a font
 * decoration. */
static bool MenuReservedRow(ActRaiserLocalizationMenu menu, unsigned line) {
  return ArLanguageRowShape_IsReserved((ArLanguageRowShape)menu, line);
}

static bool BuildRow(ActRaiserLocalizationMenu menu, ArTextCellRegion region,
                     unsigned line, unsigned field_count,
                     ArLocalizationTextRowRule *rule) {
  memset(rule, 0, sizeof(*rule));
  rule->first_line = (uint8_t)line;
  rule->last_line = (uint8_t)line;
  rule->field_count = (uint8_t)field_count;
  rule->cell_count = (uint8_t)field_count;
  rule->shared_columns = MenuSharedRow(menu, line, field_count) ? 1u : 0u;
  const bool physical = MenuPhysicalCell(menu, line, field_count);
  for (unsigned index = 0; index < field_count; ++index) {
    unsigned column = 0, next_column = 0;
    if (!MenuColumns(menu, line, index, field_count, &column, &next_column))
      return false;
    /* A right-aligned value must retain the native blank before its
     * neighbour. */
    if (MenuValueCell(menu, line, index, field_count) &&
        index + 1 < field_count)
      --next_column;
    if (next_column > region.columns) next_column = region.columns;
    if (next_column <= column) return false;
    ArLocalizationTextCellRule *cell = &rule->cells[index];
    cell->italic = MenuValueCell(menu, line, index, field_count) ||
        (physical && line == 0);
    cell->start = (uint8_t)column;
    cell->end = (uint8_t)next_column;
    if (physical) {
      cell->alignment = (uint8_t)(line == 0
          ? kArTextHorizontalAlignment_Center
          : (index == 2 ? kArTextHorizontalAlignment_Trailing
                        : kArTextHorizontalAlignment_Leading));
      /* Leave one native pixel of breathing room on each side of the arrow,
       * even when a long translated label uses all of its width. */
      cell->gutter_trailing = line == 2 && index == 0 ? 1u : 0u;
      cell->gutter_leading = line == 2 && index == 2 ? 1u : 0u;
    } else {
      cell->alignment = (uint8_t)(MenuValueCell(menu, line, index, field_count)
          ? kArTextHorizontalAlignment_Trailing
          : kArTextHorizontalAlignment_Leading);
      cell->follows_direction = 1u;
    }
  }
  return true;
}

static bool SameShape(const ArLocalizationTextRowRule *a,
                      const ArLocalizationTextRowRule *b) {
  return a->field_count == b->field_count &&
      a->cell_count == b->cell_count &&
      a->shared_columns == b->shared_columns &&
      !memcmp(a->cells, b->cells, sizeof(a->cells));
}

bool ActRaiserLocalizationGrid_Build(ActRaiserLocalizationMenu menu,
                                     ArTextCellRegion region,
                                     ArLocalizationTextGrid *grid) {
  if (!grid || menu == kActRaiserLocalizationMenu_None || !region.rows ||
      !region.columns)
    return false;
  memset(grid, 0, sizeof(*grid));
  const bool tight_rows = menu == kActRaiserLocalizationMenu_FixedRows &&
      region.rows <= 2;
  grid->row_height = tight_rows ? 1u : 2u;
  grid->crop_rows = tight_rows ? 1u : 0u;
  grid->shared_column_count = (uint8_t)MenuSharedColumns(menu);
  grid->shared_template_line = 3;

  /* Reserved rows are matched before any shape, so a divider stays native
   * however the translation happens to split it. */
  for (unsigned line = 0; line < region.rows; ++line) {
    if (!MenuReservedRow(menu, line)) continue;
    if (grid->rule_count &&
        grid->rules[grid->rule_count - 1u].native_reserved &&
        grid->rules[grid->rule_count - 1u].last_line + 1u == line) {
      grid->rules[grid->rule_count - 1u].last_line = (uint8_t)line;
      continue;
    }
    if (grid->rule_count >= kArLocalizationGridMaximumRules) return false;
    ArLocalizationTextRowRule *rule = &grid->rules[grid->rule_count++];
    memset(rule, 0, sizeof(*rule));
    rule->first_line = rule->last_line = (uint8_t)line;
    rule->field_count = kArLocalizationGridAnyFieldCount;
    rule->native_reserved = 1u;
  }

  for (unsigned field_count = 1;
       field_count <= kArLocalizationGridMaximumCells; ++field_count) {
    ArLocalizationTextRowRule *open = NULL;
    for (unsigned line = 0; line < region.rows; ++line) {
      ArLocalizationTextRowRule candidate;
      if (!BuildRow(menu, region, line, field_count, &candidate)) {
        open = NULL;
        continue;
      }
      if (open && open->last_line + 1u == line && SameShape(open, &candidate)) {
        open->last_line = (uint8_t)line;
        continue;
      }
      if (grid->rule_count >= kArLocalizationGridMaximumRules) return false;
      grid->rules[grid->rule_count] = candidate;
      open = &grid->rules[grid->rule_count++];
    }
  }
  return grid->rule_count != 0;
}
