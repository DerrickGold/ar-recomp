#include "presentation_frame_generation.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
  kMotionSampleStep = 4,
  kGlobalMotionCoarseSampleStep = 16,
  kGlobalMotionRefineSampleStep = 8,
  kMotionDistancePenalty = 12,
  kMotionMinimumImprovementPercent = 12,
  kMotionMinimumImprovementCost = 64,
  kMotionInverseTolerance = 1,
};

typedef struct MotionVector {
  int dx;
  int dy;
} MotionVector;

typedef struct MotionSearchResult {
  MotionVector motion;
  bool reliable;
} MotionSearchResult;

static int AbsInt(int value) {
  return value < 0 ? -value : value;
}

static int ClampInt(int value, int low, int high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

static unsigned ChannelDifference(uint32_t a, uint32_t b, int shift) {
  return (unsigned)AbsInt(
      (int)((a >> shift) & 0xffu) - (int)((b >> shift) & 0xffu));
}

static unsigned PixelDifference(uint32_t a, uint32_t b) {
  if (a == b) return 0;
  return ChannelDifference(a, b, 0) +
      ChannelDifference(a, b, 8) +
      ChannelDifference(a, b, 16) +
      ChannelDifference(a, b, 24) * 2u;
}

static bool BlockHasSignal(const uint32_t *pixels, int pitch,
                           int x0, int y0, int width, int height) {
  const int x1 = ClampInt(
      x0 + kPresentationFrameGenerationBlockSize, 0, width);
  const int y1 = ClampInt(
      y0 + kPresentationFrameGenerationBlockSize, 0, height);
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      if (pixels[(size_t)y * pitch + x] != 0) return true;
    }
  }
  return false;
}

static unsigned BlockCost(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch,
    int width, int height, int x0, int y0, int dx, int dy) {
  const int x1 = ClampInt(
      x0 + kPresentationFrameGenerationBlockSize, 0, width);
  const int y1 = ClampInt(
      y0 + kPresentationFrameGenerationBlockSize, 0, height);
  unsigned cost = (unsigned)(AbsInt(dx) + AbsInt(dy)) *
      kMotionDistancePenalty;
  for (int y = y0; y < y1; y += kMotionSampleStep) {
    const int target_y = y + dy;
    for (int x = x0; x < x1; x += kMotionSampleStep) {
      const int target_x = x + dx;
      const uint32_t source_pixel =
          source[(size_t)y * source_pitch + x];
      /* Captured OBJ planes are transparent outside their finite surface.
       * Compare against that transparent value rather than assigning a blanket
       * boundary penalty: a transparent sample near an edge carries no evidence
       * against legitimate outward sprite motion. */
      const uint32_t target_pixel =
          target_x < 0 || target_x >= width ||
          target_y < 0 || target_y >= height
              ? 0
              : target[(size_t)target_y * target_pitch + target_x];
      cost += PixelDifference(
          source_pixel, target_pixel);
    }
  }
  return cost;
}

static bool BetterMotion(unsigned cost, MotionVector candidate,
                         unsigned best_cost, MotionVector best) {
  if (cost != best_cost) return cost < best_cost;
  const int magnitude = AbsInt(candidate.dx) + AbsInt(candidate.dy);
  const int best_magnitude = AbsInt(best.dx) + AbsInt(best.dy);
  if (magnitude != best_magnitude) return magnitude < best_magnitude;
  if (candidate.dy != best.dy) return candidate.dy < best.dy;
  return candidate.dx < best.dx;
}

static bool MotionIsNonzero(MotionVector motion) {
  return motion.dx != 0 || motion.dy != 0;
}

static bool MotionHasMeaningfulImprovement(unsigned best_cost,
                                           unsigned stationary_cost) {
  if (best_cost >= stationary_cost) return false;
  const unsigned improvement = stationary_cost - best_cost;
  unsigned required = (unsigned)(
      ((uint64_t)stationary_cost * kMotionMinimumImprovementPercent + 99u) /
      100u);
  if (required < kMotionMinimumImprovementCost)
    required = kMotionMinimumImprovementCost;
  return improvement >= required;
}

