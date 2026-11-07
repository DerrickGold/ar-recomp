#include "present/presentation_upload_mirror.h"
#include "app/performance_metrics.h"

#include "support/test_assert.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void TestIdenticalRegionIsClean(void) {
  uint32_t current[12];
  uint32_t previous[12];
  for (unsigned i = 0; i < 12; i++)
    current[i] = previous[i] = i;
  ArRenderRectI dirty = {-1, -1, -1, -1};
  assert(!PresentationUploadMirror_FindDirtyRect(
      (const uint8_t *)current, 4 * (int)sizeof(uint32_t), (const uint8_t *)previous,
      4 * (int)sizeof(uint32_t), 4, 3, &dirty));
  assert(dirty.x == 0 && dirty.y == 0 && dirty.w == 0 && dirty.h == 0);
}

static void TestDirtyBoundsSpanEveryChangedPixel(void) {
  uint32_t current[20] = {0};
  uint32_t previous[20] = {0};
  /* Five physical pixels per row, but only the first four are in-region. */
  current[1] = 0x11223344u;
  current[3 * 5 + 3] = 0x55667788u;
  current[4] = 0xffffffffu; /* Pitch padding must not widen the result. */
  ArRenderRectI dirty = {0};
  assert(PresentationUploadMirror_FindDirtyRect((const uint8_t *)current, 5 * (int)sizeof(uint32_t),
                                                (const uint8_t *)previous,
                                                5 * (int)sizeof(uint32_t), 4, 4, &dirty));
  assert(dirty.x == 1 && dirty.y == 0 && dirty.w == 3 && dirty.h == 4);
}

static void TestSingleChangedByteStillUploadsWholePixel(void) {
  uint8_t current[12] = {0};
  uint8_t previous[12] = {0};
  current[6] = 1;
  ArRenderRectI dirty = {0};
  assert(PresentationUploadMirror_FindDirtyRect(current, 12, previous, 12, 3, 1, &dirty));
  assert(dirty.x == 1 && dirty.y == 0 && dirty.w == 1 && dirty.h == 1);
}

static uint32_t Random(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static void TestAgainstByteOracle(void) {
  uint8_t current[32768], previous[32768];
  uint32_t state = 0x273951u;
  for (int pass = 0; pass < 3000; pass++) {
    const int width = 1 + (int)(Random(&state) % 97);
    const int height = 1 + (int)(Random(&state) % 73);
    const int cp = width * 4 + (int)(Random(&state) % 7);
    const int pp = width * 4 + (int)(Random(&state) % 7);
    const int ca = (int)(Random(&state) % 4), pa = (int)(Random(&state) % 4);
    memset(current, 0xa5, sizeof(current));
    memset(previous, 0x5a, sizeof(previous));
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width * 4; x++) {
        const uint8_t value = (uint8_t)Random(&state);
        current[ca + y * cp + x] = previous[pa + y * pp + x] = value;
        if (pass % 5 && Random(&state) % (pass % 3 ? 173 : 3) == 0) current[ca + y * cp + x] ^= 1;
      }
    }
    int x0 = width, x1 = 0, y0 = height, y1 = 0;
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width * 4; x++) {
        if (current[ca + y * cp + x] == previous[pa + y * pp + x]) continue;
        if (x / 4 < x0) x0 = x / 4;
        if (x / 4 + 1 > x1) x1 = x / 4 + 1;
        if (y < y0) y0 = y;
        y1 = y + 1;
      }
    ArRenderRectI dirty;
    const bool changed = PresentationUploadMirror_FindDirtyRect(current + ca, cp, previous + pa, pp,
                                                                width, height, &dirty);
    assert(changed == (x0 < width));
    const ArRenderRectI expected =
        changed ? (ArRenderRectI){x0, y0, x1 - x0, y1 - y0} : (ArRenderRectI){0};
    assert(dirty.x == expected.x && dirty.y == expected.y && dirty.w == expected.w &&
           dirty.h == expected.h);
  }
}

