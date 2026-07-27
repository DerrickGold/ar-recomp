#include "actraiser_ws_gap.h"

/* Fill one strip of `count` pixels starting at `x0` on every row. */
static void FillStrip(uint8_t *rows, size_t pitch, int height,
                      int x0, int count, uint32_t fill_argb) {
  if (count <= 0) return;
  for (int y = 0; y < height; y++) {
    uint32_t *row = (uint32_t *)(void *)(rows + (size_t)y * pitch);
    for (int x = 0; x < count; x++)
      row[x0 + x] = fill_argb;
  }
}

void ActRaiserFillMarginGaps(uint8_t *rows, size_t pitch, int height,
                             int budget, int live_left, int live_right,
                             uint32_t fill_argb) {
  if (!rows || height <= 0 || budget <= 0) return;
  /* A margin wider than the budget cannot happen (PpuSetExtraSideSpace clamps
   * to it), so treat it as a caller bug and do nothing rather than compute a
   * negative width or index past the row. */
  if (live_left < 0 || live_right < 0 ||
      live_left > budget || live_right > budget)
    return;

  /* Left gap occupies columns [0, budget - live_left): the live window starts
   * at column budget - live_left. */
  FillStrip(rows, pitch, height, 0, budget - live_left, fill_argb);

  /* Right gap starts immediately after the live window's last column. The
   * authentic 256 columns sit at [budget, budget + 256). */
  const int authentic_width = 256;
  FillStrip(rows, pitch, height, budget + authentic_width + live_right,
            budget - live_right, fill_argb);
}