static bool MotionsAreInverse(MotionVector forward, MotionVector backward) {
  return AbsInt(forward.dx + backward.dx) <= kMotionInverseTolerance &&
      AbsInt(forward.dy + backward.dy) <= kMotionInverseTolerance;
}

static MotionSearchResult FindBlockMotion(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch,
    int width, int height, int x0, int y0, MotionVector seed) {
  MotionVector best = {
    ClampInt(seed.dx, -kPresentationFrameGenerationSearchRadius,
             kPresentationFrameGenerationSearchRadius),
    ClampInt(seed.dy, -kPresentationFrameGenerationSearchRadius,
             kPresentationFrameGenerationSearchRadius),
  };
  const unsigned stationary_cost = BlockCost(
      source, target, source_pitch, target_pitch,
      width, height, x0, y0, 0, 0);
  unsigned best_cost = BlockCost(
      source, target, source_pitch, target_pitch,
      width, height, x0, y0, best.dx, best.dy);

  /* Three-step diamond/square search: 27 candidates rather than an exhaustive
   * 225, while neighbour seeding makes coherent plane motion the common case.
   * The final unit step still finds exact one-pixel motion. */
  static const int offsets[9][2] = {
    { 0, 0}, {-1,-1}, { 0,-1}, { 1,-1},
    {-1, 0}, { 1, 0}, {-1, 1}, { 0, 1}, { 1, 1},
  };
  for (int step = 4; step >= 1; step /= 2) {
    const MotionVector center = best;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
      MotionVector candidate = {
        ClampInt(center.dx + offsets[i][0] * step,
                 -kPresentationFrameGenerationSearchRadius,
                 kPresentationFrameGenerationSearchRadius),
        ClampInt(center.dy + offsets[i][1] * step,
                 -kPresentationFrameGenerationSearchRadius,
                 kPresentationFrameGenerationSearchRadius),
      };
      const unsigned cost = BlockCost(
          source, target, source_pitch, target_pitch,
          width, height, x0, y0, candidate.dx, candidate.dy);
      if (BetterMotion(cost, candidate, best_cost, best)) {
        best = candidate;
        best_cost = cost;
      }
    }
  }
  const bool reliable = MotionIsNonzero(best) &&
      MotionHasMeaningfulImprovement(best_cost, stationary_cost);
  return (MotionSearchResult){
    .motion = reliable ? best : (MotionVector){0, 0},
    .reliable = reliable,
  };
}

