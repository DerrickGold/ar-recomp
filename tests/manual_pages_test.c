/* Page indexing and reader kinematics for the in-game manual.
 *
 * The reader's SDL shim is untestable here (window, textures, draw calls), so
 * everything that can be arithmetic or policy lives in manual_pages.c and is
 * asserted here -- the diorama_layer_order / scene3d_math pattern.
 *
 * The load-bearing case is TestLeafNeverGoesBehindASettledPage: SDL_RenderGeometry
 * has no depth test, so the reader's correctness rests on one fixed draw order,
 * and that order is only valid because the turning leaf cannot dip behind the
 * pages it passes over. That is a property of the geometry, checked here across
 * the whole turn domain, rather than a comment nobody can verify.
 */

#include "manual_pages.h"
#include "scene3d_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

/* ── Album fixtures ───────────────────────────────────────────────────────── */

/* A minimal but STRUCTURALLY REAL baseline JPEG: SOI, an APP0, a SOF0 carrying
 * the geometry, a SOS, some entropy bytes, EOI. Real enough that the carver's
 * marker walk is exercised rather than bypassed. */
static size_t AppendJpeg(unsigned char *out, size_t at, int w, int h,
                         size_t filler) {
  const unsigned char head[] = {
    0xFF, 0xD8,                                     /* SOI */
    0xFF, 0xE0, 0x00, 0x04, 'J', 'F',               /* APP0, len 4 */
  };
  memcpy(out + at, head, sizeof head);
  at += sizeof head;
  const unsigned char sof[] = {
    0xFF, 0xC0, 0x00, 0x0B, 0x08,
    (unsigned char)(h >> 8), (unsigned char)(h & 0xFF),
    (unsigned char)(w >> 8), (unsigned char)(w & 0xFF),
    0x01, 0x01, 0x11, 0x00,
  };
  memcpy(out + at, sof, sizeof sof);
  at += sizeof sof;
  const unsigned char sos[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 };
  memcpy(out + at, sos, sizeof sos);
  at += sizeof sos;
  /* Entropy-coded bytes, with 0xFF BYTE-STUFFED as 0xFF00 exactly as a real
   * encoder must -- an unstuffed 0xFF in scan data is not valid JPEG, and a
   * fixture that emits one is testing a stream no decoder would accept. */
  for (size_t i = 0; i < filler; i++) {
    const unsigned char byte = (unsigned char)(i * 7u + 3u);
    out[at++] = byte;
    if (byte == 0xFF) out[at++] = 0x00;
  }
  out[at++] = 0xFF;
  out[at++] = 0xD9;                                 /* EOI */
  return at;
}

static void TestCarvesEveryPageOfAnAlbum(void) {
  unsigned char *buf = (unsigned char *)calloc(1, 1 << 16);
  CHECK(buf != NULL);
  if (!buf) return;
  size_t at = 0;
  for (int i = 0; i < 6; i++) at = AppendJpeg(buf, at, 739, 1080, 400);

  ManualPageIndex index;
  const int found = ManualPages_CarveAlbum(buf, at, &index);
  CHECK(found == 6);
  CHECK(index.count == 6);
  for (int i = 0; i < index.count; i++) {
    CHECK(index.pages[i].width == 739);
    CHECK(index.pages[i].height == 1080);
    CHECK(index.pages[i].length > 0);
    /* Every recorded range must start on a real SOI and end on a real EOI --
     * an off-by-one here hands stb a truncated stream at display time. */
    const unsigned char *p = buf + index.pages[i].offset;
    CHECK(p[0] == 0xFF && p[1] == 0xD8);
    const unsigned char *e = p + index.pages[i].length;
    CHECK(e[-2] == 0xFF && e[-1] == 0xD9);
  }
  /* Ranges must not overlap, or a page would be decoded twice from one stream. */
  for (int i = 1; i < index.count; i++) {
    CHECK(index.pages[i].offset >=
          index.pages[i - 1].offset + index.pages[i - 1].length);
  }
  CHECK(ManualPages_LooksLikeAlbum(&index, at));
  free(buf);
}

/* THE predicate that matters. "I found a JPEG" is a harmful success test: a
 * vector document with a letterhead logo on each page satisfies it and would
 * produce a manual of letterheads. */
static void TestLetterheadDocumentIsNotAnAlbum(void) {
  const size_t total = 1 << 16;
  unsigned char *buf = (unsigned char *)calloc(1, total);
  CHECK(buf != NULL);
  if (!buf) return;
  /* Two small logos adrift in a much larger file of "vector content". */
  size_t at = 1000;
  at = AppendJpeg(buf, at, 1148, 210, 60);
  at = AppendJpeg(buf, at, 1148, 210, 60);

  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, total, &index) == 2);
  /* Found images, so a naive test would have said yes. */
  CHECK(!ManualPages_LooksLikeAlbum(&index, total));
  free(buf);
}

/* A SINGLE page is not an album, and the count rule must be what says so.
 *
 * The letterhead test above is rejected by the 80%-dominance rule, so the
 * `count < 2` clause had no fixture of its own -- both weakening it to `< 1` and
 * deleting it entirely left the suite green. A lone image that DOMINATES its
 * container (a bare .jpg, or a one-page PDF) slips through every other rule. */
static void TestASingleDominantImageIsNotAnAlbum(void) {
  unsigned char *buf = (unsigned char *)calloc(1, 1 << 14);
  CHECK(buf != NULL);
  if (!buf) return;
  /* One page filling essentially the whole container: dominance passes, geometry
   * is trivially uniform, so ONLY the count rule can reject it. */
  const size_t at = AppendJpeg(buf, 0, 739, 1080, 2000);
  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, at, &index) == 1);
  uint64_t bytes = 0;
  for (int i = 0; i < index.count; i++) bytes += index.pages[i].length;
  CHECK(bytes * 100u >= (uint64_t)at * 80u);   /* dominance would pass */
  CHECK(!ManualPages_LooksLikeAlbum(&index, at));   /* but it is not an album */
  free(buf);
}

static void TestMixedGeometryIsNotAnAlbum(void) {
  unsigned char *buf = (unsigned char *)calloc(1, 1 << 16);
  CHECK(buf != NULL);
  if (!buf) return;
  size_t at = 0;
  at = AppendJpeg(buf, at, 739, 1080, 800);
  at = AppendJpeg(buf, at, 739, 1080, 800);
  at = AppendJpeg(buf, at, 512, 512, 800);   /* an embedded figure */
  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, at, &index) == 3);
  CHECK(!ManualPages_LooksLikeAlbum(&index, at));
  free(buf);
}

static void TestVectorDocumentYieldsNothing(void) {
  /* No SOI anywhere: a text-and-fonts PDF. Zero is the CORRECT answer, and the
   * caller must rasterise or refuse rather than ship an empty manual. */
  unsigned char buf[4096];
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 31u + 7u);
  for (size_t i = 0; i + 1 < sizeof buf; i++) {
    if (buf[i] == 0xFF && buf[i + 1] == 0xD8) buf[i] = 0x00;
  }
  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, sizeof buf, &index) == 0);
  CHECK(!ManualPages_LooksLikeAlbum(&index, sizeof buf));
}

/* A thumbnail inside an APPn segment carries its own EOI. Cutting at the FIRST
 * 0xFFD9 would truncate the page to the thumbnail and silently lose the rest. */