static void TestWideBoundsSkipInteriorAndReportComparisons(void) {
  enum { kRow = 256, kRows = 16 };
  uint8_t current[kRow * kRows] = {0}, previous[kRow * kRows] = {0};
  for (unsigned changed = 0; changed < 2; ++changed) {
    if (changed) {
      current[0] = current[kRow - 1] = 1;
      current[(kRows - 1) * kRow + 17] = 1;
    }
    PerformanceMetrics_Configure(true, false);
    ArRenderRectI dirty;
    assert(PresentationUploadMirror_FindDirtyRect(current, kRow, previous, kRow, kRow / 4, kRows,
                                                  &dirty) == (changed != 0));
    PerformanceMetrics_PresentCompleted(1);
    PerformanceMetrics_PresentCompleted(UINT64_C(1000000001));
    PerformanceSnapshot sample;
    PerformanceMetrics_Snapshot(&sample);
    /* Two full-row comparisons plus two failing 32-byte edge blocks and
     * their byte refinements. Clean images compare every row once. Counts
     * describe requested operands, not libc's actual early-exit reads. */
    const double bytes = changed ? 2 * kRow * 2 + 2 * (32 * 2 + 2) : kRow * kRows * 2;
    assert(sample.ready && sample.counts[kPerformanceCount_ScanBytes] == bytes / 2);
    if (changed) assert(dirty.x == 0 && dirty.y == 0 && dirty.w == kRow / 4 && dirty.h == kRows);
    PerformanceMetrics_Configure(false, false);
  }
}

/* Exercise real texture updates, not just the dirty-rectangle helper. Source
 * bytes are unaligned with non-pixel pitch padding; untouched texture bytes
 * must survive every exact or conservatively widened upload. */
enum { kWidth = 16, kHeight = 8, kPitch = kWidth * 4 + 7,
       kTextureWidth = 24, kTextureHeight = 16 };
typedef struct UploadBackend {
  uint8_t pixels[2][kTextureWidth * kTextureHeight * 4];
  unsigned calls;
  bool fail;
} UploadBackend;

static bool Update(void *context, ArRenderTexture texture,
    const ArRenderRectI *rect, const void *pixels, int pitch) {
  UploadBackend *backend = context;
  assert(texture.value >= 1 && texture.value <= 2 && rect);
  assert(rect->x >= 0 && rect->y >= 0 && rect->w > 0 && rect->h > 0);
  assert(rect->x + rect->w <= kTextureWidth && rect->y + rect->h <= kTextureHeight);
  assert(pitch >= rect->w * 4);
  ++backend->calls;
  if (backend->fail) return false;
  for (int y = 0; y < rect->h; ++y)
    memcpy(backend->pixels[texture.value - 1] +
        ((rect->y + y) * kTextureWidth + rect->x) * 4,
        (const uint8_t *)pixels + (size_t)y * pitch, (size_t)rect->w * 4);
  return true;
}
static const ArRenderBackendOps kUploadOps = {
  .struct_size = sizeof(ArRenderBackendOps), .update_texture = Update,
};

static void FillSource(uint8_t *source) {
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kPitch; ++x)
      source[y * kPitch + x] = (uint8_t)(x * 13 + y * 27);
}

static PresentationUploadResult UploadChecked(UploadBackend *backend,
    PresentationUploadMirror *mirror, unsigned texture, const uint8_t *source,
    int width, int height, int pitch, int x, int y,
    const PresentationUploadChanges *changes, uint64_t *scanned) {
  uint8_t expected[sizeof(backend->pixels)];
  memcpy(expected, backend->pixels, sizeof(expected));
  if (!backend->fail)
    for (int row = 0; row < height; ++row)
      memcpy(expected + (texture - 1) * sizeof(backend->pixels[0]) +
          ((y + row) * kTextureWidth + x) * 4,
          source + (size_t)row * pitch, (size_t)width * 4);
  ArRenderDevice device = {.ops = &kUploadOps, .context = backend};
  const unsigned calls = backend->calls;
  PerformanceMetrics_Configure(true, false);
  PresentationUploadResult result;
  assert(PresentationUploadMirror_UploadArgb8888WithChanges(mirror, &device,
      (ArRenderTexture){texture}, source, width, height, pitch, x, y, changes,
      &result) == !backend->fail);
  PerformanceMetrics_PresentCompleted(1);
  PerformanceMetrics_PresentCompleted(UINT64_C(1000000001));
  PerformanceSnapshot sample;
  PerformanceMetrics_Snapshot(&sample);
  assert(sample.ready);
  *scanned = (uint64_t)(sample.counts[kPerformanceCount_ScanBytes] * sample.presents);
  PerformanceMetrics_Configure(false, false);
  assert(!memcmp(expected, backend->pixels, sizeof(expected)));
  assert(backend->calls == calls + result.changed);
  if (backend->fail) {
    assert(!mirror->valid && !result.uploaded_bytes);
  } else {
    assert(mirror->valid && mirror->source_revision == (changes ? changes->revision : 0));
    assert(result.uploaded_bytes == (uint64_t)result.destination.w * result.destination.h * 4);
    for (int row = 0; row < height; ++row)
      assert(!memcmp(mirror->pixels + (size_t)row * width * 4,
          source + (size_t)row * pitch, (size_t)width * 4));
  }
  return result;
}