static void AnalyzeDirection(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch,
    int width, int height, int blocks_x, int blocks_y,
    int8_t *out_dx, int8_t *out_dy, bool *out_reliable) {
  bool signal[kPresentationFrameGenerationMaximumBlocks] = {false};
  for (int by = 0; by < blocks_y; by++) {
    for (int bx = 0; bx < blocks_x; bx++) {
      const int at = by * blocks_x + bx;
      signal[at] = BlockHasSignal(
          source, source_pitch,
          bx * kPresentationFrameGenerationBlockSize,
          by * kPresentationFrameGenerationBlockSize,
          width, height);
      if (!signal[at]) {
        out_dx[at] = 0;
        out_dy[at] = 0;
        out_reliable[at] = false;
        continue;
      }
      MotionVector seed = {0, 0};
      int contributors = 0;
      if (bx > 0) {
        seed.dx += out_dx[at - 1];
        seed.dy += out_dy[at - 1];
        contributors++;
      }
      if (by > 0) {
        seed.dx += out_dx[at - blocks_x];
        seed.dy += out_dy[at - blocks_x];
        contributors++;
      }
      if (contributors > 1) {
        seed.dx /= contributors;
        seed.dy /= contributors;
      }
      const MotionSearchResult result = FindBlockMotion(
          source, target, source_pitch, target_pitch,
          width, height,
          bx * kPresentationFrameGenerationBlockSize,
          by * kPresentationFrameGenerationBlockSize,
          seed);
      out_dx[at] = (int8_t)result.motion.dx;
      out_dy[at] = (int8_t)result.motion.dy;
      out_reliable[at] = result.reliable;
    }
  }
  /* Give each occupied block a one-block motion halo. A sparse sprite that
   * touches a block edge then remains rigid instead of being blended toward a
   * zero vector from transparent neighbours; the halo does not recursively
   * spread across the whole plane or merge distant actors. */
  static const int neighbour_x[] = {-1, 1, 0, 0};
  static const int neighbour_y[] = {0, 0, -1, 1};
  for (int by = 0; by < blocks_y; by++) {
    for (int bx = 0; bx < blocks_x; bx++) {
      const int at = by * blocks_x + bx;
      if (signal[at]) continue;
      int dx = 0, dy = 0, contributors = 0;
      for (int neighbour = 0; neighbour < 4; neighbour++) {
        const int nx = bx + neighbour_x[neighbour];
        const int ny = by + neighbour_y[neighbour];
        if (nx < 0 || nx >= blocks_x || ny < 0 || ny >= blocks_y)
          continue;
        const int source_at = ny * blocks_x + nx;
        if (!signal[source_at] || !out_reliable[source_at]) continue;
        dx += out_dx[source_at];
        dy += out_dy[source_at];
        contributors++;
      }
      if (contributors) {
        out_dx[at] = (int8_t)(dx / contributors);
        out_dy[at] = (int8_t)(dy / contributors);
        out_reliable[at] = true;
      }
    }
  }
}

static unsigned GlobalCost(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch,
    int width, int height, int dx, int dy, int sample_step) {
  unsigned cost = (unsigned)(AbsInt(dx) + AbsInt(dy)) *
      kMotionDistancePenalty;
  /* Stay one search radius inside every edge. All candidate vectors then
   * compare the same sample population, rather than allowing boundary
   * penalties to outweigh the image match and bias motion toward zero. */
  for (int y = kPresentationFrameGenerationSearchRadius;
       y < height - kPresentationFrameGenerationSearchRadius;
       y += sample_step) {
    const int target_y = y + dy;
    for (int x = kPresentationFrameGenerationSearchRadius;
         x < width - kPresentationFrameGenerationSearchRadius;
         x += sample_step) {
      const int target_x = x + dx;
      cost += PixelDifference(
          source[(size_t)y * source_pitch + x],
          target[(size_t)target_y * target_pitch + target_x]);
    }
  }
  return cost;
}