static void TestThumbnailEoiDoesNotTruncateAPage(void) {
  unsigned char buf[8192];
  memset(buf, 0, sizeof buf);
  size_t at = 0;
  const unsigned char head[] = { 0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x0A };
  memcpy(buf + at, head, sizeof head); at += sizeof head;
  /* APP1 payload of 8 bytes containing a decoy EOI. */
  const unsigned char decoy[] = { 'E', 'x', 0xFF, 0xD9, 0x00, 0x00, 0x00, 0x00 };
  memcpy(buf + at, decoy, sizeof decoy); at += sizeof decoy;
  const unsigned char sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x04, 0x38,
                                0x02, 0xE3, 0x01, 0x01, 0x11, 0x00 };
  memcpy(buf + at, sof, sizeof sof); at += sizeof sof;
  const unsigned char sos[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 };
  memcpy(buf + at, sos, sizeof sos); at += sizeof sos;
  for (int i = 0; i < 500; i++) {
    const unsigned char byte = (unsigned char)(i + 1);
    buf[at++] = byte;
    if (byte == 0xFF) buf[at++] = 0x00;
  }
  buf[at++] = 0xFF; buf[at++] = 0xD9;
  const size_t total = at;

  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, total, &index) == 1);
  if (index.count == 1) {
    /* The page must run to the REAL EOI, not the decoy 20-odd bytes in. */
    CHECK(index.pages[0].length == total);
    CHECK(index.pages[0].width == 0x02E3);
    CHECK(index.pages[0].height == 0x0438);
  }
}

/* RESTART MARKERS. A scanner-produced JPEG often carries a DRI and RST0..RST7
 * inside the scan; they are payload, not structure. Neither fixture emitted one,
 * so deleting the RSTn clause from the scan walk left the suite green -- while
 * real restart-interval pages carved as ZERO. */
static void TestRestartMarkersInScanDataAreNotMistakenForStructure(void) {
  unsigned char buf[4096];
  memset(buf, 0, sizeof buf);
  size_t at = 0;
  const unsigned char head[] = { 0xFF, 0xD8 };
  memcpy(buf + at, head, sizeof head); at += sizeof head;
  /* DRI: define restart interval. */
  const unsigned char dri[] = { 0xFF, 0xDD, 0x00, 0x04, 0x00, 0x01 };
  memcpy(buf + at, dri, sizeof dri); at += sizeof dri;
  const unsigned char sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x04, 0x38,
                                0x02, 0xE3, 0x01, 0x01, 0x11, 0x00 };
  memcpy(buf + at, sof, sizeof sof); at += sizeof sof;
  const unsigned char sos[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
                                0x00, 0x00, 0x3F, 0x00 };
  memcpy(buf + at, sos, sizeof sos); at += sizeof sos;
  /* Entropy data interleaved with all eight restart markers. */
  for (int block = 0; block < 8; block++) {
    for (int i = 0; i < 24; i++) {
      const unsigned char byte = (unsigned char)(i * 11u + block);
      buf[at++] = byte;
      if (byte == 0xFF) buf[at++] = 0x00;
    }
    buf[at++] = 0xFF;
    buf[at++] = (unsigned char)(0xD0 + block);   /* RSTn */
  }
  buf[at++] = 0xFF; buf[at++] = 0xD9;
  const size_t total = at;

  ManualPageIndex index;
  CHECK(ManualPages_CarveAlbum(buf, total, &index) == 1);
  if (index.count == 1) {
    /* The page must span to the REAL EOI, past every restart marker. */
    CHECK(index.pages[0].length == total);
    CHECK(index.pages[0].width == 0x02E3);
    CHECK(index.pages[0].height == 0x0438);
  }
}

/* A COMPLETE NESTED JPEG inside an APPn segment -- an Exif thumbnail, which real
 * scanner and camera output routinely carries.
 *
 * The existing decoy-EOI fixture has no nested SOI, so it does not exercise the
 * `at = end` advance: with `at += 2` instead, the scan re-enters the page it just
 * recorded and emits the THUMBNAIL as a second page. A 40-page album would carve
 * as 80 pages of alternating page/thumbnail, and the geometry check would then
 * reject the whole manual. */
static void TestNestedThumbnailIsNotCarvedAsItsOwnPage(void) {
  unsigned char buf[8192];
  memset(buf, 0, sizeof buf);

  /* Build a complete little JPEG to embed. */
  unsigned char thumb[512];
  const size_t thumb_len = AppendJpeg(thumb, 0, 160, 120, 40);
  CHECK(thumb_len < sizeof thumb);

  size_t at = 0;
  buf[at++] = 0xFF; buf[at++] = 0xD8;                     /* outer SOI */
  /* APP1 whose payload IS the complete thumbnail. */
  const size_t seg = thumb_len + 2;
  buf[at++] = 0xFF; buf[at++] = 0xE1;
  buf[at++] = (unsigned char)(seg >> 8);
  buf[at++] = (unsigned char)(seg & 0xFF);
  memcpy(buf + at, thumb, thumb_len); at += thumb_len;
  const unsigned char sof[] = { 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x04, 0x38,
                                0x02, 0xE3, 0x01, 0x01, 0x11, 0x00 };
  memcpy(buf + at, sof, sizeof sof); at += sizeof sof;
  const unsigned char sos[] = { 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
                                0x00, 0x00, 0x3F, 0x00 };
  memcpy(buf + at, sos, sizeof sos); at += sizeof sos;
  for (int i = 0; i < 300; i++) {
    const unsigned char byte = (unsigned char)(i * 13u + 5u);
    buf[at++] = byte;
    if (byte == 0xFF) buf[at++] = 0x00;
  }
  buf[at++] = 0xFF; buf[at++] = 0xD9;                     /* outer EOI */
  const size_t total = at;

  ManualPageIndex index;
  /* ONE page: the outer image. The nested thumbnail is part of it, not a page. */
  CHECK(ManualPages_CarveAlbum(buf, total, &index) == 1);
  if (index.count == 1) {
    CHECK(index.pages[0].length == total);
    CHECK(index.pages[0].width == 0x02E3);   /* the OUTER geometry, not 160x120 */
    CHECK(index.pages[0].height == 0x0438);
  }
}

static void TestCarveRespectsThePageCap(void) {
  /* Cheap synthetic minimum-size pages, more than the cap allows. */
  const size_t each = 40;
  const size_t total = each * (kManualMaxPages + 8);
  unsigned char *buf = (unsigned char *)calloc(1, total);
  CHECK(buf != NULL);
  if (!buf) return;
  size_t at = 0;
  for (int i = 0; i < kManualMaxPages + 8 && at + each <= total; i++)
    at = AppendJpeg(buf, at, 739, 1080, 1);
  ManualPageIndex index;
  const int found = ManualPages_CarveAlbum(buf, at, &index);
  CHECK(found == kManualMaxPages);
  free(buf);
}

/* ── Fit, zoom, pan ───────────────────────────────────────────────────────── */

static void TestFitIsExactOnTheConstrainingAxis(void) {
  float w = 0, h = 0;
  /* A 739x1080 page (portrait) in a 1920x1080 view is HEIGHT-constrained. */
  ManualView_FittedSize(739, 1080, 1920, 1080, 1.0f, &w, &h);
  CHECK(fabsf(h - 1080.0f) < 0.01f);
  CHECK(w < 1920.0f);
  /* Aspect preserved. */
  CHECK(fabsf((w / h) - (739.0f / 1080.0f)) < 0.001f);

  /* A short wide view is width-constrained instead. */
  ManualView_FittedSize(739, 1080, 400, 4000, 1.0f, &w, &h);
  CHECK(fabsf(w - 400.0f) < 0.01f);
  CHECK(h < 4000.0f);
}

static void TestAFittedPageCannotBePanned(void) {
  ManualView view;
  ManualView_Init(&view);
  ManualView_Pan(&view, 500.0f, -500.0f, 739, 1080, 1920, 1080);
  /* At fit there is no overhang, so panning is correctly a no-op. */
  CHECK(view.pan_x == 0.0f);
  CHECK(view.pan_y == 0.0f);
}

