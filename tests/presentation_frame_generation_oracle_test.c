/* Bit-identity oracle for the global-motion search's loop interchange.
 *
 * The search still visits the same 15x15 candidate set; only the order in
 * which samples and candidates are visited changed, so that each source pixel
 * is read once instead of the whole surface being streamed once per candidate.
 * That reordering is only safe if it cannot alter a single chosen vector.
 *
 * This test re-implements the ORIGINAL candidate-outer order independently and
 * requires the shipping analyzer to agree exactly, over image pairs chosen to
 * stress the parts most likely to diverge: exact ties, wrap-around magnitudes,
 * high-frequency pixel art (which is why the exhaustive search exists at all),
 * and motion at the edge of the search radius.
 *
 * It compares the public Analyze output rather than the internal cost array:
 * agreement on the vector is the property that matters, and it keeps the test
 * independent of the search's internals. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "presentation_frame_generation.h"

static int failures;
static int comparisons;   /* pairs where Analyze accepted and a vector was checked */
#define CHECK(expression) do {                                            \
  if (!(expression)) {                                                    \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);      \
    failures++;                                                           \
  }                                                                       \
} while (0)

enum {
  kRadius = kPresentationFrameGenerationSearchRadius,
  kCoarseStep = 16,
  kRefineStep = 8,
  kDistancePenalty = 12,
  kMinimumImprovementPercent = 12,
  kMinimumImprovementCost = 64,
  kInverseTolerance = 1,
};

/* ------------------------------------------------------ reference search -- */
/* Deliberately a transcription of the ORIGINAL shape: one full pass over the
 * surface per candidate. Slow by design; it exists to be obviously correct. */

static int AbsInt(int value) { return value < 0 ? -value : value; }

static int ClampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

static unsigned ChannelDifference(uint32_t a, uint32_t b, int shift) {
  return (unsigned)AbsInt(
      (int)((a >> shift) & 0xffu) - (int)((b >> shift) & 0xffu));
}

static unsigned PixelDifference(uint32_t a, uint32_t b) {
  if (a == b) return 0;
  return ChannelDifference(a, b, 0) + ChannelDifference(a, b, 8) +
      ChannelDifference(a, b, 16) + ChannelDifference(a, b, 24) * 2u;
}

static unsigned ReferenceCost(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch,
    int width, int height, int dx, int dy, int sample_step) {
  unsigned cost = (unsigned)(AbsInt(dx) + AbsInt(dy)) * kDistancePenalty;
  for (int y = kRadius; y < height - kRadius; y += sample_step) {
    for (int x = kRadius; x < width - kRadius; x += sample_step) {
      cost += PixelDifference(
          source[(size_t)y * source_pitch + x],
          target[(size_t)(y + dy) * target_pitch + (x + dx)]);
    }
  }
  return cost;
}

static bool BetterMotion(unsigned cost, int dx, int dy,
                         unsigned best_cost, int best_dx, int best_dy) {
  if (cost != best_cost) return cost < best_cost;
  const int magnitude = AbsInt(dx) + AbsInt(dy);
  const int best_magnitude = AbsInt(best_dx) + AbsInt(best_dy);
  if (magnitude != best_magnitude) return magnitude < best_magnitude;
  if (dy != best_dy) return dy < best_dy;
  return dx < best_dx;
}