static void TestExactRevisionAndRowHints(void) {
  UploadBackend backend = {0};
  PresentationUploadMirror mirror = {0};
  uint8_t storage[3 + kPitch * kHeight + 9], rows[kHeight] = {0};
  memset(storage, 0xa5, sizeof(storage));
  uint8_t *source = storage + 3;
  FillSource(source);
  uint64_t scanned;
  PresentationUploadChanges changes = {.revision = 1};
  PresentationUploadResult r = UploadChecked(&backend, &mirror, 1, source,
      kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(r.changed && r.uploaded_bytes == kWidth * kHeight * 4 && !scanned);
  changes.previous_revision = 1;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(!r.changed && !scanned);

  /* Two sparse rows produce the same exact rectangle and bytes as full
   * comparison, while the six proven-clean rows are never scanned. */
  source[2 * kPitch + 3 * 4 + 1] ^= 1;
  source[5 * kPitch + 10 * 4 + 2] ^= 1;
  rows[2] = rows[5] = 1;
  changes = (PresentationUploadChanges){1, 2, rows, kHeight};
  ArRenderRectI full;
  PerformanceMetrics_Configure(true, false);
  assert(PresentationUploadMirror_FindDirtyRect(source, kPitch, mirror.pixels,
      kWidth * 4, kWidth, kHeight, &full));
  PerformanceMetrics_PresentCompleted(1);
  PerformanceMetrics_PresentCompleted(UINT64_C(1000000001));
  PerformanceSnapshot sample; PerformanceMetrics_Snapshot(&sample);
  const uint64_t full_scan = (uint64_t)(sample.counts[kPerformanceCount_ScanBytes] * sample.presents);
  PerformanceMetrics_Configure(false, false);
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(r.destination.x == full.x + 1 && r.destination.y == full.y + 2 &&
      r.destination.w == full.w && r.destination.h == full.h);
  assert(r.destination.x == 4 && r.destination.y == 4 && r.destination.w == 8 && r.destination.h == 4);
  assert(scanned > 0 && full_scan - scanned == (kHeight - 2) * kWidth * 4 * 2);

  /* Exercise the wide-bounds reverse scan with holes in the row mask. */
  memset(rows, 0, sizeof(rows)); rows[0] = rows[7] = 1;
  source[0] ^= 1; source[kWidth * 4 - 1] ^= 1; source[7 * kPitch + 17] ^= 1;
  changes.previous_revision = 2; changes.revision = 3;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(r.destination.x == 1 && r.destination.y == 2 && r.destination.w == kWidth && r.destination.h == kHeight);
  memset(rows, 0, sizeof(rows)); rows[1] = rows[6] = 1;
  changes.previous_revision = 3; changes.revision = 4;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(!r.changed && scanned == 2 * kWidth * 4 * 2);
  memset(rows, 0, sizeof(rows)); changes.previous_revision = 4; changes.revision = 5;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(!r.changed && !scanned);

  /* Revision 6 was never synchronized: its changed row is absent from the
   * 6->7 hint, so only a full comparison can recover both changed rows. */
  source[3 * kPitch + 9] ^= 1; source[4 * kPitch + 13] ^= 1;
  rows[4] = 1; changes.previous_revision = 6; changes.revision = 7;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(r.changed && r.destination.y == 5 && r.destination.h == 2 && scanned >= kWidth * kHeight * 4 * 2);
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, NULL, &scanned);
  assert(!r.changed && scanned == kWidth * kHeight * 4 * 2 && !mirror.source_revision);
  changes.previous_revision = changes.revision = 7;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(!r.changed && scanned == kWidth * kHeight * 4 * 2);

  /* Zero revisions and malformed row counts cannot authorize row skipping. */
  memset(rows, 0, sizeof(rows));
  for (unsigned pass = 0; pass < 5; ++pass) {
    source[2 * kPitch + 5] ^= 1;
    changes.previous_revision = mirror.source_revision;
    changes.revision = pass ? 8 + pass : 0;
    changes.row_count = pass < 2 ? kHeight : pass == 2 ? 0 : pass == 3 ? kHeight - 1 : kHeight + 1;
    r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
    assert(r.changed && scanned >= kWidth * kHeight * 4 * 2);
  }
  changes.previous_revision = changes.revision; ++changes.revision; changes.changed_rows = NULL;
  r = UploadChecked(&backend, &mirror, 1, source, kWidth, kHeight, kPitch, 1, 2, &changes, &scanned);
  assert(!r.changed && scanned == kWidth * kHeight * 4 * 2);
  for (unsigned i = 0; i < 3; ++i) assert(storage[i] == 0xa5);
  for (unsigned i = 3 + kPitch * kHeight; i < sizeof(storage); ++i) assert(storage[i] == 0xa5);
  PresentationUploadMirror_Reset(&mirror);
}