static void TestPanIsClampedToTheOverhang(void) {
  ManualView view;
  ManualView_Init(&view);
  ManualView_Zoom(&view, 3.0f, 739, 1080, 1920, 1080);
  float lx = 0, ly = 0;
  ManualView_PanLimit(&view, 739, 1080, 1920, 1080, &lx, &ly);
  CHECK(ly > 0.0f);   /* zoomed 3x, the page overhangs vertically */
  ManualView_Pan(&view, 0.0f, 99999.0f, 739, 1080, 1920, 1080);
  CHECK(fabsf(view.pan_y - ly) < 0.01f);
  ManualView_Pan(&view, 0.0f, -99999.0f, 739, 1080, 1920, 1080);
  CHECK(fabsf(view.pan_y + ly) < 0.01f);
}

/* Zooming OUT must drag the pan back inside the shrinking overhang. Without the
 * re-clamp the page parks off-centre with the user having done nothing. */
static void TestZoomingOutReClampsPan(void) {
  ManualView view;
  ManualView_Init(&view);
  ManualView_Zoom(&view, 5.0f, 739, 1080, 1920, 1080);
  ManualView_Pan(&view, 0.0f, 99999.0f, 739, 1080, 1920, 1080);
  const float panned = view.pan_y;
  CHECK(panned > 0.0f);

  ManualView_Zoom(&view, 0.2f, 739, 1080, 1920, 1080);   /* back to fit */
  float lx = 0, ly = 0;
  ManualView_PanLimit(&view, 739, 1080, 1920, 1080, &lx, &ly);
  CHECK(fabsf(view.pan_y) <= ly + 0.01f);
  CHECK(view.pan_y < panned);
}

static void TestZoomIsClampedBothWays(void) {
  ManualView view;
  ManualView_Init(&view);
  for (int i = 0; i < 40; i++)
    ManualView_Zoom(&view, 2.0f, 739, 1080, 1920, 1080);
  CHECK(view.zoom <= (float)kManualZoomMaxPermille / 1000.0f + 0.001f);
  for (int i = 0; i < 40; i++)
    ManualView_Zoom(&view, 0.5f, 739, 1080, 1920, 1080);
  CHECK(view.zoom >= (float)kManualZoomMinPermille / 1000.0f - 0.001f);
  /* Nonsense factors are ignored rather than poisoning the state with NaN. */
  const float before = view.zoom;
  ManualView_Zoom(&view, 0.0f, 739, 1080, 1920, 1080);
  ManualView_Zoom(&view, -1.0f, 739, 1080, 1920, 1080);
  ManualView_Zoom(&view, NAN, 739, 1080, 1920, 1080);
  CHECK(view.zoom == before);
}

/* ── Paging ───────────────────────────────────────────────────────────────── */

static void TestTurnsStopAtBothEndsOfTheBooklet(void) {
  ManualView view;
  ManualView_Init(&view);
  CHECK(!ManualView_BeginTurn(&view, -1, 40));   /* already on page 0 */
  CHECK(ManualView_BeginTurn(&view, +1, 40));
  while (ManualView_AdvanceTurn(&view, 0.05f, 0.35f)) { /* run it out */ }
  CHECK(view.item == 1);

  ManualView_GoTo(&view, 39, 40);
  CHECK(!ManualView_BeginTurn(&view, +1, 40));   /* past the last page */
  CHECK(view.item == 39);
}

/* A held key must not queue turns: each would abort the last mid-flight and the
 * reader would skip pages the user never saw. */
static void TestATurnInFlightRefusesAnother(void) {
  ManualView view;
  ManualView_Init(&view);
  CHECK(ManualView_BeginTurn(&view, +1, 40));
  CHECK(!ManualView_BeginTurn(&view, +1, 40));
  CHECK(!ManualView_BeginTurn(&view, -1, 40));
  CHECK(view.turn_target == 1);
}

static void TestTurnLandsAndResetsZoom(void) {
  ManualView view;
  ManualView_Init(&view);
  ManualView_Zoom(&view, 4.0f, 739, 1080, 1920, 1080);
  ManualView_Pan(&view, 0.0f, 300.0f, 739, 1080, 1920, 1080);
  CHECK(ManualView_BeginTurn(&view, +1, 40));
  /* Arriving on a new sheet still zoomed into the old one's corner is
   * disorienting -- and a fitted destination is what keeps the draw order safe. */
  CHECK(view.zoom == 1.0f);
  CHECK(view.pan_x == 0.0f && view.pan_y == 0.0f);

  int guard = 0;
  while (ManualView_AdvanceTurn(&view, 1.0f / 60.0f, 0.35f) && guard++ < 1000) {}
  CHECK(view.turn == 0.0f);
  CHECK(view.item == 1);
}

/* Frame-rate independence: the turn must take the same wall-clock time whether
 * the host is running at 60 or 144 Hz. */
static void TestTurnDurationIsClockDriven(void) {
  const float dts[] = { 1.0f / 60.0f, 1.0f / 144.0f };
  for (int i = 0; i < 2; i++) {
    ManualView view;
    ManualView_Init(&view);
    CHECK(ManualView_BeginTurn(&view, +1, 40));
    float elapsed = 0.0f;
    int guard = 0;
    while (ManualView_AdvanceTurn(&view, dts[i], 0.35f) && guard++ < 100000)
      elapsed += dts[i];
    CHECK(fabsf(elapsed - 0.35f) < 0.05f);
  }
}

static void TestDegenerateTurnDurationLandsImmediately(void) {
  ManualView view;
  ManualView_Init(&view);
  CHECK(ManualView_BeginTurn(&view, +1, 40));
  CHECK(!ManualView_AdvanceTurn(&view, 0.016f, 0.0f));
  CHECK(view.item == 1);
  CHECK(view.turn == 0.0f);
}

/* ── Spread layout ───────────────────────────────────────────────────────────
 *
 * PARITY IS THE FRAGILE PART. Off by one and every interior opening pairs the
 * wrong halves, so a map drawn across the gutter shows its right half beside the
 * NEXT page's left -- plausible-looking and completely wrong. These assert the
 * real 40-page booklet page by page.
 */

static void TestSpreadLayoutOfTheRealBooklet(void) {
  const int pages = 40;
  /* front cover + 19 interior openings + back cover */
  CHECK(ManualPages_SpreadCount(pages) == 21);

  ManualSpread spread;
  /* Front cover stands alone on the right, like a closed book opening. */
  CHECK(ManualPages_SpreadAt(pages, 0, &spread));
  CHECK(spread.left == -1 && spread.right == 0);
  CHECK(ManualSpread_IsSingle(&spread));

  /* First interior opening pairs pages 2 and 3 (1-based), i.e. indices 1 and 2. */
  CHECK(ManualPages_SpreadAt(pages, 1, &spread));
  CHECK(spread.left == 1 && spread.right == 2);
  CHECK(!ManualSpread_IsSingle(&spread));

  /* Every interior opening must be (odd, even) in index terms -- that IS the
   * parity, and it is what keeps a two-page map together. */
  for (int i = 1; i <= 19; i++) {
    CHECK(ManualPages_SpreadAt(pages, i, &spread));
    CHECK(spread.left == 2 * i - 1);
    CHECK(spread.right == 2 * i);
    CHECK(spread.left % 2 == 1);
  }

  /* Back cover alone on the left. */
  CHECK(ManualPages_SpreadAt(pages, 20, &spread));
  CHECK(spread.left == 39 && spread.right == -1);
  CHECK(ManualSpread_IsSingle(&spread));

  /* Out of range is refused, not clamped -- a clamp would silently show the
   * wrong opening. */
  CHECK(!ManualPages_SpreadAt(pages, 21, &spread));
  CHECK(!ManualPages_SpreadAt(pages, -1, &spread));
}

