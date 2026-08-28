/* Geometry of the framebuffer margin-gap fill.
 *
 * Every case is checked against a SENTINEL-filled buffer, so a test can tell
 * "wrote the fill colour", "left the pixel alone", and "wrote something else"
 * apart — a memset-based check could not distinguish the last two.
 */
#include "actraiser_ws_gap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kSentinel 0xDEADBEEFu
#define kFill     0xFF1030A0u

enum { kBudget = 120, kAuthentic = 256, kWidth = kAuthentic + 2 * kBudget,
       kHeight = 8 };

static int g_failures;

static uint32_t g_buffer[kHeight][kWidth];

static void Reset(void) {
  for (int y = 0; y < kHeight; y++)
    for (int x = 0; x < kWidth; x++)
      g_buffer[y][x] = kSentinel;
}

/* Assert every column in [x0,x1) equals `want`, on every row. */
static void ExpectSpan(const char *label, int x0, int x1, uint32_t want) {
  for (int y = 0; y < kHeight; y++) {
    for (int x = x0; x < x1; x++) {
      if (g_buffer[y][x] != want) {
        printf("FAIL %s: [%d][%d] = 0x%08X, want 0x%08X\n",
               label, y, x, g_buffer[y][x], want);
        g_failures++;
        return;
      }
    }
  }
}

static void Fill(int live_left, int live_right, uint32_t fill) {
  ActRaiserFillMarginGaps((uint8_t *)g_buffer, sizeof(g_buffer[0]), kHeight,
                          kBudget, live_left, live_right, fill);
}

/* At a level's START the left margin has collapsed: the left gap must be
 * filled and nothing else touched. */
static void TestLeftGapAtLevelStart(void) {
  Reset();
  Fill(0, kBudget, kFill);
  ExpectSpan("left-gap filled", 0, kBudget, kFill);
  ExpectSpan("rest untouched", kBudget, kWidth, kSentinel);
}

/* At a level's END the right margin has collapsed. */
static void TestRightGapAtLevelEnd(void) {
  Reset();
  Fill(kBudget, 0, kFill);
  ExpectSpan("left untouched", 0, kBudget + kAuthentic, kSentinel);
  ExpectSpan("right-gap filled", kBudget + kAuthentic, kWidth, kFill);
}

/* The PARTIAL case: as the camera pulls away from a bound the margin ramps 1px
 * per camera pixel, so for most of that travel the live margin is neither 0 nor
 * the full budget. This is the only case that pins the right gap's START column
 * (budget + 256 + live_right) rather than just its width -- with live_right == 0
 * the offset is unobservable. */
static void TestPartialMarginRamp(void) {
  const int live = 40;
  Reset();
  Fill(live, live, kFill);
  /* Left gap: columns [0, budget - live). */
  ExpectSpan("partial left filled", 0, kBudget - live, kFill);
  /* The live window and the authentic centre must survive untouched. */
  ExpectSpan("partial live-left untouched", kBudget - live,
             kBudget + kAuthentic + live, kSentinel);
  /* Right gap starts after the live window's last column, not after the
   * authentic 256. */
  ExpectSpan("partial right filled", kBudget + kAuthentic + live, kWidth,
             kFill);
}

/* Asymmetric partial margins -- the two sides collapse independently, so a fix
 * that computed one offset from the other side's margin would pass every
 * symmetric case. */
static void TestAsymmetricPartialMargins(void) {
  Reset();
  Fill(10, 70, kFill);
  ExpectSpan("asym left filled", 0, kBudget - 10, kFill);
  ExpectSpan("asym middle untouched", kBudget - 10,
             kBudget + kAuthentic + 70, kSentinel);
  ExpectSpan("asym right filled", kBudget + kAuthentic + 70, kWidth, kFill);
}

/* Mid-level both margins are full, so there is no gap at all. Writing anything
 * here would paint over real rendered pixels. */
static void TestNoGapMidLevel(void) {
  Reset();
  Fill(kBudget, kBudget, kFill);
  ExpectSpan("nothing written", 0, kWidth, kSentinel);
}

/* A fully bounded screen (PpuSetExtraSpaceCentered) renders only the authentic
 * 256, so BOTH gaps exist and the centre must survive. */
static void TestBothGapsWhenFullyBounded(void) {
  Reset();
  Fill(0, 0, kFill);
  ExpectSpan("left filled", 0, kBudget, kFill);
  ExpectSpan("centre untouched", kBudget, kBudget + kAuthentic, kSentinel);
  ExpectSpan("right filled", kBudget + kAuthentic, kWidth, kFill);
}

/* fill_argb 0 must reproduce the previous memset-to-black behaviour exactly.
 * This is what makes the A/B toggle a true comparison rather than a
 * half-migration, so it is asserted rather than assumed. */
static void TestZeroFillMatchesLegacyBlack(void) {
  Reset();
  Fill(0, 0, 0u);
  ExpectSpan("left black", 0, kBudget, 0u);
  ExpectSpan("centre untouched", kBudget, kBudget + kAuthentic, kSentinel);
  ExpectSpan("right black", kBudget + kAuthentic, kWidth, 0u);
}

/* Nonsense inputs must write nothing rather than clamp: a caller that computed
 * a margin wider than the budget has a bug, and silently painting over real
 * pixels would hide it. Also proves there is no out-of-bounds store. */
static void TestOutOfRangeInputsWriteNothing(void) {
  const struct { int left, right; const char *label; } cases[] = {
    { 200, 0, "left > budget" },
    { 0, 200, "right > budget" },
    { -1, 0, "negative left" },
    { 0, -1, "negative right" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    Reset();
    Fill(cases[i].left, cases[i].right, kFill);
    ExpectSpan(cases[i].label, 0, kWidth, kSentinel);
  }
}

/* Degenerate geometry: a zero budget means there is no margin machinery at all
 * (4:3 / g_ws_extra == 0), so there is nothing to fill. */
static void TestZeroBudgetWritesNothing(void) {
  Reset();
  ActRaiserFillMarginGaps((uint8_t *)g_buffer, sizeof(g_buffer[0]), kHeight,
                          0, 0, 0, kFill);
  ExpectSpan("zero budget", 0, kWidth, kSentinel);
}

/* A NULL buffer or non-positive height must be a no-op, not a crash. */
static void TestNullAndEmptyAreSafe(void) {
  ActRaiserFillMarginGaps(NULL, sizeof(g_buffer[0]), kHeight,
                          kBudget, 0, 0, kFill);
  Reset();
  ActRaiserFillMarginGaps((uint8_t *)g_buffer, sizeof(g_buffer[0]), 0,
                          kBudget, 0, 0, kFill);
  ExpectSpan("zero height", 0, kWidth, kSentinel);
}

int main(void) {
  TestLeftGapAtLevelStart();
  TestRightGapAtLevelEnd();
  TestPartialMarginRamp();
  TestAsymmetricPartialMargins();
  TestNoGapMidLevel();
  TestBothGapsWhenFullyBounded();
  TestZeroFillMatchesLegacyBlack();
  TestOutOfRangeInputsWriteNothing();
  TestZeroBudgetWritesNothing();
  TestNullAndEmptyAreSafe();
  if (g_failures) {
    printf("actraiser_ws_gap_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("actraiser_ws_gap_test: all checks passed\n");
  return 0;
}
