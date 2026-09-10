/* The renderer no longer knows ActRaiser's menu geometry: the game derives an
 * ArLocalizationTextGrid and publishes it with the frame. These cases pin the
 * derived geometry to the native values the renderer used to hold, so moving
 * it across the boundary cannot quietly change a report's columns. */
#include "actraiser/actraiser_localization_grid.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

static ArLocalizationTextGrid Build(ActRaiserLocalizationMenu menu,
                                    uint16_t columns, uint16_t rows) {
  ArLocalizationTextGrid grid;
  memset(&grid, 0, sizeof(grid));
  const ArTextCellRegion region = {0, 0, columns, rows};
  CHECK(ActRaiserLocalizationGrid_Build(menu, region, &grid));
  return grid;
}

/* Asserts one cell of the row rule that a line of `field_count` cells uses. */
static void ExpectCell(const ArLocalizationTextGrid *grid, unsigned line,
                       unsigned field_count, unsigned index, unsigned start,
                       unsigned end, ArTextHorizontalAlignment alignment) {
  const ArLocalizationTextRowRule *rule =
      ArLocalizationGrid_FindRow(grid, line, field_count);
  if (!rule || rule->native_reserved || index >= rule->cell_count) {
    fprintf(stderr, "%s:%d: no cell for line %u field %u/%u\n", __FILE__,
            __LINE__, line, index, field_count);
    ++failures;
    return;
  }
  const ArLocalizationTextCellRule *cell = &rule->cells[index];
  if (cell->start != start || cell->end != end ||
      cell->alignment != alignment) {
    fprintf(stderr,
            "%s:%d: line %u field %u/%u is [%u,%u) align %d, expected "
            "[%u,%u) align %d\n",
            __FILE__, __LINE__, line, index, field_count, cell->start,
            cell->end, (int)cell->alignment, start, end, (int)alignment);
    ++failures;
  }
}