/* Every page must appear exactly once across all openings, or the reader either
 * hides a page or shows one twice. */
static void TestEveryPageAppearsExactlyOnce(void) {
  for (int pages = 1; pages <= 41; pages++) {
    int seen[64];
    memset(seen, 0, sizeof seen);
    const int spreads = ManualPages_SpreadCount(pages);
    for (int i = 0; i < spreads; i++) {
      ManualSpread spread;
      CHECK(ManualPages_SpreadAt(pages, i, &spread));
      if (spread.left >= 0) seen[spread.left]++;
      if (spread.right >= 0) seen[spread.right]++;
    }
    for (int p = 0; p < pages; p++) {
      if (seen[p] != 1) {
        printf("  page_count=%d: page %d appears %d time(s)\n", pages, p, seen[p]);
        CHECK(seen[p] == 1);
      }
    }
  }
}

/* SpreadForPage is the inverse of SpreadAt, so "jump to page N" lands on the
 * opening that actually shows N. */
static void TestSpreadForPageIsTheInverse(void) {
  for (int pages = 1; pages <= 41; pages++) {
    for (int p = 0; p < pages; p++) {
      const int spread_index = ManualPages_SpreadForPage(pages, p);
      CHECK(spread_index >= 0);
      ManualSpread spread;
      CHECK(ManualPages_SpreadAt(pages, spread_index, &spread));
      CHECK(spread.left == p || spread.right == p);
    }
    CHECK(ManualPages_SpreadForPage(pages, -1) == -1);
    CHECK(ManualPages_SpreadForPage(pages, pages) == -1);
  }
}

/* An ODD interior count leaves one page without a partner. It must get its own
 * opening rather than being dropped. */
static void TestOddInteriorCountLeavesALonePage(void) {
  /* 5 pages: cover, [2,3], [4], back cover(5). */
  CHECK(ManualPages_SpreadCount(5) == 4);
  ManualSpread spread;
  CHECK(ManualPages_SpreadAt(5, 2, &spread));
  CHECK(spread.left == 3 && spread.right == -1);
  CHECK(ManualSpread_IsSingle(&spread));
}

static void TestDegenerateBookletSizes(void) {
  CHECK(ManualPages_SpreadCount(0) == 0);
  CHECK(ManualPages_SpreadCount(-3) == 0);
  ManualSpread spread;
  CHECK(!ManualPages_SpreadAt(0, 0, &spread));

  /* One page: a cover and nothing else. */
  CHECK(ManualPages_SpreadCount(1) == 1);
  CHECK(ManualPages_SpreadAt(1, 0, &spread));
  CHECK(spread.left == -1 && spread.right == 0);

  /* Two pages: front and back, no interior. */
  CHECK(ManualPages_SpreadCount(2) == 2);
  CHECK(ManualPages_SpreadAt(2, 0, &spread));
  CHECK(spread.right == 0 && spread.left == -1);
  CHECK(ManualPages_SpreadAt(2, 1, &spread));
  CHECK(spread.left == 1 && spread.right == -1);
}

/* ── What is on screen during a turn ─────────────────────────────────────────
 *
 * These two cases are the reported bugs: the underlying pages jumped to the
 * destination on the first frame of a turn, and a backward turn showed the
 * lifting page mirrored.
 */

/* THE CONTINUITY CONDITION. A page turn moves ONE sheet. The side it lifts FROM
 * reveals the new page; the side it falls TOWARD is unchanged until the leaf's
 * own back face covers it. Advancing both sides at once pops the stationary page
 * to its new value on frame one. */
static void TestForwardTurnLeavesTheLeftPageAlone(void) {
  const int pages = 40;
  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 5, ManualPages_SpreadCount(pages));

  ManualSpread settled;
  CHECK(ManualPages_SpreadAt(pages, 5, &settled));
  ManualSpread target;
  CHECK(ManualPages_SpreadAt(pages, 6, &target));

  ManualTurnFrame frame;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.left_page == settled.left);
  CHECK(frame.right_page == settled.right);

  CHECK(ManualView_BeginTurn(&view, +1, ManualPages_SpreadCount(pages)));
  /* Through the WHOLE animation the left page must stay put... */
  for (int i = 1; i < 40; i++) {
    view.turn = (float)i / 40.0f;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(frame.left_page == settled.left);
    /* ...while the right side shows what the lifting leaf uncovers. */
    CHECK(frame.right_page == target.right);
  }

  /* And no pop at the boundary: what the leaf's back face carries at the end is
   * exactly what the settled frame then draws on the left. */
  view.turn = 0.99f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(!ManualTurn_FrontFaceVisible(view.turn));
  const int landing = frame.leaf_page;
  view.turn = 0.0f;
  view.item = 6;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.left_page == landing);
  CHECK(frame.left_page == target.left);
}

static void TestBackwardTurnLeavesTheRightPageAlone(void) {
  const int pages = 40;
  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 6, ManualPages_SpreadCount(pages));

  ManualSpread settled, target;
  CHECK(ManualPages_SpreadAt(pages, 6, &settled));
  CHECK(ManualPages_SpreadAt(pages, 5, &target));

  CHECK(ManualView_BeginTurn(&view, -1, ManualPages_SpreadCount(pages)));
  ManualTurnFrame frame;
  for (int i = 1; i < 40; i++) {
    view.turn = -(float)i / 40.0f;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(frame.right_page == settled.right);   /* unchanged */
    CHECK(frame.left_page == target.left);      /* revealed */
  }
  /* Continuity at the boundary, mirrored. */
  view.turn = -0.99f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  const int landing = frame.leaf_page;
  view.turn = 0.0f;
  view.item = 5;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.right_page == landing);
  CHECK(frame.right_page == target.right);
}

/* The leaf shows the page you were reading first, then the page you are turning
 * to -- in BOTH directions. */
static void TestLeafShowsTheSheetsOwnTwoPages(void) {
  const int pages = 40;
  ManualSpread settled, next, previous;
  CHECK(ManualPages_SpreadAt(pages, 5, &settled));
  CHECK(ManualPages_SpreadAt(pages, 6, &next));
  CHECK(ManualPages_SpreadAt(pages, 4, &previous));

  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 5, ManualPages_SpreadCount(pages));
  ManualTurnFrame frame;

  view.turn = 0.2f;   view.turn_target = 6;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.leaf_page == settled.right);    /* the page being lifted */
  view.turn = 0.8f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.leaf_page == next.left);        /* its reverse */

  view.turn = -0.2f;  view.turn_target = 4;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.leaf_page == settled.left);
  view.turn = -0.8f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.leaf_page == previous.right);
}

/* THE MIRROR TABLE. The leaf's u runs 0 at the gutter always, but a LEFT page
 * meets the gutter on its right edge and a RIGHT page on its left -- so whether
 * the texture is flipped depends on the direction AND the visible face. It is an
 * exclusive-or. Mirroring on the face alone reverses every backward turn, which
 * is the reported bug. */