/* Returns whether a reliable vector was found, writing it to out_dx/out_dy. */
static bool ReferenceGlobalMotion(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch, int width, int height,
    int *out_dx, int *out_dy) {
  int best_dx = 0, best_dy = 0;
  unsigned best_cost = UINT_MAX;
  for (int dy = -kRadius; dy <= kRadius; dy++) {
    for (int dx = -kRadius; dx <= kRadius; dx++) {
      const unsigned cost = ReferenceCost(
          source, target, source_pitch, target_pitch,
          width, height, dx, dy, kCoarseStep);
      if (BetterMotion(cost, dx, dy, best_cost, best_dx, best_dy)) {
        best_dx = dx; best_dy = dy; best_cost = cost;
      }
    }
  }
  const int coarse_dx = best_dx, coarse_dy = best_dy;
  best_cost = UINT_MAX;
  for (int oy = -1; oy <= 1; oy++) {
    for (int ox = -1; ox <= 1; ox++) {
      const int dx = ClampInt(coarse_dx + ox, -kRadius, kRadius);
      const int dy = ClampInt(coarse_dy + oy, -kRadius, kRadius);
      const unsigned cost = ReferenceCost(
          source, target, source_pitch, target_pitch,
          width, height, dx, dy, kRefineStep);
      if (BetterMotion(cost, dx, dy, best_cost, best_dx, best_dy)) {
        best_dx = dx; best_dy = dy; best_cost = cost;
      }
    }
  }
  const unsigned stationary = ReferenceCost(
      source, target, source_pitch, target_pitch,
      width, height, 0, 0, kRefineStep);
  const bool nonzero = best_dx != 0 || best_dy != 0;
  const bool improved =
      best_cost + kMinimumImprovementCost <= stationary &&
      (uint64_t)best_cost * 100u <=
          (uint64_t)stationary * (100u - kMinimumImprovementPercent);
  *out_dx = (nonzero && improved) ? best_dx : 0;
  *out_dy = (nonzero && improved) ? best_dy : 0;
  return nonzero && improved;
}

/* ------------------------------------------------------------- fixtures -- */

static uint32_t NextRandom(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (*state = x);
}

/* Shifts `previous` by (dx,dy) into `current`, filling exposed edges with a
 * deterministic pattern so the two frames are never trivially identical. */
static void ShiftFrame(const uint32_t *previous, uint32_t *current,
                       int width, int height, int pitch, int dx, int dy,
                       uint32_t *seed) {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const int sx = x - dx, sy = y - dy;
      current[(size_t)y * pitch + x] =
          (sx >= 0 && sx < width && sy >= 0 && sy < height)
              ? previous[(size_t)sy * pitch + sx]
              : (0xff000000u | (NextRandom(seed) & 0x00ffffffu));
    }
  }
}

/* Reproduces Analyze's ENTIRE Global-mode decision, not just the forward
 * search, so the assertion can be exact equality in every case. An earlier
 * version only checked the vector when Analyze accepted the pair, which let a
 * deliberate perturbation hide by causing rejections instead of wrong answers
 * -- the failure mode a permissive oracle is most likely to have. */
static void CompareOne(const char *label, const uint32_t *previous,
                       const uint32_t *current, int width, int height,
                       int pitch) {
  int forward_dx = 0, forward_dy = 0, backward_dx = 0, backward_dy = 0;
  const bool forward_reliable = ReferenceGlobalMotion(
      previous, current, pitch, pitch, width, height,
      &forward_dx, &forward_dy);
  const bool backward_reliable = ReferenceGlobalMotion(
      current, previous, pitch, pitch, width, height,
      &backward_dx, &backward_dy);
  const bool inverse =
      AbsInt(forward_dx + backward_dx) <= kInverseTolerance &&
      AbsInt(forward_dy + backward_dy) <= kInverseTolerance;
  const bool expect_accept = forward_reliable && backward_reliable && inverse;

  PresentationFrameGenerationMotionField field;
  const bool analyzed = PresentationFrameGeneration_Analyze(
      previous, current, width, height, pitch, pitch,
      kPresentationFrameGenerationAnalysis_Global, &field);

  if (analyzed != expect_accept) {
    fprintf(stderr, "%s: accepted=%d but reference expected %d\n",
            label, (int)analyzed, (int)expect_accept);
    failures++;
    return;
  }
  if (!analyzed) return;
  comparisons++;
  if (field.forward_dx[0] != forward_dx || field.forward_dy[0] != forward_dy ||
      field.backward_dx[0] != backward_dx ||
      field.backward_dy[0] != backward_dy) {
    fprintf(stderr,
            "%s: forward (%d,%d)/backward (%d,%d) != reference "
            "(%d,%d)/(%d,%d)\n",
            label, field.forward_dx[0], field.forward_dy[0],
            field.backward_dx[0], field.backward_dy[0],
            forward_dx, forward_dy, backward_dx, backward_dy);
    failures++;
  }
}

enum { kWidth = 96, kHeight = 64, kPitch = 128 };

