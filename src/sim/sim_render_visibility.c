#include "sim_render_metadata.h"
#include <math.h>

/* Pure, classified placement policy shared by capture and both renderers. */
bool Sim3D_HeightClassStandsOnTerrain(SimHeightClass height_class) {
  switch (height_class) {
    case kSimHeightClass_Grounded:
    case kSimHeightClass_WaterPlane:
    case kSimHeightClass_GroundEffect:
    case kSimHeightClass_SemiGrounded:
    case kSimHeightClass_GroundStrike:
      return true;
    case kSimHeightClass_None:
    case kSimHeightClass_Flying:
    case kSimHeightClass_FlyingProjectile:
    case kSimHeightClass_MapPlane:
    case kSimHeightClass_Count:
      break;
  }
  return false;
}

/* Pure visibility masks, shared by metadata validation and presentation. */
float Sim3D_CloudCoverage(float x, float y, float clear_x0, float clear_x1,
                          float clear_y0, float clear_y1, float inset,
                          float falloff) {
  /* Signed distance outside the rectangle: negative inside, zero on the edge.
   * The larger axis wins rather than the diagonal, so a corner is never
   * thinner than the edges meeting there. */
  float dx = clear_x0 - x;
  float dx1 = x - clear_x1;
  if (dx1 > dx) dx = dx1;
  float dy = clear_y0 - y;
  float dy1 = y - clear_y1;
  if (dy1 > dy) dy = dy1;
  float distance = dx > dy ? dx : dy;

  /* The inset is in pixels but the rectangle's two axes are very different
   * sizes -- roughly 496 wide against 224 tall -- so an inset the horizontal
   * axis shrugs off can swallow the vertical one from both sides and veil the
   * middle of the screen. Cap it at a quarter of the shorter half-extent so
   * the playable centre stays clear whatever the setting says. */
  float half_x = (clear_x1 - clear_x0) * 0.5f;
  float half_y = (clear_y1 - clear_y0) * 0.5f;
  float smallest = half_x < half_y ? half_x : half_y;
  float limit = smallest * 0.5f;
  if (inset > limit) inset = limit;
  if (inset < 0.0f) inset = 0.0f;

  float width = inset + falloff;
  if (width <= 0.0f) return distance >= 0.0f ? 1.0f : 0.0f;
  float coverage = (distance + inset) / width;
  return coverage < 0.0f ? 0.0f : coverage > 1.0f ? 1.0f : coverage;
}

float Sim3D_CullProximity(int16_t anchor_x, int16_t anchor_y,
                          int margin_left, int margin_right,
                          int margin_top, int margin_bottom,
                          int lead, int corner, int lift_inset) {
  if (lead <= 0) lead = 1;

  /* The complete host-renderable window. Real OAM remains vertically
   * authentic, but exact synthetic parts provide the explicit top/bottom
   * reach, so the cues must follow those margins rather than the byte decode. */
  float x0 = (float)(-margin_left);
  float x1 = (float)(kSimSpriteWindowBiasedWidth + margin_right);
  float y0 = (float)(-margin_top);
  float y1 = (float)(kSimSpriteWindowBiasedHeight + margin_bottom);
  /* Bottom only; see the header. Clamped so an absurd inset cannot invert the
   * window or collapse it onto a line. */
  if (lift_inset > 0) {
    float limit = (y1 - y0) * 0.5f;
    float inset = (float)lift_inset;
    if (inset > limit) inset = limit;
    y1 -= inset;
  }

  float half_x = (x1 - x0) * 0.5f;
  float half_y = (y1 - y0) * 0.5f;
  float centre_x = (x0 + x1) * 0.5f;
  float centre_y = (y0 + y1) * 0.5f;

  /* Corner radius, clamped so it can never exceed the shorter half-extent --
   * beyond that the "rectangle" is just a capsule and the window stops
   * describing the emitter's predicate at all. */
  float radius = (float)corner;
  float smallest = half_x < half_y ? half_x : half_y;
  if (radius > smallest) radius = smallest;
  if (radius < 0.0f) radius = 0.0f;

  /* Signed distance to a rounded rectangle: negative inside, zero on the
   * edge. The previous form took the larger axis, which is a Chebyshev
   * distance and therefore an axis-aligned box with hard corners -- the
   * visible squareness of the lit region was that choice showing through.
   *
   * Rounding moves cover in the safe direction. At a corner this distance is
   * radius*(sqrt(2)-1) GREATER than the flat-edge case, so a record cutting
   * the diagonal is covered sooner than one approaching the edges meeting
   * there, never later. */
  float qx = (float)anchor_x - centre_x;
  float qy = (float)anchor_y - centre_y;
  qx = (qx < 0.0f ? -qx : qx) - (half_x - radius);
  qy = (qy < 0.0f ? -qy : qy) - (half_y - radius);
  float ox = qx > 0.0f ? qx : 0.0f;
  float oy = qy > 0.0f ? qy : 0.0f;
  float outside = sqrtf(ox * ox + oy * oy);
  float longest = qx > qy ? qx : qy;
  float inside = longest < 0.0f ? longest : 0.0f;
  float distance = outside + inside - radius;

  float ramp = (distance + (float)lead) / (float)lead;
  if (ramp <= 0.0f) return 0.0f;
  if (ramp >= 1.0f) return 1.0f;
  /* Feathered rather than linear. A linear ramp has a discontinuous slope at
   * both ends, and the eye finds those two creases and reads them as edges --
   * which is the whole thing the ramp exists to avoid. */
  return ramp * ramp * (3.0f - 2.0f * ramp);
}

float Sim3D_SourceCullCover(const SimSourceRecord *source,
                            int margin_left, int margin_right,
                            int margin_top, int margin_bottom,
                            int lead, int corner, int lift_inset) {
  if (!source || !source->anchor_valid) return 0.0f;

  /* World-tier records only. Fixed-tier records are HUD and cursor furniture
   * that lives in screen space; it does not belong to the town and a cloud
   * over it would be nonsense. */
  if (source->tier != kSimRecordTier_World) return 0.0f;

  /* A record with no parts at all never asked to be drawn. Only the sprite
   * window may create cover, so a record that emitted nothing AND was never
   * clipped is the game's own decision, not ours to hide. */
  if (!source->oam_count && !source->synthetic_parts &&
      !source->clipped_parts)
    return 0.0f;

  /* Deliberately the record's own anchor, NOT where the renderer draws it.
   * See Sim3D_SourceDrawLift: when the cover arrives and where it goes are
   * two different questions, and this is the first one. */
  return Sim3D_CullProximity(source->anchor_x, source->anchor_y,
                             margin_left, margin_right,
                             margin_top, margin_bottom,
                             lead, corner, lift_inset);
}
