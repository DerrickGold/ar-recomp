#include "upload_rect_run.h"

#include <stdint.h>

static int64_t Area(ArRenderRectI rect) {
  return (int64_t)rect.w * (int64_t)rect.h;
}

static int Minimum(int a, int b) { return a < b ? a : b; }
static int Maximum(int a, int b) { return a > b ? a : b; }

static ArRenderRectI Union(ArRenderRectI a, ArRenderRectI b) {
  const int x0 = Minimum(a.x, b.x), y0 = Minimum(a.y, b.y);
  const int x1 = Maximum(a.x + a.w, b.x + b.w);
  const int y1 = Maximum(a.y + a.h, b.y + b.h);
  return (ArRenderRectI){x0, y0, x1 - x0, y1 - y0};
}

static int64_t IntersectionArea(ArRenderRectI a, ArRenderRectI b) {
  const int w = Minimum(a.x + a.w, b.x + b.w) - Maximum(a.x, b.x);
  const int h = Minimum(a.y + a.h, b.y + b.h) - Maximum(a.y, b.y);
  return w > 0 && h > 0 ? (int64_t)w * (int64_t)h : 0;
}

bool UploadRectRun_Add(UploadRectRun *run, ArRenderRectI next,
                       ArRenderRectI *flush) {
  if (next.w <= 0 || next.h <= 0) return false;
  if (!run->active) {
    run->bounds = next;
    run->active = true;
    return false;
  }
  const ArRenderRectI merged = Union(run->bounds, next);
  /* Pixels the merged update sends beyond the two separate updates. Waste
   * already inside the run was paid for by an earlier merge's saved call. */
  const int64_t waste = Area(merged) - Area(run->bounds) - Area(next) +
      IntersectionArea(run->bounds, next);
  if (waste <= kUploadRectRunMaximumWastePixels) {
    run->bounds = merged;
    return false;
  }
  *flush = run->bounds;
  run->bounds = next;
  return true;
}

bool UploadRectRun_Finish(UploadRectRun *run, ArRenderRectI *flush) {
  if (!run->active) return false;
  *flush = run->bounds;
  run->active = false;
  return true;
}