static void TestLeafMirrorIsDirectionXorFace(void) {
  const int pages = 40;
  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 5, ManualPages_SpreadCount(pages));
  ManualTurnFrame frame;

  const struct { float turn; bool want_mirror; const char *why; } cases[] = {
    {  0.2f, false, "forward, front: a RIGHT page, gutter on its left"  },
    {  0.8f, true,  "forward, back: a LEFT page, gutter on its right"   },
    { -0.2f, true,  "backward, front: a LEFT page"                      },
    { -0.8f, false, "backward, back: a RIGHT page"                      },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    view.turn = cases[i].turn;
    view.turn_target = cases[i].turn > 0.0f ? 6 : 4;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    if (frame.leaf_mirrored != cases[i].want_mirror)
      printf("  turn %+.1f: mirrored=%d want %d (%s)\n", (double)cases[i].turn,
             (int)frame.leaf_mirrored, (int)cases[i].want_mirror, cases[i].why);
    CHECK(frame.leaf_mirrored == cases[i].want_mirror);
  }
  /* Forward and backward at the SAME face must disagree -- that is the xor, and
   * it is what a face-only implementation gets wrong. */
  view.turn = 0.2f;  view.turn_target = 6;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  const bool forward_front = frame.leaf_mirrored;
  view.turn = -0.2f; view.turn_target = 4;
  CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
  CHECK(frame.leaf_mirrored != forward_front);
}

/* Single-page mode must resolve through the same call, with no left side. */
static void TestSinglePageModeResolves(void) {
  const int pages = 40;
  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 7, pages);
  ManualTurnFrame frame;
  CHECK(ManualTurn_ResolveFrame(&view, pages, false, &frame));
  CHECK(frame.left_page == -1);
  CHECK(frame.right_page == 7);

  CHECK(ManualView_BeginTurn(&view, +1, pages));
  view.turn = 0.3f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, false, &frame));
  CHECK(frame.right_page == 8);       /* revealed */
  CHECK(frame.leaf_page == 7);        /* lifting */
  view.turn = 0.8f;
  CHECK(ManualTurn_ResolveFrame(&view, pages, false, &frame));
  CHECK(frame.leaf_page >= 0);        /* a real page, never -1 */
}

/* Covers stand alone, so a turn off them must still resolve without asking for a
 * page that does not exist. */
static void TestTurnsAtTheCoversResolve(void) {
  const int pages = 40;
  const int items = ManualPages_SpreadCount(pages);
  ManualView view;
  ManualView_Init(&view);
  ManualTurnFrame frame;

  /* Front cover -> first interior opening. */
  ManualView_GoTo(&view, 0, items);
  CHECK(ManualView_BeginTurn(&view, +1, items));
  for (int i = 1; i < 20; i++) {
    view.turn = (float)i / 20.0f;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(frame.leaf_page >= 0 && frame.leaf_page < pages);
    CHECK(frame.right_page < pages);
    CHECK(frame.left_page < pages);
  }

  /* Last interior opening -> back cover. */
  ManualView_GoTo(&view, items - 2, items);
  CHECK(ManualView_BeginTurn(&view, +1, items));
  for (int i = 1; i < 20; i++) {
    view.turn = (float)i / 20.0f;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(frame.leaf_page >= 0 && frame.leaf_page < pages);
  }
}

/* THE LAYOUT WIDTH MUST NOT CHANGE BETWEEN OPENINGS.
 *
 * Sizing a single-page opening to one page and a two-up one to two makes the
 * whole view rescale 2x the instant a cover is turned, so everything jumps
 * mid-animation. The area is always two pages wide; a lone page takes one half.
 * These assert the SIDE, which is what keeps a cover across the correct half. */
/* The layout width must be CONSTANT across every opening, covers included. This
 * is the invariant whose absence made the animation glitch on single pages: the
 * area was one page wide on a cover and two on an interior opening, so the whole
 * view rescaled 2x mid-turn. */
static void TestLayoutWidthNeverDependsOnTheOpening(void) {
  CHECK(ManualPages_LayoutPageWidths(true) == 2);
  CHECK(ManualPages_LayoutPageWidths(false) == 1);

  /* Walk every opening of the real booklet: the width must not move. */
  const int pages = 40;
  const int items = ManualPages_SpreadCount(pages);
  const int expected = ManualPages_LayoutPageWidths(true);
  for (int i = 0; i < items; i++) {
    ManualSpread spread;
    CHECK(ManualPages_SpreadAt(pages, i, &spread));
    /* Whatever the opening holds -- one page or two -- the layout is the same. */
    CHECK(ManualPages_LayoutPageWidths(true) == expected);
    /* And the fitted size follows from that width alone, so it is identical on a
     * cover and on an interior opening. */
    float w = 0.0f, h = 0.0f;
    ManualView_FittedSize(739 * expected, 1080, 1600, 1040, 1.0f, &w, &h);
    CHECK(fabsf(w - 1423.3f) < 0.5f);
    CHECK(fabsf(h - 1040.0f) < 0.5f);
  }
}

static void TestSinglePageOpeningsPickTheCorrectHalf(void) {
  const int pages = 40;
  const int items = ManualPages_SpreadCount(pages);
  ManualSpread spread;

  /* Front cover: alone on the RIGHT, so it swings leftward like real paper. */
  CHECK(ManualPages_SpreadAt(pages, 0, &spread));
  CHECK(ManualSpread_IsSingle(&spread));
  CHECK(ManualSpread_SingleOnRight(&spread));

  /* Back cover: alone on the LEFT. */
  CHECK(ManualPages_SpreadAt(pages, items - 1, &spread));
  CHECK(ManualSpread_IsSingle(&spread));
  CHECK(!ManualSpread_SingleOnRight(&spread));

  /* A two-up opening has no "side" -- the query is meaningless and must not
   * accidentally report the right half. */
  CHECK(ManualPages_SpreadAt(pages, 5, &spread));
  CHECK(!ManualSpread_IsSingle(&spread));
  CHECK(!ManualSpread_SingleOnRight(&spread));

  /* A lone ODD interior page sits on the left, because it is the left half of an
   * opening whose right half does not exist. */
  CHECK(ManualPages_SpreadAt(5, 2, &spread));
  CHECK(ManualSpread_IsSingle(&spread));
  CHECK(!ManualSpread_SingleOnRight(&spread));
}

/* DRIVE THE STATE MACHINE BACKWARDS THROUGH ITS REAL API.
 *
 * The two backward tests above set view.turn by HAND, so no test ever ran
 * BeginTurn(-1) -> AdvanceTurn -> completion. That left the sign of the turn
 * unguarded in two places: BeginTurn's initial direction, and AdvanceTurn's
 * `forward` test that reapplies it. Both mutations survived the whole suite while
 * making a backward turn animate forwards.
 *
 * The fix is to exercise the API the way the reader does, and assert the SIGN of
 * turn at every step -- not just that it ends up on the right item, which both
 * mutants also did. */
static void TestBackwardTurnDrivenThroughTheRealApi(void) {
  const int pages = 40;
  const int items = ManualPages_SpreadCount(pages);
  ManualView view;
  ManualView_Init(&view);
  ManualView_GoTo(&view, 6, items);

  CHECK(ManualView_BeginTurn(&view, -1, items));
  /* A backward turn must be NEGATIVE from the very first frame -- that sign is
   * what tells the renderer which leaf lifts and which way it mirrors. */
  CHECK(view.turn < 0.0f);
  CHECK(view.turn_target == 5);

  int steps = 0;
  float previous = view.turn;
  while (ManualView_AdvanceTurn(&view, 1.0f / 60.0f, 0.35f)) {
    CHECK(view.turn < 0.0f);                    /* stays negative throughout */
    CHECK(fabsf(view.turn) >= fabsf(previous)); /* magnitude only grows */
    /* And the resolved frame agrees on the direction every single step. */
    ManualTurnFrame frame;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(!frame.leaf_on_right);                /* backward lifts the LEFT leaf */
    previous = view.turn;
    if (++steps > 1000) break;
  }
  CHECK(steps > 3);                             /* it really animated */
  CHECK(view.turn == 0.0f);
  CHECK(view.item == 5);

  /* Forward, for contrast: the same drive must be POSITIVE throughout. */
  ManualView_GoTo(&view, 6, items);
  CHECK(ManualView_BeginTurn(&view, +1, items));
  CHECK(view.turn > 0.0f);
  while (ManualView_AdvanceTurn(&view, 1.0f / 60.0f, 0.35f)) {
    CHECK(view.turn > 0.0f);
    ManualTurnFrame frame;
    CHECK(ManualTurn_ResolveFrame(&view, pages, true, &frame));
    CHECK(frame.leaf_on_right);                 /* forward lifts the RIGHT leaf */
  }
  CHECK(view.item == 7);
}