static void TestHintIdentityAndFailureRecovery(void) {
  UploadBackend backend = {0};
  PresentationUploadMirror mirror = {0};
  uint8_t source[kPitch * kHeight], rows[kHeight] = {0};
  FillSource(source);
  PresentationUploadChanges changes = {30, 30, rows, kHeight};
  uint64_t scanned;
  unsigned texture = 1; int width = kWidth, height = kHeight, x = 1, y = 2;
  for (unsigned pass = 0; pass < 6; ++pass) {
    if (pass == 1) texture = 2;
    if (pass == 2) --width;
    if (pass == 3) --height;
    if (pass == 4) ++x;
    if (pass == 5) ++y;
    PresentationUploadResult r = UploadChecked(&backend, &mirror, texture, source,
        width, height, kPitch, x, y, &changes, &scanned);
    assert(r.changed && !scanned && r.destination.x == x && r.destination.y == y &&
        r.destination.w == width && r.destination.h == height);
  }
  /* Pitch and pointer changes alone do not change logical source content. */
  uint8_t packed[kWidth * kHeight * 4];
  for (int row = 0; row < height; ++row)
    memcpy(packed + row * width * 4, source + row * kPitch, (size_t)width * 4);
  PresentationUploadResult r = UploadChecked(&backend, &mirror, texture, packed,
      width, height, width * 4, x, y, &changes, &scanned);
  assert(!r.changed && !scanned);

  source[3 * kPitch + 9] ^= 1; rows[3] = 1;
  changes.previous_revision = 30; changes.revision = 31; changes.row_count = height;
  backend.fail = true;
  r = UploadChecked(&backend, &mirror, texture, source, width, height, kPitch, x, y, &changes, &scanned);
  assert(r.changed && !mirror.valid);
  backend.fail = false;
  r = UploadChecked(&backend, &mirror, texture, source, width, height, kPitch, x, y, &changes, &scanned);
  assert(r.changed && !scanned && r.destination.w == width && r.destination.h == height);
  PresentationUploadMirror_Reset(&mirror);
  assert(!mirror.valid && !mirror.pixels && !mirror.source_revision);
  changes.previous_revision = changes.revision;
  r = UploadChecked(&backend, &mirror, texture, source, width, height, kPitch, x, y, &changes, &scanned);
  assert(r.changed && !scanned && r.destination.w == width && r.destination.h == height);
  PresentationUploadMirror_Reset(&mirror);
}

int main(void) {
  TestIdenticalRegionIsClean();
  TestDirtyBoundsSpanEveryChangedPixel();
  TestSingleChangedByteStillUploadsWholePixel();
  TestAgainstByteOracle();
  TestWideBoundsSkipInteriorAndReportComparisons();
  TestExactRevisionAndRowHints();
  TestHintIdentityAndFailureRecovery();
  puts("presentation_upload_mirror_test: ok");
  return 0;
}