static MotionSearchResult FindGlobalMotion(
    const uint32_t *source, const uint32_t *target,
    int source_pitch, int target_pitch, int width, int height) {
  MotionVector best = {0, 0};
  unsigned best_cost = UINT_MAX;
  /* Exhaust the small 15x15 search on a coarse grid. The former three-step
   * hill climb was faster but could settle in a diagonal local minimum on
   * high-frequency pixel art and move an entire background the wrong way. */
  for (int dy = -kPresentationFrameGenerationSearchRadius;
       dy <= kPresentationFrameGenerationSearchRadius; dy++) {
    for (int dx = -kPresentationFrameGenerationSearchRadius;
         dx <= kPresentationFrameGenerationSearchRadius; dx++) {
      const MotionVector candidate = {dx, dy};
      const unsigned cost = GlobalCost(
          source, target, source_pitch, target_pitch,
          width, height, dx, dy, kGlobalMotionCoarseSampleStep);
      if (BetterMotion(cost, candidate, best_cost, best)) {
        best = candidate;
        best_cost = cost;
      }
    }
  }
  /* Re-score the coarse winner and its immediate neighbours on twice the
   * sample density. */
  const MotionVector coarse = best;
  best_cost = UINT_MAX;
  for (int oy = -1; oy <= 1; oy++) {
    for (int ox = -1; ox <= 1; ox++) {
      const MotionVector candidate = {
        ClampInt(coarse.dx + ox,
                 -kPresentationFrameGenerationSearchRadius,
                 kPresentationFrameGenerationSearchRadius),
        ClampInt(coarse.dy + oy,
                 -kPresentationFrameGenerationSearchRadius,
                 kPresentationFrameGenerationSearchRadius),
      };
      const unsigned cost = GlobalCost(
          source, target, source_pitch, target_pitch,
          width, height, candidate.dx, candidate.dy,
          kGlobalMotionRefineSampleStep);
      if (BetterMotion(cost, candidate, best_cost, best)) {
        best = candidate;
        best_cost = cost;
      }
    }
  }
  const unsigned stationary_cost = GlobalCost(
      source, target, source_pitch, target_pitch,
      width, height, 0, 0, kGlobalMotionRefineSampleStep);
  const bool reliable = MotionIsNonzero(best) &&
      MotionHasMeaningfulImprovement(best_cost, stationary_cost);
  return (MotionSearchResult){
    .motion = reliable ? best : (MotionVector){0, 0},
    .reliable = reliable,
  };
}

static void FillDirection(int blocks, MotionVector motion,
                          int8_t *out_dx, int8_t *out_dy) {
  for (int at = 0; at < blocks; at++) {
    out_dx[at] = (int8_t)motion.dx;
    out_dy[at] = (int8_t)motion.dy;
  }
}

bool PresentationFrameGeneration_Analyze(
    const uint32_t *previous, const uint32_t *current,
    int width, int height, int previous_pitch, int current_pitch,
    PresentationFrameGenerationAnalysisMode mode,
    PresentationFrameGenerationMotionField *field) {
  if (field) memset(field, 0, sizeof(*field));
  if (!previous || !current || !field || width <= 0 || height <= 0 ||
      width > kPresentationFrameGenerationMaximumWidth ||
      height > kPresentationFrameGenerationMaximumHeight ||
      previous_pitch < width || current_pitch < width)
    return false;

  field->width = width;
  field->height = height;
  field->blocks_x =
      (width + kPresentationFrameGenerationBlockSize - 1) /
      kPresentationFrameGenerationBlockSize;
  field->blocks_y =
      (height + kPresentationFrameGenerationBlockSize - 1) /
      kPresentationFrameGenerationBlockSize;
  if (mode == kPresentationFrameGenerationAnalysis_Global) {
    const MotionSearchResult forward = FindGlobalMotion(
        previous, current, previous_pitch, current_pitch, width, height);
    const MotionSearchResult backward = FindGlobalMotion(
        current, previous, current_pitch, previous_pitch, width, height);
    if (!forward.reliable || !backward.reliable ||
        !MotionsAreInverse(forward.motion, backward.motion)) {
      memset(field, 0, sizeof(*field));
      return false;
    }
    const int blocks = field->blocks_x * field->blocks_y;
    FillDirection(
        blocks, forward.motion, field->forward_dx, field->forward_dy);
    FillDirection(
        blocks, backward.motion, field->backward_dx, field->backward_dy);
    field->uniform = true;
  } else if (mode == kPresentationFrameGenerationAnalysis_Blocks) {
    bool forward_reliable[kPresentationFrameGenerationMaximumBlocks] = {false};
    bool backward_reliable[kPresentationFrameGenerationMaximumBlocks] = {false};
    AnalyzeDirection(
        previous, current, previous_pitch, current_pitch,
        width, height, field->blocks_x, field->blocks_y,
        field->forward_dx, field->forward_dy, forward_reliable);
    AnalyzeDirection(
        current, previous, current_pitch, previous_pitch,
        width, height, field->blocks_x, field->blocks_y,
        field->backward_dx, field->backward_dy, backward_reliable);
    bool any_reliable_motion = false;
    const int blocks = field->blocks_x * field->blocks_y;
    for (int at = 0; at < blocks; at++) {
      const MotionVector forward = {
        field->forward_dx[at], field->forward_dy[at]
      };
      const MotionVector backward = {
        field->backward_dx[at], field->backward_dy[at]
      };
      if (!forward_reliable[at] || !backward_reliable[at] ||
          !MotionIsNonzero(forward) || !MotionIsNonzero(backward) ||
          !MotionsAreInverse(forward, backward)) {
        field->forward_dx[at] = 0;
        field->forward_dy[at] = 0;
        field->backward_dx[at] = 0;
        field->backward_dy[at] = 0;
        continue;
      }
      any_reliable_motion = true;
    }
    if (!any_reliable_motion) {
      memset(field, 0, sizeof(*field));
      return false;
    }
    field->uniform = false;
  } else {
    memset(field, 0, sizeof(*field));
    return false;
  }
  field->valid = true;
  return true;
}