static void TestShiftedPatterns(void) {
  static uint32_t previous[kPitch * kHeight];
  static uint32_t current[kPitch * kHeight];
  /* Sweep the whole candidate set, including the radius edges and zero. */
  const int offsets[] = {0, 1, -1, 3, -5, 7, -7, 6, -2};
  for (size_t pattern = 0; pattern < 3; pattern++) {
    uint32_t seed = 0x1234567u + (uint32_t)pattern * 7919u;
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        uint32_t value;
        if (pattern == 0) {            /* high-frequency pixel art */
          value = 0xff000000u | (uint32_t)(((x * 37) ^ (y * 91)) & 0xffffffu);
        } else if (pattern == 1) {     /* large flat regions, many ties */
          value = 0xff000000u | (uint32_t)(((x / 16) * 0x3070u) +
                                           (uint32_t)((y / 16) * 0x11u));
        } else {                       /* noise */
          value = 0xff000000u | (NextRandom(&seed) & 0x00ffffffu);
        }
        previous[(size_t)y * kPitch + x] = value;
      }
    }
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      for (size_t j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++) {
        uint32_t edge_seed = 0xabcdefu + (uint32_t)(i * 31 + j);
        ShiftFrame(previous, current, kWidth, kHeight, kPitch,
                   offsets[i], offsets[j], &edge_seed);
        char label[96];
        snprintf(label, sizeof(label), "pattern %zu shift (%d,%d)",
                 pattern, offsets[i], offsets[j]);
        CompareOne(label, previous, current, kWidth, kHeight, kPitch);
      }
    }
  }
}

static void TestRandomPairs(void) {
  static uint32_t previous[kPitch * kHeight];
  static uint32_t current[kPitch * kHeight];
  uint32_t seed = 0xC0FFEEu;
  for (int trial = 0; trial < 40; trial++) {
    for (int y = 0; y < kHeight; y++) {
      for (int x = 0; x < kWidth; x++) {
        /* Quantised noise creates genuine cost ties, which is where an
         * order-dependent tie-break would show up. */
        previous[(size_t)y * kPitch + x] =
            0xff000000u | (NextRandom(&seed) & 0x00f0f0f0u);
        current[(size_t)y * kPitch + x] =
            0xff000000u | (NextRandom(&seed) & 0x00f0f0f0u);
      }
    }
    char label[64];
    snprintf(label, sizeof(label), "random pair %d", trial);
    CompareOne(label, previous, current, kWidth, kHeight, kPitch);
  }
}

static void TestDegenerateExtents(void) {
  /* Narrower than the search radius leaves the sample loops empty, so every
   * candidate keeps only its distance penalty and (0,0) must win. */
  static uint32_t previous[kPitch * kHeight];
  static uint32_t current[kPitch * kHeight];
  uint32_t seed = 0x5EEDu;
  for (int i = 0; i < kPitch * kHeight; i++) {
    previous[i] = NextRandom(&seed);
    current[i] = NextRandom(&seed);
  }
  const int extents[][2] = {{1, 1}, {8, 8}, {15, 15}, {16, 15}, {17, 20}};
  for (size_t i = 0; i < sizeof(extents) / sizeof(extents[0]); i++) {
    char label[64];
    snprintf(label, sizeof(label), "extent %dx%d",
             extents[i][0], extents[i][1]);
    CompareOne(label, previous, current, extents[i][0], extents[i][1], kPitch);
  }
}

int main(void) {
  TestShiftedPatterns();
  TestRandomPairs();
  TestDegenerateExtents();
  /* A silently vacuous oracle is worse than none: if Analyze rejected every
   * pair, nothing was ever compared and the test proves nothing. */
  if (comparisons < 20) {
    fprintf(stderr,
            "presentation frame generation oracle: only %d comparisons; "
            "fixtures no longer exercise the search\n", comparisons);
    return 1;
  }
  if (failures) {
    fprintf(stderr, "presentation frame generation oracle: %d failure(s)\n",
            failures);
    return 1;
  }
  printf("presentation frame generation oracle: ok (%d vectors compared)\n",
         comparisons);
  return 0;
}