/* ── The ordering invariant ───────────────────────────────────────────────── */

/* THE load-bearing assertion of this module.
 *
 * SDL_RenderGeometry has no depth test and no backface culling, so the reader
 * draws backdrop -> destination page -> shadow -> leaf ONCE, with no
 * mid-animation reordering. That is only correct because the turning leaf never
 * dips behind the settled pages it passes over. Settled pages sit at negative z;
 * the leaf must stay at z >= 0 for every phase and every point on the sheet. */
static void TestLeafNeverGoesBehindASettledPage(void) {
  for (int step = -100; step <= 100; step++) {
    const float turn = (float)step / 100.0f;
    for (int ui = 0; ui <= 20; ui++) {
      for (int vi = 0; vi <= 4; vi++) {
        float x = 0, y = 0, z = 0;
        ManualTurn_LeafPoint(turn, (float)ui / 20.0f, (float)vi / 4.0f,
                             &x, &y, &z);
        CHECK(z >= 0.0f);
        CHECK(isfinite(x) && isfinite(y) && isfinite(z));
        /* The sheet is a UNIT sheet centred on the origin: x and y each stay
         * within [-0.5, 0.5] so that at rest it is exactly the rectangle a
         * settled page occupies. That coincidence is what makes the first frame
         * of a turn invisible -- when the leaf was scaled independently it
         * rendered at 80% of the page's height and the page appeared to shrink
         * the moment it was touched. */
        CHECK(fabsf(x) <= 0.501f);
        CHECK(fabsf(y) <= 0.501f);
        /* The lift cannot exceed the sheet's own width, since it is a rotation
         * of that width out of the page. */
        CHECK(z <= 1.001f);
      }
    }
  }
}

/* THE INVARIANT THAT MAKES THE BOW SAFE, and the reason a bow is allowed where a
 * free curl is not.
 *
 * A bowed sheet DOES fold back on itself in screen x near the hinge when it is
 * edge-on -- measured ~14 px of overlap at the shipped amplitude -- so it
 * genuinely self-occludes, and SDL_RenderGeometry has no depth test to sort it.
 * Painter's order still renders it correctly for one reason only: the mesh emits
 * u ascending, and z is MONOTONICALLY NON-DECREASING in u, so the later-drawn
 * strip of any overlap is always the nearer one.
 *
 * Break monotonicity and the fold renders inside-out. That is why the amplitude
 * is bounded rather than tuned by eye. */
/* The renderer's mesh must be emitted with u as the OUTER loop, because depth
 * rises with u and painter's order is the only thing resolving the bowed sheet's
 * self-overlap. This asserts the module-side premise; the renderer's own ordering
 * is a constraint the header now states explicitly. */
static void TestDepthRisesWithUAtEveryPhase(void) {
  for (int i = -100; i <= 100; i++) {
    const float turn = (float)i / 100.0f;
    CHECK(ManualTurn_DepthRisesWithU(turn, 200));
  }
  /* Degenerate sample counts are refused rather than reporting success. */
  CHECK(!ManualTurn_DepthRisesWithU(0.5f, 1));
  CHECK(!ManualTurn_DepthRisesWithU(0.5f, 0));
}

static void TestBowKeepsDepthMonotonicInU(void) {
  for (int step = -200; step <= 200; step++) {
    const float turn = (float)step / 200.0f;
    float previous_z = -1.0f;
    for (int iu = 0; iu <= 200; iu++) {
      float x = 0, y = 0, z = 0;
      ManualTurn_LeafPoint(turn, (float)iu / 200.0f, 0.5f, &x, &y, &z);
      /* Non-decreasing, so the nearer part of a fold is always drawn later. */
      CHECK(z >= previous_z - 1e-5f);
      previous_z = z;
    }
  }
}

/* The bow must vanish wherever the sheet has to coincide with something else:
 * flat at rest and at the landing, and pinned at the hinge and the free edge. */
static void TestBowVanishesAtTheEndsAndEdges(void) {
  /* At rest and landed: dead flat, or the sheet would not match the page. */
  for (int iu = 0; iu <= 20; iu++) {
    const float u = (float)iu / 20.0f;
    CHECK(fabsf(ManualTurn_BowOffset(0.0f, u)) < 1e-6f);
    CHECK(fabsf(ManualTurn_BowOffset(1.0f, u)) < 1e-5f);
    CHECK(fabsf(ManualTurn_BowOffset(-1.0f, u)) < 1e-5f);
  }
  /* Pinned at both edges for every phase. */
  for (int i = -20; i <= 20; i++) {
    const float turn = (float)i / 20.0f;
    CHECK(fabsf(ManualTurn_BowOffset(turn, 0.0f)) < 1e-6f);
    CHECK(fabsf(ManualTurn_BowOffset(turn, 1.0f)) < 1e-5f);
  }
  /* And it actually bows in between -- a zero-amplitude "curl" would pass every
   * assertion above while doing nothing. */
  CHECK(ManualTurn_BowOffset(0.5f, 0.5f) > 0.01f);

  /* The amplitude must stay inside the proven bound. */
  CHECK(kManualCurlPermille <= kManualCurlMaxPermille);
  float peak = 0.0f;
  for (int i = -100; i <= 100; i++)
    for (int iu = 0; iu <= 100; iu++) {
      const float b = fabsf(ManualTurn_BowOffset((float)i / 100.0f,
                                                 (float)iu / 100.0f));
      if (b > peak) peak = b;
    }
  CHECK(peak <= (float)kManualCurlMaxPermille / 1000.0f + 1e-4f);
}

/* The bow must displace along the sheet's OWN NORMAL, not along +z.
 *
 * A probe that applied it as a bare z offset passed every other assertion: the
 * ends stay pinned, z stays monotonic, the amplitude is bounded. But the sheet
 * would balloon toward the camera instead of BENDING, and the give-away is that a
 * normal-directed bow also shifts x -- by -bow*sin(angle), which is largest when
 * the sheet is edge-on and exactly zero at rest and at the landing.
 *
 * Without this case the difference between "bends like paper" and "inflates like
 * a balloon" is untested. */
static void TestBowFollowsTheSurfaceNormal(void) {
  /* Edge-on, mid-span: the bow is nearly all in x, because the sheet's normal is
   * nearly horizontal there. */
  float x = 0, y = 0, z = 0;
  ManualTurn_LeafPoint(0.5f, 0.5f, 0.5f, &x, &y, &z);
  const float bow = ManualTurn_BowOffset(0.5f, 0.5f);
  const float angle = ManualTurn_HingeAngle(0.5f);
  const float span = 0.5f * 0.5f;
  /* x must be displaced from the rigid position by -bow*sin(angle). */
  const float rigid_x = span * cosf(angle);
  CHECK(fabsf((x - rigid_x) + bow * sinf(angle)) < 1e-4f);
  /* And that displacement is real, not rounding. */
  CHECK(fabsf(x - rigid_x) > 1e-3f);

  /* Sweeping the phase, the x displacement must track sin(angle) -- zero at both
   * ends, maximal edge-on. A +z-only bow leaves x untouched throughout. */
  float peak_dx = 0.0f;
  for (int i = 0; i <= 100; i++) {
    const float turn = (float)i / 100.0f;
    const float a = ManualTurn_HingeAngle(turn);
    const float b = ManualTurn_BowOffset(turn, 0.5f);
    float px = 0, py = 0, pz = 0;
    ManualTurn_LeafPoint(turn, 0.5f, 0.5f, &px, &py, &pz);
    const float expected_dx = -b * sinf(a);
    CHECK(fabsf((px - 0.25f * cosf(a)) - expected_dx) < 1e-4f);
    if (fabsf(expected_dx) > peak_dx) peak_dx = fabsf(expected_dx);
  }
  CHECK(peak_dx > 0.01f);   /* the normal really does tilt out of z */
}