int main(void) {
  const ArLocalizationTextGrid sound =
      Build(kActRaiserLocalizationMenu_SoundTest, 10, 6);
  CHECK(sound.row_height == 2);
  ExpectCell(&sound, 0, 1, 0, 0, 10, kArTextHorizontalAlignment_Leading);
  for (unsigned line = 2; line <= 4; line += 2) {
    ExpectCell(&sound, line, 2, 0, 1, 6, kArTextHorizontalAlignment_Leading);
    ExpectCell(&sound, line, 2, 1, 7, 9, kArTextHorizontalAlignment_Trailing);
    ExpectCell(&sound, line, 1, 0, 0, 10, kArTextHorizontalAlignment_Leading);
  }
  CHECK(!ArLocalizationGrid_FindRow(&sound, 0, 2));
  CHECK(!ArLocalizationGrid_FindRow(&sound, 5, 1));
  const ArLocalizationTextGrid cities =
      Build(kActRaiserLocalizationMenu_StatusCities, 26, 20);
  const ArLocalizationTextGrid score =
      Build(kActRaiserLocalizationMenu_StatusScore, 26, 20);
  const ArLocalizationTextGrid master =
      Build(kActRaiserLocalizationMenu_StatusMaster, 12, 17);
  const ArLocalizationTextGrid speed =
      Build(kActRaiserLocalizationMenu_MessageSpeed, 10, 4);
  const ArLocalizationTextGrid rows =
      Build(kActRaiserLocalizationMenu_FixedRows, 10, 2);
  CHECK(ArLocalizationGrid_FindRow(&speed, 0, 10)->cells[0].italic);
  CHECK(!ArLocalizationGrid_FindRow(&speed, 2, 3)->cells[0].italic);
  CHECK(ArLocalizationGrid_FindRow(&master, 3, 4)->cells[1].italic);
  CHECK(!ArLocalizationGrid_FindRow(&master, 3, 4)->cells[0].italic);
  CHECK(ArLocalizationGrid_FindRow(&cities, 7, 5)->cells[1].italic);
  CHECK(!ArLocalizationGrid_FindRow(&cities, 3, 5)->cells[1].italic);

  /* Authoring accepts precisely the shapes represented by the game-owned
   * grid, including reserved artwork rows and the last logical row. Pixel
   * anchors remain tested independently below. */
  const ArLocalizationTextGrid *grids[] = {&cities, &score, &master, &rows, &speed};
  const ArLanguageRowShape shapes[] = {kArLanguageRowShape_Cities,
      kArLanguageRowShape_Score, kArLanguageRowShape_Master,
      kArLanguageRowShape_FixedRows, kArLanguageRowShape_MessageSpeed};
  const unsigned heights[] = {20, 20, 17, 2, 4};
  for (unsigned index = 0; index < 5; ++index) {
    for (unsigned line = 0; line < heights[index]; ++line) {
      for (unsigned fields = 1; fields <= 11; ++fields) {
        CHECK(ArLanguageRowShape_Allows(shapes[index], line, fields) ==
              (ArLocalizationGrid_FindRow(grids[index], line, fields) != NULL));
      }
    }
  }
  CHECK(ArLanguageRowShape_ForRoute("status.report.cities_report") == kArLanguageRowShape_Cities);
  CHECK(ArLanguageRowShape_ForRoute("sim.menu.use_offering") == kArLanguageRowShape_FixedRows);
  CHECK(ArLanguageRowShape_ForRoute("sky.menu.magic.fire") == kArLanguageRowShape_FixedRows);
  CHECK(ArLanguageRowShape_ForRoute("unknown") == kArLanguageRowShape_None);
  CHECK(!ArLanguageRowShape_Allows(kArLanguageRowShape_Cities, 20, 1));

  /* Native column anchors, including the blank a right-aligned value keeps
   * before its neighbour. */
  ExpectCell(&master, 3, 4, 1, 3, 6, kArTextHorizontalAlignment_Trailing);
  ExpectCell(&cities, 7, 5, 1, 10, 13, kArTextHorizontalAlignment_Trailing);
  ExpectCell(&cities, 3, 5, 4, 23, 26, kArTextHorizontalAlignment_Leading);
  ExpectCell(&cities, 7, 5, 2, 14, 20, kArTextHorizontalAlignment_Leading);
  ExpectCell(&master, 12, 1, 0, 0, 5, kArTextHorizontalAlignment_Leading);
  ExpectCell(&master, 1, 1, 0, 9, 12, kArTextHorizontalAlignment_Trailing);
  ExpectCell(&score, 7, 3, 1, 15, 20, kArTextHorizontalAlignment_Trailing);

  /* Row shapes the native menus cannot draw stay unrepresented, so the
   * renderer rejects them exactly as the old geometry did. */
  CHECK(!ArLocalizationGrid_FindRow(&speed, 0, 8));
  CHECK(!ArLocalizationGrid_FindRow(&cities, 7, 4));
  CHECK(!ArLocalizationGrid_FindRow(&master, 8, 2));

  /* The scale ticks and the labels beside the arrow are placed physically:
   * a right-to-left translation must not mirror them. */
  for (unsigned digit = 0; digit < 10; ++digit) {
    ExpectCell(&speed, 0, 10, digit, digit, digit + 1,
               kArTextHorizontalAlignment_Center);
    const ArLocalizationTextRowRule *rule =
        ArLocalizationGrid_FindRow(&speed, 0, 10);
    CHECK(rule && !rule->cells[digit].follows_direction);
  }
  ExpectCell(&speed, 2, 3, 0, 0, 4, kArTextHorizontalAlignment_Leading);
  ExpectCell(&speed, 2, 3, 2, 6, 10, kArTextHorizontalAlignment_Trailing);
  const ArLocalizationTextRowRule *speed_labels =
      ArLocalizationGrid_FindRow(&speed, 2, 3);
  CHECK(speed_labels && !speed_labels->cells[0].follows_direction);
  /* The outer labels bracket the arrow and keep a pixel clear beside it. */
  CHECK(speed_labels && speed_labels->cells[0].gutter_trailing &&
        speed_labels->cells[2].gutter_leading &&
        !speed_labels->cells[1].gutter_leading &&
        !speed_labels->cells[1].gutter_trailing);

  /* Ordinary cells follow the paragraph, so a right-to-left translation
   * anchors them on the other edge. */
  const ArLocalizationTextRowRule *city_row =
      ArLocalizationGrid_FindRow(&cities, 7, 5);
  CHECK(city_row && city_row->cells[0].follows_direction);

  /* Column headings and data rows share one fitted set of widths; the title
   * and totals above them do not. */
  CHECK(cities.shared_column_count == 5 && score.shared_column_count == 3);
  CHECK(cities.shared_template_line == 3);
  CHECK(ArLocalizationGrid_FindRow(&cities, 3, 5)->shared_columns);
  CHECK(ArLocalizationGrid_FindRow(&cities, 9, 5)->shared_columns);
  CHECK(!ArLocalizationGrid_FindRow(&cities, 1, 5)->shared_columns);
  CHECK(!ArLocalizationGrid_FindRow(&cities, 4, 5)->shared_columns);
  CHECK(!ArLocalizationGrid_FindRow(&cities, 9, 3)->shared_columns);
  CHECK(!master.shared_column_count && !speed.shared_column_count);

  /* The divider under the headings stays native whatever shape the
   * translation happens to split it into. */
  for (unsigned field_count = 1; field_count <= 5; ++field_count) {
    const ArLocalizationTextRowRule *divider =
        ArLocalizationGrid_FindRow(&cities, 5, field_count);
    CHECK(divider && divider->native_reserved && !divider->cell_count);
    divider = ArLocalizationGrid_FindRow(&score, 5, field_count);
    CHECK(divider && divider->native_reserved);
  }
  CHECK(!ArLocalizationGrid_FindRow(&master, 5, 1)->native_reserved);

  /* A two-row menu packs one native row per text row and may crop; a taller
   * claim keeps the native two-row cadence. */
  CHECK(rows.row_height == 1 && rows.crop_rows);
  CHECK(cities.row_height == 2 && !cities.crop_rows);
  const ArLocalizationTextGrid tall =
      Build(kActRaiserLocalizationMenu_FixedRows, 6, 5);
  CHECK(tall.row_height == 2 && !tall.crop_rows);
  /* A cell never runs past the region it was built for. */
  ExpectCell(&tall, 0, 1, 0, 0, 6, kArTextHorizontalAlignment_Leading);

  ArLocalizationTextGrid unused;
  CHECK(!ActRaiserLocalizationGrid_Build(kActRaiserLocalizationMenu_None,
                                         (ArTextCellRegion){0, 0, 10, 2},
                                         &unused));
  CHECK(!ActRaiserLocalizationGrid_Build(kActRaiserLocalizationMenu_FixedRows,
                                         (ArTextCellRegion){0, 0, 0, 2},
                                         &unused));

  if (failures) {
    fprintf(stderr, "%d localization grid test(s) failed\n", failures);
    return 1;
  }
  puts("localization grid tests passed");
  return 0;
}
