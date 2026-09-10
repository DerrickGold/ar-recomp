#include "render/localized_text_layout.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

int main(void) {
  const int preferred[] = {80, 32, 48, 24, 24};
  const int translated[] = {56, 24, 28, 12, 40};
  ArTextTableColumns columns = {0};
  CHECK(ArLocalizedTextLayout_FitColumns(translated, preferred, 5, 208, 8, 2, &columns));
  CHECK(columns.gap == 8 && columns.left[0] == 0 && columns.right[4] == 208);
  CHECK(columns.left[4] <= 168); /* Borrow pixels for a five-cell header. */
  for (unsigned i = 0; i < 5; ++i) {
    CHECK(columns.right[i] - columns.left[i] >= translated[i]);
    if (i) CHECK(columns.left[i] - columns.right[i - 1] == 8);
  }
  const int crowded[] = {70, 30, 42, 14, 28};
  CHECK(ArLocalizedTextLayout_FitColumns(crowded, preferred, 5, 208, 8, 2, &columns));
  CHECK(columns.gap == 6); /* Tighten spacing before changing font size. */
  const ArTextTableColumns before = columns;
  CHECK(!ArLocalizedTextLayout_FitColumns(crowded, preferred, 5, 190, 8, 2, &columns));
  CHECK(!memcmp(&before, &columns, sizeof(columns)));
  CHECK(!ArLocalizedTextLayout_FitColumns(NULL, preferred, 5, 208, 8, 2, &columns));
  const int ten_minimum[] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
  const int ten_preferred[] = {12, 12, 12, 12, 12, 12, 12, 12, 12, 12};
  CHECK(ArLocalizedTextLayout_FitColumns(
      ten_minimum, ten_preferred, 10, 160, 4, 2, &columns));
  CHECK(columns.left[0] == 0 && columns.right[9] == 160);
  CHECK(!ArLocalizedTextLayout_FitColumns(
      ten_minimum, ten_preferred, 11, 208, 8, 2, &columns));
  CHECK(!ArLocalizedTextLayout_FitColumns(crowded, preferred, 5, 208, 1, 2, &columns));
  for (int width = 200; width <= 1600; width += 7) {
    CHECK(ArLocalizedTextLayout_FitColumns(crowded, preferred, 5, width, 8, 2, &columns));
    CHECK(columns.left[0] == 0 && columns.right[4] == width);
    for (unsigned i = 0; i < 5; ++i) {
      CHECK(columns.right[i] - columns.left[i] >= crowded[i]);
      if (i) CHECK(columns.left[i] - columns.right[i - 1] >= 2);
    }
  }
  for (int scale = 1; scale <= 6; ++scale) {
    const ArRenderRectI left = {10 * scale, 20, 23 * scale, 10};
    const ArRenderRectI right = {75 * scale, 20, 35 * scale, 10};
    ArRenderRectI arrow = {0, 20, 16 * scale, 8 * scale};
    CHECK(ArLocalizedTextLayout_CenterBetween(left, right, &arrow));
    CHECK(arrow.x - (left.x + left.w) == right.x - (arrow.x + arrow.w));
    CHECK(arrow.y == 20 && arrow.w == 16 * scale && arrow.h == 8 * scale);
  }
  ArRenderRectI too_wide = {99, 20, 17, 8};
  CHECK(!ArLocalizedTextLayout_CenterBetween(
      (ArRenderRectI){0, 0, 20, 8}, (ArRenderRectI){36, 0, 20, 8}, &too_wide));
  CHECK(too_wide.x == 99); /* Failed placement is atomic. */
  for (int scale = 1; scale <= 6; ++scale) {
    /* Transparent sprite padding must not influence its visible center. */
    ArRenderRectI arrow = {91, 0, 16 * scale, 8 * scale};
    const ArRenderRectI ink = {0, 1, 16, 3};
    const ArRenderRectI label = {10, 20 * scale, 35, 7 * scale};
    CHECK(ArLocalizedTextLayout_CenterInkVertically(label, ink, 8, &arrow));
    const int difference = (2 * arrow.y + 5 * scale) -
        (2 * label.y + label.h);
    CHECK(difference >= -1 && difference <= 1);
    CHECK(arrow.x == 91 && arrow.w == 16 * scale && arrow.h == 8 * scale);
  }
  ArRenderRectI invalid = {1, 99, 16, 8};
  CHECK(!ArLocalizedTextLayout_CenterInkVertically(
      (ArRenderRectI){0}, (ArRenderRectI){0, 1, 8, 4}, 8, &invalid));
  CHECK(!ArLocalizedTextLayout_CenterInkVertically(
      (ArRenderRectI){0, 0, 10, 10}, (ArRenderRectI){0, 7, 8, 4}, 8, &invalid));
  CHECK(invalid.y == 99);
  ArTextSurface surface = {.ascent = 18, .descent = -5, .line_advance = 25};
  ArTextRevealCluster narrow = {1, 1, 3, 25, 5, 22};
  ArTextRevealCluster wide = {2, 1, 8, 25, 21, 24};
  ArTextRevealCluster accented = {5, 1, 29, 25, 16, 25};
  ArRenderRectI a, b, c;
  const ArRenderRectI origin = {100, 200, 200, 100};
  CHECK(ArLocalizedTextLayout_NameUnderline(&surface, &narrow, origin, &a));
  CHECK(ArLocalizedTextLayout_NameUnderline(&surface, &wide, origin, &b));
  CHECK(ArLocalizedTextLayout_NameUnderline(&surface, &accented, origin, &c));
  CHECK(a.y == b.y && b.y == c.y);
  CHECK(a.h == b.h && b.h == c.h);
  CHECK(a.x >= origin.x + narrow.x && a.x + a.w <= origin.x + wide.x);
  CHECK(b.w > a.w && b.x + b.w <= origin.x + accented.x);
  CHECK(a.y >= origin.y + surface.line_advance + surface.ascent - surface.descent);

  const ArTextRevealCluster lines[] = {
      {1, 0, 0, 0, 10, 20}, {2, 0, 10, 0, 10, 20},
      {4, 1, 0, 25, 10, 20}, {7, 2, 0, 50, 10, 20},
      {8, 2, 10, 50, 10, 20}, {10, 3, 0, 75, 10, 20}};
  surface.reveal_clusters = lines;
  surface.reveal_cluster_count = sizeof(lines) / sizeof(lines[0]);
  uint32_t revealed = 99;
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 0, 50, &revealed) == 0);
  CHECK(revealed == 0);
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 4, 50, &revealed) == 0);
  CHECK(revealed == 3);
  /* Hidden next-page text and an incomplete UTF-8 grapheme cannot push old
   * lines away. A complete newly revealed line scrolls exactly one row. */
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 6, 50, &revealed) == 0);
  CHECK(revealed == 3);
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 7, 50, &revealed) == 25);
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 8, 50, &revealed) == 25);
  CHECK(ArLocalizedTextLayout_ScrollOffset(&surface, 10, 50, &revealed) == 50);

  ArRenderRectI source = {0, 0, 200, 200};
  ArRenderRectI destination = {100, 150, 200, 200};
  CHECK(ArLocalizedTextLayout_Clip(origin, &source, &destination));
  CHECK(source.y == 50 && source.h == 100);
  CHECK(destination.y == 200 && destination.h == 100);
  source = (ArRenderRectI){0, 0, 10, 20};
  destination = (ArRenderRectI){100, 175, 10, 20};
  CHECK(!ArLocalizedTextLayout_Clip(origin, &source, &destination));
  source = (ArRenderRectI){0, 0, 10, 20};
  destination = (ArRenderRectI){100, 190, 10, 20};
  CHECK(ArLocalizedTextLayout_Clip(origin, &source, &destination));
  CHECK(source.y == 10 && source.h == 10 && destination.h == 10);
  /* Uniform key pitch. Three keys a row, a two-blank gutter between them, and
   * a shaper whose advances differ per glyph the way a proportional font's do.
   * The last key of the second row carries no ink, standing in for the finish
   * and backspace keys the game draws as native artwork. */
  {
    enum { kKeyColumns = 3, kPerRow = 7, kClusters = 14 };
    static const char text[] = "A  B  C\nD  E  F";
    static ArTextRevealCluster keys[kClusters] = {
      {1, 0, 0, 0, 30, 20},   {2, 0, 30, 0, 10, 20},  {3, 0, 40, 0, 10, 20},
      {4, 0, 50, 0, 50, 20},  {5, 0, 100, 0, 10, 20}, {6, 0, 110, 0, 10, 20},
      {7, 0, 120, 0, 20, 20},
      {9, 1, 0, 20, 20, 20},  {10, 1, 20, 20, 10, 20}, {11, 1, 30, 20, 10, 20},
      {12, 1, 40, 20, 60, 20},{13, 1, 100, 20, 10, 20},{14, 1, 110, 20, 10, 20},
      {15, 1, 120, 20, 40, 20},
    };
    ArRenderRectI ink[kClusters];
    for (size_t i = 0; i < kClusters; ++i)
      ink[i] = (ArRenderRectI){keys[i].x, keys[i].y, keys[i].width, keys[i].height};
    const size_t gutters[] = {1, 2, 4, 5, 8, 9, 11, 12};
    for (size_t i = 0; i < sizeof(gutters) / sizeof(gutters[0]); ++i)
      ink[gutters[i]] = (ArRenderRectI){0, 0, 0, 0};
    ink[13] = (ArRenderRectI){0, 0, 0, 0}; /* artwork key: placed, but no ink */
    ArTextSurface keyboard = {
      .width = 160, .height = 40, .line_advance = 20,
      .reveal_clusters = keys, .cluster_ink_bounds = ink,
      .reveal_cluster_count = kClusters,
    };
    int shifts[kClusters];
    CHECK(ArLocalizedTextLayout_KeyCellShifts(
        &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns, 2,
        /*first_key_center=*/50, /*key_pitch=*/100, shifts, kClusters));
    /* Every key lands on its column, whatever advance the shaper measured. */
    CHECK(shifts[0] == 50 - 15 && shifts[3] == 150 - 75 && shifts[6] == 250 - 130);
    CHECK(shifts[7] == 50 - 10 && shifts[10] == 150 - 70 && shifts[13] == 250 - 140);
    /* Each gutter splits down the middle, so a cut never lands on a glyph. */
    CHECK(shifts[1] == shifts[0] && shifts[2] == shifts[3]);
    CHECK(shifts[4] == shifts[3] && shifts[5] == shifts[6]);
    CHECK(shifts[8] == shifts[7] && shifts[9] == shifts[10]);
    /* A row that does not hold the stated number of keys leaves text flowed. */
    CHECK(!ArLocalizedTextLayout_KeyCellShifts(
        &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns + 1u, 2,
        50, 100, shifts, kClusters));
    CHECK(!ArLocalizedTextLayout_KeyCellShifts(
        &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns, 2,
        50, 0, shifts, kClusters));
    CHECK(!ArLocalizedTextLayout_KeyCellShifts(
        &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns, 2,
        50, 100, shifts, kClusters - 1u));
    /* Columns stay exactly on pitch at every size the window can produce.
     * Each key's shift is solved from its column, not accumulated along the
     * row, so a shrinking window scales the grid without drift piling up in
     * the last column -- which is what pushed a key into the frame before. */
    for (int pitch = 3; pitch <= 400; ++pitch) {
      const int center = pitch * 3 / 4;
      CHECK(ArLocalizedTextLayout_KeyCellShifts(
          &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns, 2,
          center, pitch, shifts, kClusters));
      for (unsigned row = 0; row < 2u; ++row) {
        for (unsigned key = 0; key < kKeyColumns; ++key) {
          const size_t index = row * kPerRow + key * 3u; /* key, gutter, gutter */
          /* Same extent rule the layout uses: ink when the key has any, and
           * the shaped advance box when it is artwork that has none. */
          const ArRenderRectI box = ink[index].w > 0 && ink[index].h > 0
              ? ink[index]
              : (ArRenderRectI){keys[index].x, keys[index].y,
                                keys[index].width, keys[index].height};
          const int placed = (box.x + box.x + box.w) / 2 + shifts[index];
          CHECK(placed == center + (int)key * pitch);
        }
      }
    }

    /* Lines outside the keyed range keep flowing. */
    CHECK(ArLocalizedTextLayout_KeyCellShifts(
        &keyboard, text, sizeof(text) - 1u, " ", 1, kKeyColumns, 1,
        50, 100, shifts, kClusters));
    for (size_t i = 0; i < kPerRow; ++i) CHECK(shifts[i] == 0);
    CHECK(shifts[7] != 0);
  }

  return failures ? 1 : 0;
}