/* At rest the bowed sheet must STILL be exactly the settled page -- the bow
 * cannot reintroduce the mismatch that made a turn look like a resize. */
static void TestBowedSheetStillMatchesThePageAtRest(void) {
  float x = 0, y = 0, z = 0;
  ManualTurn_LeafPoint(0.0f, 0.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x) < 1e-6f);
  CHECK(fabsf(z) < 1e-6f);
  ManualTurn_LeafPoint(0.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x - 0.5f) < 1e-6f);
  CHECK(fabsf(z) < 1e-6f);
  /* And landed, on the facing page's outer edge. */
  ManualTurn_LeafPoint(1.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x + 0.5f) < 1e-4f);
  CHECK(fabsf(z) < 1e-4f);
}

static void TestHingeSweepsAHalfTurnAndIsMonotonic(void) {
  CHECK(fabsf(ManualTurn_HingeAngle(0.0f)) < 1e-6f);
  CHECK(fabsf(ManualTurn_HingeAngle(1.0f) - (float)M_PI) < 1e-5f);
  /* Monotone, or the sheet would visibly stutter backward mid-turn. */
  float previous = -1.0f;
  for (int i = 0; i <= 100; i++) {
    const float a = ManualTurn_HingeAngle((float)i / 100.0f);
    CHECK(a >= previous - 1e-6f);
    previous = a;
  }
  /* Eased at both ends: the first and last steps move less than a middle one. */
  const float first = ManualTurn_HingeAngle(0.02f) - ManualTurn_HingeAngle(0.0f);
  const float middle = ManualTurn_HingeAngle(0.52f) - ManualTurn_HingeAngle(0.5f);
  CHECK(first < middle);
}

/* The two directions must be exact mirrors, or a backward turn animates as though
 * it were going forward.
 *
 * THE PROBE POINT MATTERS. This case used to sample turn=+-0.5 at u=1.0, which is
 * doubly degenerate: HingeAngle(0.5) is exactly pi/2 so span*cos(angle) is 0, AND
 * the bow vanishes at u=1 by construction. Both sides were ~1.6e-8 and the
 * assertion compared zero to zero -- deleting `if (turn < 0.0f) x = -x;` left the
 * whole suite green while every backward turn ran forwards. Sample AWAY from the
 * degenerate phase, and assert the sign explicitly rather than only the symmetry:
 * a mirror of two zeroes is still a mirror. */
static void TestBackwardTurnMirrorsTheHinge(void) {
  /* Off-centre phases, where x is substantially non-zero on both sides. */
  const float phases[] = { 0.15f, 0.3f, 0.7f, 0.85f };
  for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
    const float t = phases[i];
    float fx = 0, fy = 0, fz = 0, bx = 0, by = 0, bz = 0;
    ManualTurn_LeafPoint(t, 1.0f, 0.5f, &fx, &fy, &fz);
    ManualTurn_LeafPoint(-t, 1.0f, 0.5f, &bx, &by, &bz);
    /* Substantial, so neither side can be an accidental zero. */
    CHECK(fabsf(fx) > 0.05f);
    CHECK(fabsf(bx) > 0.05f);
    /* OPPOSITE SIGNS -- this is what a deleted mirror breaks. */
    CHECK(fx * bx < 0.0f);
    CHECK(fabsf(fx + bx) < 1e-5f);   /* and an exact mirror */
    CHECK(fabsf(fz - bz) < 1e-5f);   /* same lift either way */
  }

  /* The landing is the strongest single assertion: a forward turn ends on the
   * LEFT outer edge, a backward turn on the RIGHT. */
  float x = 0, y = 0, z = 0;
  ManualTurn_LeafPoint(1.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x + 0.5f) < 1e-4f);
  ManualTurn_LeafPoint(-1.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x - 0.5f) < 1e-4f);

  /* And across the whole sweep the sheet stays on its own side of the gutter
   * until it crosses, so a mid-turn frame can never be on the wrong half. */
  for (int i = 1; i < 50; i++) {
    const float t = (float)i / 100.0f;     /* first half only: not yet crossed */
    float fwd = 0, back = 0, ignored = 0;
    ManualTurn_LeafPoint(t, 1.0f, 0.5f, &fwd, &ignored, &ignored);
    ManualTurn_LeafPoint(-t, 1.0f, 0.5f, &back, &ignored, &ignored);
    CHECK(fwd > 0.0f);    /* forward lifts from the RIGHT half */
    CHECK(back < 0.0f);   /* backward from the LEFT */
  }
}

/* THE HINGE IS THE GUTTER. A forward turn lifts the right leaf and lays it on
 * the left, so the free edge travels from +0.5 to exactly -0.5 -- covering the
 * facing page and no more.
 *
 * Hinging on the sheet's own outer edge instead put the landed free edge at
 * x = -1.5, a full sheet-width past the page: the leaf flipped onto empty space.
 * My first version had exactly that bug and this case is what found it. */
static void TestTurnPivotsOnTheGutter(void) {
  float x = 0, y = 0, z = 0;

  /* At rest: gutter edge at 0, free edge at +0.5, flat. */
  ManualTurn_LeafPoint(0.0f, 0.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x) < 1e-5f);
  CHECK(fabsf(z) < 1e-5f);
  ManualTurn_LeafPoint(0.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x - 0.5f) < 1e-5f);
  CHECK(fabsf(z) < 1e-5f);

  /* Landed: the free edge has crossed to the facing page's outer edge, EXACTLY. */
  ManualTurn_LeafPoint(1.0f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x + 0.5f) < 1e-5f);
  CHECK(fabsf(z) < 1e-5f);

  /* Halfway: standing upright on the gutter, lifted by its own half-width. */
  ManualTurn_LeafPoint(0.5f, 1.0f, 0.5f, &x, &y, &z);
  CHECK(fabsf(x) < 1e-4f);
  CHECK(fabsf(z - 0.5f) < 1e-4f);

  /* The gutter edge NEVER moves, at any phase -- it is the spine. */
  for (int i = -100; i <= 100; i++) {
    ManualTurn_LeafPoint((float)i / 100.0f, 0.0f, 0.5f, &x, &y, &z);
    CHECK(fabsf(x) < 1e-5f);
    CHECK(fabsf(z) < 1e-5f);
  }

  /* Full vertical extent regardless of phase, so the sheet is never short. */
  float top = 0, bottom = 0, ignored = 0;
  ManualTurn_LeafPoint(0.37f, 0.5f, 0.0f, &ignored, &top, &ignored);
  ManualTurn_LeafPoint(0.37f, 0.5f, 1.0f, &ignored, &bottom, &ignored);
  CHECK(fabsf(top + 0.5f) < 1e-5f);
  CHECK(fabsf(bottom - 0.5f) < 1e-5f);
}

/* SheetExtents must make a z=0 unit sheet project to EXACTLY the requested pixel
 * size. This is the arithmetic that replaced hand-picked scale factors. */
