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
  if (!(thickness > 0.0f)) thickness = 0.0f;   /* also catches NaN */
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  /* The face drops as it comes forward, so a thick layer reads as a block rather
   * than a tall wall -- see kSkirtDropPerDepth. */
  if (out_y) *out_y = y_bottom - t * thickness * kSkirtDropPerDepth;
  if (out_z) *out_z = z_bottom + t * thickness;
  if (out_shade) *out_shade = 1.0f - (1.0f - kSkirtNearShade) * t;
}

/* ── stack: fill a depth gap with parallel copies ─────────────────────── */

/* Falloff at the FARTHEST copy. Both are multipliers on the layer's own values,
 * so a stacked translucent layer stays translucent. Not 0: a fully transparent
 * copy is a draw call that paints nothing. */
static const float kStackFarShade = 0.50f;
static const float kStackFarAlpha = 0.35f;

bool DioramaStackCopyIsVisible(int index, int copies) {
  if (copies < 1) copies = 1;
  return index >= 0 && index < copies;
}

void DioramaStackCopy(int index, int copies, float z_base, float depth,
                      float *out_z, float *out_shade, float *out_alpha) {
  if (copies < 1) copies = 1;
  if (!(depth > 0.0f)) depth = 0.0f;      /* also catches NaN */
  if (index < 0) index = 0;
  if (index > copies - 1) index = copies - 1;
  /* Fraction of the way into the gap. A single copy is entirely at z_base, which
   * keeps `copies:1` a no-op rather than a division by zero. */
  float f = (copies > 1) ? (float)index / (float)(copies - 1) : 0.0f;
  if (out_z) *out_z = z_base + f * depth;
  if (out_shade) *out_shade = 1.0f - (1.0f - kStackFarShade) * f;
  if (out_alpha) *out_alpha = 1.0f - (1.0f - kStackFarAlpha) * f;
}

/* Mirrors DioramaStackDirection (diorama_layer_order.h). Kept as local constants
 * so this pure module does not depend on the override module's header; the test
 * asserts the two agree. */
enum { kStackDirForward = 0, kStackDirBackward = 1, kStackDirBoth = 2 };

void DioramaStackCopyDirected(int index, int copies, float z_base, float depth,
                              int direction, float *out_z, float *out_shade,
                              float *out_alpha) {
  if (copies < 1) copies = 1;
  if (!(depth > 0.0f)) depth = 0.0f;
  if (index < 0) index = 0;
  if (index > copies - 1) index = copies - 1;
  float f = (copies > 1) ? (float)index / (float)(copies - 1) : 0.0f;

  float z = z_base, distance = f;
  switch (direction) {
    case kStackDirBackward:
      z = z_base - f * depth;
      break;
    case kStackDirBoth:
      /* Centred on the plane: index 0 is the far edge, the last is the near edge,
       * and for an odd count the middle copy coincides with the plane itself.
       * Distance-from-plane is a fold, so the fade is symmetric about the centre.
       *
       * A single copy is a special case: there is no span to centre, so it sits ON
       * the plane rather than at (f-0.5) == -0.5, which would put the one copy of
       * a degenerate stack at the far edge with nothing at the plane at all. */
      if (copies > 1) {
        /* Not tuning values: f runs 0..1 across the fill, so subtracting the
         * midpoint re-centres it to -1/2..+1/2, and doubling the absolute value
         * rescales distance-from-centre back to 0..1 so the fade reaches its full
         * range at each edge rather than half of it. Both follow from `f` being
         * normalised, so they change only if that does. */
        const float kMidpoint = 0.5f;
        z = z_base + (f - kMidpoint) * depth;
        distance = fabsf(f - kMidpoint) / kMidpoint;
      }
      break;
    case kStackDirForward:
    default:
      z = z_base + f * depth;
      break;
  }
  if (out_z) *out_z = z;
  if (out_shade) *out_shade = 1.0f - (1.0f - kStackFarShade) * distance;
  if (out_alpha) *out_alpha = 1.0f - (1.0f - kStackFarAlpha) * distance;
}

bool DioramaStackCopyIsRedundant(int index, int copies, int direction) {
  if (copies < 1) copies = 1;
  if (index < 0 || index >= copies) return false;
  if (direction == kStackDirBoth) {
    /* Offsets are (i/(copies-1) - 0.5)*depth, so a copy lands exactly on the
     * plane only when copies is odd, at the midpoint index. */
    if (copies < 2 || (copies % 2) == 0) return false;
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
  DioramaStackCopyDirected(index, copies, z_base, depth, direction, &z, &shade,
                           &alpha);
  if (solid) {
    /* Depth is shared with the stack; only the falloff differs. */
    shade = kVoxelShade;
    alpha = 1.0f;
  }
  if (out_z) *out_z = z;
  if (out_shade) *out_shade = shade;
  if (out_alpha) *out_alpha = alpha;
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