static int InterpolatedMotion(const int8_t *motion, int blocks_x, int blocks_y,
                              int x, int y) {
  /* Each estimate belongs to its block centre. Clamp the half-block border to
   * the edge vector, then interpolate between neighbouring centres. */
  const int half_block = kPresentationFrameGenerationBlockSize / 2;
  int field_x = x - half_block;
  int field_y = y - half_block;
  if (field_x < 0) field_x = 0;
  if (field_y < 0) field_y = 0;
  const int bx = ClampInt(
      field_x / kPresentationFrameGenerationBlockSize, 0, blocks_x - 1);
  const int by = ClampInt(
      field_y / kPresentationFrameGenerationBlockSize, 0, blocks_y - 1);
  const int bx1 = bx + 1 < blocks_x ? bx + 1 : bx;
  const int by1 = by + 1 < blocks_y ? by + 1 : by;
  const int fx = bx1 == bx ? 0
      : field_x % kPresentationFrameGenerationBlockSize;
  const int fy = by1 == by ? 0
      : field_y % kPresentationFrameGenerationBlockSize;
  const int ix = kPresentationFrameGenerationBlockSize - fx;
  const int iy = kPresentationFrameGenerationBlockSize - fy;
  const int top = motion[by * blocks_x + bx] * ix +
      motion[by * blocks_x + bx1] * fx;
  const int bottom = motion[by1 * blocks_x + bx] * ix +
      motion[by1 * blocks_x + bx1] * fx;
  return top * iy + bottom * fy;  /* vector * block_size^2 */
}

void PresentationFrameGeneration_MotionAt(
    const PresentationFrameGenerationMotionField *field,
    bool forward, int x, int y, float *out_dx, float *out_dy) {
  if (out_dx) *out_dx = 0.0f;
  if (out_dy) *out_dy = 0.0f;
  if (!field || !field->valid || field->blocks_x <= 0 ||
      field->blocks_y <= 0)
    return;
  x = ClampInt(x, 0, field->width - 1);
  y = ClampInt(y, 0, field->height - 1);
  const int scale = kPresentationFrameGenerationBlockSize *
      kPresentationFrameGenerationBlockSize;
  const int8_t *dx = forward ? field->forward_dx : field->backward_dx;
  const int8_t *dy = forward ? field->forward_dy : field->backward_dy;
  if (out_dx)
    *out_dx = (float)InterpolatedMotion(
        dx, field->blocks_x, field->blocks_y, x, y) / (float)scale;
  if (out_dy)
    *out_dy = (float)InterpolatedMotion(
        dy, field->blocks_x, field->blocks_y, x, y) / (float)scale;
}

float PresentationFrameGeneration_PairPhase(float alpha,
                                             uint8_t capture_ticks) {
  if (!capture_ticks) return 1.0f;
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;
  return ((float)capture_ticks - 1.0f + alpha) /
      (float)capture_ticks;
}