static void TestSheetExtentsReproduceTheRequestedSize(void) {
  const int view_w = 960, view_h = 1040;
  const float page_w = 711.6f, page_h = 1040.0f;
  /* Only the UNTILTED camera is asserted to the pixel. With a tilt the sheet is
   * genuinely in perspective -- a step along world x also moves in screen y, and
   * the near edge is larger than the far one -- so "one unit == N pixels" is not
   * meant to hold and asserting it would be asserting the absence of perspective.
   * What must hold for BOTH is that the flat page and the leaf agree, which
   * TestTurnPivotsOnTheGutter and the ordering test cover. */
  const float tilts[][2] = { { 0.0f, 0.0f } };
  for (int t = 0; t < 1; t++) {
    Scene3DCamera camera = { tilts[t][0], tilts[t][1], 2.6f, 0.9f };
    float matrix[16];
    Scene3D_BuildViewProjection(&camera, view_w, view_h, matrix);
    float half_x = 0.0f, half_y = 0.0f;
    CHECK(ManualTurn_SheetExtents(matrix, view_w, view_h, page_w, page_h,
                                  &half_x, &half_y));
    CHECK(half_x > 0.0f && half_y > 0.0f);

    /* A one-unit step must be exactly the requested pixel count, on both axes.
     * That equality IS the fix: hand-picked scale factors had the leaf at 80% of
     * the page's height, so the page appeared to shrink at the start of a turn. */
    Scene3DPoint left, right, top, bottom;
    CHECK(Scene3D_ProjectWorldPoint(matrix, -half_x, 0.0f, 0.0f,
                                    view_w, view_h, &left));
    CHECK(Scene3D_ProjectWorldPoint(matrix, half_x, 0.0f, 0.0f,
                                    view_w, view_h, &right));
    CHECK(Scene3D_ProjectWorldPoint(matrix, 0.0f, half_y, 0.0f,
                                    view_w, view_h, &top));
    CHECK(Scene3D_ProjectWorldPoint(matrix, 0.0f, -half_y, 0.0f,
                                    view_w, view_h, &bottom));
    CHECK(fabsf((right.x - left.x) - page_w) < 0.05f);
    CHECK(fabsf((bottom.y - top.y) - page_h) < 0.05f);

    /* And the leaf at rest occupies the gutter-to-edge half of that, so its own
     * span is exactly half the requested width. */
    float lx0 = 0, ly0 = 0, lz0 = 0, lx1 = 0, ly1 = 0, lz1 = 0;
    ManualTurn_LeafPoint(0.0f, 0.0f, 0.5f, &lx0, &ly0, &lz0);
    ManualTurn_LeafPoint(0.0f, 1.0f, 0.5f, &lx1, &ly1, &lz1);
    Scene3DPoint gutter, edge;
    CHECK(Scene3D_ProjectWorldPoint(matrix, lx0 * 2.0f * half_x, 0.0f, 0.0f,
                                    view_w, view_h, &gutter));
    CHECK(Scene3D_ProjectWorldPoint(matrix, lx1 * 2.0f * half_x, 0.0f, 0.0f,
                                    view_w, view_h, &edge));
    CHECK(fabsf(fabsf(edge.x - gutter.x) - page_w * 0.5f) < 0.05f);
  }
  /* Degenerate inputs are refused rather than producing nonsense extents. */
  float matrix[16];
  Scene3DCamera camera = { 0.0f, 0.0f, 2.6f, 0.9f };
  Scene3D_BuildViewProjection(&camera, view_w, view_h, matrix);
  float hx = 0.0f, hy = 0.0f;
  CHECK(!ManualTurn_SheetExtents(matrix, 0, view_h, page_w, page_h, &hx, &hy));
  CHECK(!ManualTurn_SheetExtents(matrix, view_w, view_h, 0.0f, page_h, &hx, &hy));
  CHECK(!ManualTurn_SheetExtents(NULL, view_w, view_h, page_w, page_h, &hx, &hy));
}

static void TestFaceFlipsAtTheHalfway(void) {
  CHECK(ManualTurn_FrontFaceVisible(0.0f));
  CHECK(!ManualTurn_FrontFaceVisible(1.0f));
  /* Exactly one flip across the sweep -- if the predicate oscillated, the
   * texture would visibly pop back and forth mid-turn. */
  int flips = 0;
  bool previous = ManualTurn_FrontFaceVisible(0.0f);
  for (int i = 1; i <= 200; i++) {
    const bool now = ManualTurn_FrontFaceVisible((float)i / 200.0f);
    if (now != previous) flips++;
    previous = now;
  }
  CHECK(flips == 1);
}

static void TestShadeIsBoundedAndDimmestEdgeOn(void) {
  for (int i = 0; i <= 100; i++) {
    const float t = (float)i / 100.0f;
    for (int ui = 0; ui <= 10; ui++) {
      const float s = ManualTurn_LeafShade(t, (float)ui / 10.0f);
      CHECK(s >= 0.0f && s <= 1.0f);
      CHECK(s > 0.0f);            /* never pure black */
    }
  }
  /* Edge-on (halfway) is dimmer than flat at either end. */
  CHECK(ManualTurn_LeafShade(0.5f, 1.0f) < ManualTurn_LeafShade(0.0f, 1.0f));
  CHECK(ManualTurn_LeafShade(0.5f, 1.0f) < ManualTurn_LeafShade(1.0f, 1.0f));
}

int main(void) {
  TestCarvesEveryPageOfAnAlbum();
  TestLetterheadDocumentIsNotAnAlbum();
  TestASingleDominantImageIsNotAnAlbum();
  TestMixedGeometryIsNotAnAlbum();
  TestVectorDocumentYieldsNothing();
  TestThumbnailEoiDoesNotTruncateAPage();
  TestNestedThumbnailIsNotCarvedAsItsOwnPage();
  TestRestartMarkersInScanDataAreNotMistakenForStructure();
  TestCarveRespectsThePageCap();

  TestFitIsExactOnTheConstrainingAxis();
  TestAFittedPageCannotBePanned();
  TestPanIsClampedToTheOverhang();
  TestZoomingOutReClampsPan();
  TestZoomIsClampedBothWays();

  TestTurnsStopAtBothEndsOfTheBooklet();
  TestATurnInFlightRefusesAnother();
  TestTurnLandsAndResetsZoom();
  TestTurnDurationIsClockDriven();
  TestDegenerateTurnDurationLandsImmediately();

  TestSpreadLayoutOfTheRealBooklet();
  TestEveryPageAppearsExactlyOnce();
  TestSpreadForPageIsTheInverse();
  TestOddInteriorCountLeavesALonePage();
  TestDegenerateBookletSizes();

  TestLayoutWidthNeverDependsOnTheOpening();
  TestSinglePageOpeningsPickTheCorrectHalf();
  TestBackwardTurnDrivenThroughTheRealApi();
  TestForwardTurnLeavesTheLeftPageAlone();
  TestBackwardTurnLeavesTheRightPageAlone();
  TestLeafShowsTheSheetsOwnTwoPages();
  TestLeafMirrorIsDirectionXorFace();
  TestSinglePageModeResolves();
  TestTurnsAtTheCoversResolve();

  TestLeafNeverGoesBehindASettledPage();
  TestDepthRisesWithUAtEveryPhase();
  TestBowKeepsDepthMonotonicInU();
  TestBowVanishesAtTheEndsAndEdges();
  TestBowFollowsTheSurfaceNormal();
  TestBowedSheetStillMatchesThePageAtRest();
  TestHingeSweepsAHalfTurnAndIsMonotonic();
  TestBackwardTurnMirrorsTheHinge();
  TestTurnPivotsOnTheGutter();
  TestSheetExtentsReproduceTheRequestedSize();
  TestFaceFlipsAtTheHalfway();
  TestShadeIsBoundedAndDimmestEdgeOn();

  if (g_failures) {
    printf("manual_pages_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("manual_pages_test: all checks passed\n");
  return 0;
}
