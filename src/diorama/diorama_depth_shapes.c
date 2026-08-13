#include "diorama_depth_shapes.h"

#include <math.h>    /* fabsf: symmetric fade for a centred stack */

/* ── thickness: the extruded near face ────────────────────────────────── */

/* How dark the near lip gets. 1.0 would make the fold invisible; too low reads
 * as a black band rather than the same material turned away from the light. */
static const float kSkirtNearShade = 0.55f;

/* How far the near lip DROPS in world Y, as a fraction of how far it comes forward
 * in Z. This is a look decision, not geometry: at 1.0 the face is a 45-degree ramp
 * and a thick layer reads as a tall wall; at 0 it is horizontal and reads as a
 * shelf. Half keeps a block's proportions plausible. Named because someone tuning
 * how a thickness reads would otherwise never find it inside the vertex formula. */
static const float kSkirtDropPerDepth = 0.5f;

float DioramaSkirtNearShade(void) { return kSkirtNearShade; }

void DioramaSkirtVertex(float t, float z_bottom, float y_bottom,
                        float thickness, float *out_y, float *out_z,
                        float *out_shade) {
  /* The face drops as it comes forward, so a thick layer reads as a block rather
   * than a tall wall -- see kSkirtDropPerDepth. */
  *out_y = y_bottom - t * thickness * kSkirtDropPerDepth;
  *out_z = z_bottom + t * thickness;
  *out_shade = 1.0f - (1.0f - kSkirtNearShade) * t;
}

bool DioramaVerticalRepeatPlan_Build(
    int authentic_y0, int authentic_height,
    int capture_height, int texture_height,
    DioramaVerticalRepeatPlan *out) {
  if (out) *out = (DioramaVerticalRepeatPlan){0};
  if (!out || authentic_y0 < 0 || authentic_height <= 0 ||
      capture_height <= 0 || texture_height <= 0 ||
      capture_height > texture_height ||
      authentic_y0 > capture_height - authentic_height)
    return false;
  *out = (DioramaVerticalRepeatPlan) {
    .source_y0 = authentic_y0,
    .source_y1 = authentic_y0 + authentic_height,
    .fold_y = authentic_y0 + authentic_height,
    .repeat_height = authentic_height,
  };
  return true;
}

void DioramaOverflowFoldPoint(
    float t, float y_top, float z_top, float z_handoff,
    float overflow_height, float overlap_t,
    float front_z, float front_drop,
    float *out_y, float *out_z) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  if (overlap_t < 0.0f) overlap_t = 0.0f;
  if (overlap_t > 1.0f) overlap_t = 1.0f;

  const float linear_y = y_top - overflow_height * t;
  if (overlap_t >= 1.0f || t <= overlap_t) {
    const float q = overlap_t > 0.0f ? t / overlap_t : 0.0f;
    *out_y = linear_y;
    *out_z = z_top + (z_handoff - z_top) * q;
    return;
  }

  const float u = (t - overlap_t) / (1.0f - overlap_t);
  const float bend = u * u * (3.0f - 2.0f * u);
  const float handoff_y = y_top - overflow_height * overlap_t;
  const float front_y = handoff_y - front_drop * u;
  *out_y = linear_y + (front_y - linear_y) * bend;
  *out_z = z_handoff + (front_z - z_handoff) * bend;
}

float DioramaOverflowFoldRowT(int row, int subdivisions, float overlap_t) {
  if (subdivisions <= 0 || row <= 0) return 0.0f;
  if (row >= subdivisions) return 1.0f;
  if (overlap_t <= 0.0f || overlap_t >= 1.0f || subdivisions < 2)
    return (float)row / (float)subdivisions;
  if (row == 1) return overlap_t;
  return overlap_t +
      (float)(row - 1) / (float)(subdivisions - 1) * (1.0f - overlap_t);
}

/* ── stack: fill a depth gap with parallel copies ─────────────────────── */

/* Falloff at the FARTHEST copy. Both are multipliers on the layer's own values,
 * so a stacked translucent layer stays translucent. Not 0: a fully transparent
 * copy is a draw call that paints nothing. */
static const float kStackFarShade = 0.50f;
static const float kStackFarAlpha = 0.35f;

/* Mirrors DioramaStackDirection (diorama_layer_order.h). Kept as local constants
 * so this pure module does not depend on the override module's header; the test
 * asserts the two agree. */
enum { kStackDirForward = 0, kStackDirBackward = 1, kStackDirBoth = 2 };

static void ResolveStackCopy(int index, int copies, float z_base, float depth,
                             int direction, float *out_z, float *out_shade,
                             float *out_alpha) {
  const float f = (float)index / (float)(copies - 1);

  float z = z_base, distance = f;
  switch (direction) {
    case kStackDirBackward:
      z = z_base - f * depth;
      break;
    case kStackDirBoth: {
      /* f runs 0..1 across the centred fill. Fold distance about its midpoint
       * so both edges receive the same full falloff. */
      const float kMidpoint = 0.5f;
      z = z_base + (f - kMidpoint) * depth;
      distance = fabsf(f - kMidpoint) / kMidpoint;
      break;
    }
    case kStackDirForward:
    default:
      z = z_base + f * depth;
      break;
  }
  *out_z = z;
  *out_shade = 1.0f - (1.0f - kStackFarShade) * distance;
  *out_alpha = 1.0f - (1.0f - kStackFarAlpha) * distance;
}

bool DioramaStackCopyIsRedundant(int index, int copies, int direction) {
  if (direction == kStackDirBoth) {
    /* Offsets are (i/(copies-1) - 0.5)*depth, so a copy lands exactly on the
     * plane only when copies is odd, at the midpoint index. */
    if ((copies % 2) == 0) return false;
    return index == (copies - 1) / 2;
  }
  /* Forward and backward both start at the plane. */
  return index == 0;
}

/* Uniform tint on a voxel's copies. Not a depth falloff -- see the header: a solid
 * object's back half must not read as fog. Just enough that an extruded layer is
 * distinguishable from the flat one it replaces. */
static const float kVoxelShade = 0.88f;

void DioramaStackCopyShaped(int index, int copies, float z_base, float depth,
                            int direction, bool solid, float *out_z,
                            float *out_shade, float *out_alpha) {
  float z = z_base, shade = 1.0f, alpha = 1.0f;
  ResolveStackCopy(index, copies, z_base, depth, direction, &z, &shade, &alpha);
  if (solid) {
    /* Depth is shared with the stack; only the falloff differs. */
    shade = kVoxelShade;
    alpha = 1.0f;
  }
  *out_z = z;
  *out_shade = shade;
  *out_alpha = alpha;
}

/* ── tilt: linear rake and eased bow ──────────────────────────────────── */

float DioramaTiltedRowDepth(float z_world, float rake, float bow, float t) {
  /* Early-out for the no-tilt case, which is nearly every layer in nearly every
   * room. Note this is NOT what makes an unauthored layer bit-identical: for any
   * z a layer actually uses, `z + 0*t + 0*t*t` already equals z exactly (the only
   * float where it would not is -0.0). It is here because it is the common path
   * and skipping two multiplies and two adds per vertex is free, and because it
   * documents that no-tilt is exactly no-op. A mutation removing it therefore
   * cannot be caught by a test, which is why there is no test asserting it. */
  if (rake == 0.0f && bow == 0.0f) return z_world;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return z_world + rake * t + bow * t * t;
}
