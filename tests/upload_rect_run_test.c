#include "render/upload_rect_run.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { kExtent = 512, kMaximumRects = 1024 };

typedef struct Uploads {
  ArRenderRectI rects[kMaximumRects];
  int count;
} Uploads;

/* Feeds a stream through a run exactly as the texture-upload loops do. */
static void Coalesce(const ArRenderRectI *input, int count, Uploads *out) {
  UploadRectRun run = {0};
  ArRenderRectI flush;
  out->count = 0;
  for (int i = 0; i < count; ++i)
    if (UploadRectRun_Add(&run, input[i], &flush)) out->rects[out->count++] = flush;
  if (UploadRectRun_Finish(&run, &flush)) out->rects[out->count++] = flush;
  assert(!run.active);
}

static int64_t Area(ArRenderRectI rect) { return (int64_t)rect.w * rect.h; }

static void AssertCoversEveryInputPixel(const ArRenderRectI *input, int count,
                                        const Uploads *uploads) {
  static uint8_t covered[kExtent * kExtent];
  memset(covered, 0, sizeof(covered));
  for (int i = 0; i < uploads->count; ++i) {
    const ArRenderRectI r = uploads->rects[i];
    assert(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           r.x + r.w <= kExtent && r.y + r.h <= kExtent);
    for (int y = r.y; y < r.y + r.h; ++y)
      memset(covered + y * kExtent + r.x, 1, (size_t)r.w);
  }
  for (int i = 0; i < count; ++i)
    for (int y = input[i].y; y < input[i].y + input[i].h; ++y)
      for (int x = input[i].x; x < input[i].x + input[i].w; ++x)
        assert(covered[y * kExtent + x]);
}

/* The Town 3D canvas at present 1300 of the Aitos eruption replay: twelve
 * tile-row bands covering 220,928 of 262,144 pixels were twelve updates. */
static void TestAitosCanvasFrameBecomesOneUpload(void) {
  static const ArRenderRectI frame[] = {
    {0, 0, 512, 96},    {384, 96, 128, 32}, {128, 128, 384, 16},
    {112, 144, 400, 16}, {0, 160, 192, 32},  {0, 192, 512, 128},
    {432, 320, 32, 16}, {16, 336, 464, 16}, {0, 352, 480, 32},
    {0, 384, 464, 16},  {0, 400, 480, 80},  {0, 480, 448, 32},
  };
  const int count = (int)(sizeof(frame) / sizeof(frame[0]));
  Uploads uploads;
  Coalesce(frame, count, &uploads);
  assert(uploads.count == 1);
  assert(uploads.rects[0].x == 0 && uploads.rects[0].y == 0 &&
         uploads.rects[0].w == 512 && uploads.rects[0].h == 512);
  AssertCoversEveryInputPixel(frame, count, &uploads);
}

/* A burst of one-pixel-row spans over a small animated patch, the shape of the
 * voxel ground's per-row tracker, collapses to the patch. */
static void TestRowSpansOfOnePatchMerge(void) {
  ArRenderRectI spans[64];
  for (int row = 0; row < 64; ++row)
    spans[row] = (ArRenderRectI){200 + row % 5, 100 + row, 40 + row % 7, 1};
  Uploads uploads;
  Coalesce(spans, 64, &uploads);
  assert(uploads.count == 1);
  AssertCoversEveryInputPixel(spans, 64, &uploads);
}

/* Two tiles in opposite corners would re-send almost the whole texture if
 * merged; they must stay two small updates. */
static void TestDistantRectsStaySeparate(void) {
  static const ArRenderRectI corners[] = {{0, 0, 8, 8}, {504, 504, 8, 8}};
  Uploads uploads;
  Coalesce(corners, 2, &uploads);
  assert(uploads.count == 2);
  assert(!memcmp(&uploads.rects[0], &corners[0], sizeof(corners[0])));
  assert(!memcmp(&uploads.rects[1], &corners[1], sizeof(corners[1])));
}

static void TestEmptyInput(void) {
  static const ArRenderRectI empty[] = {{10, 10, 0, 4}, {3, 3, 5, -1}};
  Uploads uploads;
  Coalesce(empty, 2, &uploads);
  assert(uploads.count == 0);
  Coalesce(NULL, 0, &uploads);
  assert(uploads.count == 0);
}

static uint32_t Random(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state >> 8;
}

/* Row-banded streams like both trackers emit: every input pixel is uploaded,
 * no update is added, and the re-sent area never exceeds one allowance per
 * update saved. The last is the property the allowance's cost argument needs. */
static void TestRandomStreamsAgainstOracle(void) {
  uint32_t state = 0x5eed1234u;
  ArRenderRectI stream[kExtent];
  for (int pass = 0; pass < 2000; ++pass) {
    const int band = 1 + (int)(Random(&state) % 16);
    int count = 0;
    int64_t input_area = 0;
    for (int y = 0; y + band <= kExtent; y += band) {
      if (Random(&state) % 3) continue;
      const int x = (int)(Random(&state) % kExtent);
      const int w = 1 + (int)(Random(&state) % (unsigned)(kExtent - x));
      stream[count++] = (ArRenderRectI){x, y, w, band};
      input_area += (int64_t)w * band;
    }
    Uploads uploads;
    Coalesce(stream, count, &uploads);
    assert(uploads.count <= count);
    assert(count == 0 || uploads.count >= 1);
    int64_t output_area = 0;
    for (int i = 0; i < uploads.count; ++i) output_area += Area(uploads.rects[i]);
    assert(output_area - input_area <=
           (int64_t)(count - uploads.count) * kUploadRectRunMaximumWastePixels);
    AssertCoversEveryInputPixel(stream, count, &uploads);
  }
}

int main(void) {
  TestAitosCanvasFrameBecomesOneUpload();
  TestRowSpansOfOnePatchMerge();
  TestDistantRectsStaySeparate();
  TestEmptyInput();
  TestRandomStreamsAgainstOracle();
  printf("upload_rect_run_test: all passed\n");
  return 0;
}
