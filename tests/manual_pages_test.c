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
  CHECK(view.page == 1);

  ManualView_GoTo(&view, 39, 40);
  CHECK(!ManualView_BeginTurn(&view, +1, 40));   /* past the last page */
  CHECK(view.page == 39);
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
  CHECK(view.page == 1);
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
  CHECK(view.page == 1);
  CHECK(view.turn == 0.0f);
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
        /* And it stays within the unit sheet, so it cannot swing off-screen. */
        CHECK(fabsf(x) <= 1.001f);
        CHECK(z <= 1.001f);
      }
    }
  }
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

/* The hinge is on opposite edges for the two directions, or a backward turn
 * would animate as though it were going forward. */
static void TestBackwardTurnMirrorsTheHinge(void) {
  float fx = 0, fy = 0, fz = 0, bx = 0, by = 0, bz = 0;
  ManualTurn_LeafPoint(0.5f, 1.0f, 0.5f, &fx, &fy, &fz);
  ManualTurn_LeafPoint(-0.5f, 1.0f, 0.5f, &bx, &by, &bz);
  CHECK(fx * bx <= 0.0f);            /* opposite sides (or both at the spine) */
  CHECK(fabsf(fx + bx) < 1e-5f);     /* exact mirror */
  CHECK(fabsf(fz - bz) < 1e-5f);     /* same lift */
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
  TestMixedGeometryIsNotAnAlbum();
  TestVectorDocumentYieldsNothing();
  TestThumbnailEoiDoesNotTruncateAPage();
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

  TestLeafNeverGoesBehindASettledPage();
  TestHingeSweepsAHalfTurnAndIsMonotonic();
  TestBackwardTurnMirrorsTheHinge();
  TestFaceFlipsAtTheHalfway();
  TestShadeIsBoundedAndDimmestEdgeOn();

  if (g_failures) {
    printf("manual_pages_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("manual_pages_test: all checks passed\n");
  return 0;
}
