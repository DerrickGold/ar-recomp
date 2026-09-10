#include "render/localized_text_layout.h"

#include <stdio.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

int main(void) {
  unsigned start, end;
  for (unsigned digit = 0; digit < 10; ++digit) {
    CHECK(ArLocalizedTextLayout_TableColumns(
        kArLocalizationTextLayout_MessageSpeed, 0, digit, 10, &start, &end));
    CHECK(start == digit && end == digit + 1);
    CHECK(!ArLocalizedTextLayout_TableNumeric(
        kArLocalizationTextLayout_MessageSpeed, 0, digit, 10));
    for (ArTextDirection direction = kArTextDirection_LeftToRight;
         direction <= kArTextDirection_RightToLeft; ++direction)
      CHECK(ArLocalizedTextLayout_TableAlignment(
          kArLocalizationTextLayout_MessageSpeed, 0, digit, 10, direction) ==
          kArTextHorizontalAlignment_Center);
  }
  CHECK(ArLocalizedTextLayout_TableAlignment(
      kArLocalizationTextLayout_MessageSpeed, 2, 0, 3,
      kArTextDirection_LeftToRight) == kArTextHorizontalAlignment_Leading);
  CHECK(ArLocalizedTextLayout_TableAlignment(
      kArLocalizationTextLayout_MessageSpeed, 2, 2, 3,
      kArTextDirection_LeftToRight) == kArTextHorizontalAlignment_Trailing);
  CHECK(ArLocalizedTextLayout_TableAlignment(
      kArLocalizationTextLayout_StatusMaster, 3, 1, 4,
      kArTextDirection_LeftToRight) == kArTextHorizontalAlignment_Trailing);
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
  CHECK(!ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_MessageSpeed, 0, 0, 8, &start, &end));
  CHECK(ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_StatusMaster, 3, 1, 4, &start, &end));
  CHECK(start == 3 && end == 6); /* Blank before HP at column 7. */
  CHECK(ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_StatusCities, 7, 1, 5, &start, &end));
  CHECK(start == 10 && end == 13); /* Blank before growth at column 14. */
  CHECK(ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_StatusCities, 3, 4, 5, &start, &end));
  CHECK(start == 23 && end == 26); /* Wider translated item heading. */
  CHECK(ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_StatusMaster, 12, 0, 1, &start, &end));
  CHECK(start == 0 && end == 5); /* Magic label cannot enter native icon cells. */
  CHECK(ArLocalizedTextLayout_TableNumeric(
      kArLocalizationTextLayout_StatusMaster, 1, 0, 1));
  CHECK(!ArLocalizedTextLayout_TableNumeric(
      kArLocalizationTextLayout_StatusCities, 7, 2, 5));
  CHECK(!ArLocalizedTextLayout_TableColumns(
      kArLocalizationTextLayout_StatusCities, 7, 5, 5, &start, &end));

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
  return failures ? 1 : 0;
}
