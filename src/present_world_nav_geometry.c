#include "present_world_nav_geometry.h"

#include <math.h>
#include <float.h>
#include <string.h>

bool WorldNavigationRadialBoundsOutside(const WorldNavigationRadialBounds *b,
    const Sim3DDepthRadialTransform *t) {
  if (!b || !t || !isfinite(b->height_min) || !isfinite(b->height_max) ||
      b->height_min > b->height_max || !isfinite(t->sphere_radius) ||
      !isfinite(t->height_scale) || !isfinite(t->reference_height) ||
      t->sphere_radius <= 0 || t->height_scale < 0) return false;
  for (int i = 0; i < 16; ++i) if (!isfinite(t->matrix[i])) return false;
  for (int i = 0; i < 3; ++i) {
    if (!isfinite(b->normal_min[i]) || !isfinite(b->normal_max[i]) ||
        b->normal_min[i] > b->normal_max[i]) return false;
    for (int j = 0; j < 3; ++j) if (!isfinite(t->basis[i][j])) return false;
  }
  const double low_r = t->sphere_radius + (double)b->height_min*t->height_scale;
  const double high_r = t->sphere_radius + (double)b->height_max*t->height_scale;
  const double centre = t->sphere_radius + (double)t->reference_height*t->height_scale;
  double low[3], high[3];
  for (int axis = 0; axis < 3; ++axis) {
    double nlow = 0, nhigh = 0, magnitude = 0;
    for (int j = 0; j < 3; ++j) {
      const double a = (double)t->basis[axis][j]*b->normal_min[j];
      const double z = (double)t->basis[axis][j]*b->normal_max[j];
      nlow += fmin(a,z); nhigh += fmax(a,z); magnitude += fmax(fabs(a),fabs(z));
    }
    const double products[4] = {nlow*low_r,nlow*high_r,nhigh*low_r,nhigh*high_r};
    low[axis] = high[axis] = products[0];
    for (int j = 1; j < 4; ++j) {
      low[axis] = fmin(low[axis],products[j]); high[axis] = fmax(high[axis],products[j]);
    }
    /* Bound the shader's individual float operations, including cancellation
     * between reference radius and rise. This is intentionally looser than
     * ideal double-precision AABB math; uncertain edges stay on the GPU. */
    const double scale = fabs(t->sphere_radius) + fabs((double)t->reference_height*t->height_scale) +
        fmax(fabs(b->height_min),fabs(b->height_max))*t->height_scale;
    const double pad = 64*FLT_EPSILON*(1+magnitude)*scale + 64*FLT_MIN;
    if (pad > FLT_MAX/64 || !isfinite(pad)) return false;
    low[axis] -= (axis == 2 ? centre : 0) + pad;
    high[axis] += pad - (axis == 2 ? centre : 0);
  }
  bool outside = false;
  for (int plane = 0; plane < 6; ++plane) {
    const int axis = plane/2;
    const double sign = plane & 1 ? -1 : 1;
    double maximum = (double)t->matrix[15] + sign*t->matrix[12+axis];
    double magnitude = fabs(t->matrix[15]) + fabs(t->matrix[12+axis]);
    for (int j = 0; j < 3; ++j) {
      const double a = (double)t->matrix[j*4+3] + sign*t->matrix[j*4+axis];
      maximum += fmax(a*low[j],a*high[j]);
      magnitude += (fabs(t->matrix[j*4+3])+fabs(t->matrix[j*4+axis])) * fmax(fabs(low[j]),fabs(high[j]));
    }
    if (magnitude > FLT_MAX/64 || !isfinite(magnitude)) return false;
    outside |= maximum < -64*FLT_EPSILON*magnitude-64*FLT_MIN;
  }
  return outside;
}

bool WorldNavigationProjectClippedPoint(
    const WorldNavigationProjection *projection, ArRenderRectI viewport,
    const float world[3], Scene3DPoint *screen, float *depth, Scene3DClipPoint *clip) {
  if (!Scene3D_TransformToClip(projection->matrix, world[0], world[1], world[2], clip))
    return false;
  /* Behind-eye values are never perspective-divided. Their homogeneous
   * coordinates survive until each primitive is clipped for submission. */
  *screen = (Scene3DPoint){0};
  *depth = 0;
  if (clip->w > kScene3DMinimumProjectionDepth) {
    const float inverse = 1.0f / clip->w;
    screen->x = (clip->x * inverse * .5f + .5f) * viewport.w;
    screen->y = (1.0f - (clip->y * inverse * .5f + .5f)) * viewport.h;
    *depth = clip->z * inverse * .5f + .5f;
  }
  return true;
}

static bool ClipQuad(
    const Sim3DDepthVertex input[4], const Scene3DClipPoint clip[4],
    ArRenderRectI viewport, Sim3DDepthVertex output[kWorldNavigationClippedQuads * 4],
    size_t *count, WorldNavigationClipPlan *plans) {
  *count = 0;
  uint8_t all = 63, any = 0;
  for (int i = 0; i < 4; i++) {
    const uint8_t outside = WorldNavigationClipOutside(clip[i]);
    all &= outside; any |= outside;
  }
  if (all) return true;
  if (!any) {
    memcpy(output, input, 4 * sizeof(*output));
    *count = 1;
    if (plans) {
      plans[0] = (WorldNavigationClipPlan){0};
      for (int p = 0; p < 4; p++) {
        plans[0].points[p].x = input[p].x;
        plans[0].points[p].y = input[p].y;
        plans[0].points[p].depth = input[p].depth;
      }
    }
    return true;
  }
  const int triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (int half = 0; half < 2; half++) {
    const int *at = triangles[half];
    const Scene3DClipPoint triangle[3] = {clip[at[0]], clip[at[1]], clip[at[2]]};
    Scene3DClippedPolygon polygon;
    if (!Scene3D_ClipTriangle(triangle, &polygon)) return false;
    Sim3DDepthVertex vertices[kScene3DClippedPolygonCapacity];
    for (int i = 0; i < polygon.count; i++) {
      const Scene3DClippedVertex *source = &polygon.vertices[i];
      Sim3DDepthVertex *v = &vertices[i];
      const float inverse = 1.0f / source->point.w;
      *v = (Sim3DDepthVertex){
        .x = (source->point.x * inverse * .5f + .5f) * viewport.w,
        .y = (1.0f - (source->point.y * inverse * .5f + .5f)) * viewport.h,
        .depth = fminf(1, fmaxf(0, source->point.z * inverse * .5f + .5f)),
        .color = input[at[0]].color, .uv = input[at[0]].uv,
      };
      /* Difference form preserves uniform color/alpha/UV sentinel values
       * exactly, even when clipped weights sum to one only within rounding. */
      for (int p = 1; p < 3; p++) {
        const float w = source->weights[p];
        v->color.r += w * (input[at[p]].color.r - input[at[0]].color.r);
        v->color.g += w * (input[at[p]].color.g - input[at[0]].color.g);
        v->color.b += w * (input[at[p]].color.b - input[at[0]].color.b);
        v->color.a += w * (input[at[p]].color.a - input[at[0]].color.a);
        v->uv.x += w * (input[at[p]].uv.x - input[at[0]].uv.x);
        v->uv.y += w * (input[at[p]].uv.y - input[at[0]].uv.y);
      }
      /* Convex interpolation is bounded analytically; roundoff at a zero
       * haze/alpha endpoint must not violate the renderer's color contract. */
      v->color.r = fminf(1, fmaxf(0, v->color.r));
      v->color.g = fminf(1, fmaxf(0, v->color.g));
      v->color.b = fminf(1, fmaxf(0, v->color.b));
      v->color.a = fminf(1, fmaxf(0, v->color.a));
    }
    for (int i = 1; i + 1 < polygon.count; i++) {
      Sim3DDepthVertex *quad = output + *count * 4;
      quad[0] = vertices[0]; quad[1] = vertices[i];
      quad[2] = quad[3] = vertices[i + 1];
      if (plans) {
        WorldNavigationClipPlan *plan = &plans[*count];
        *plan = (WorldNavigationClipPlan){.triangle = (uint8_t)(half + 1)};
        const int source[4] = {0, i, i + 1, i + 1};
        for (int p = 0; p < 4; p++) {
          plan->points[p].x = quad[p].x;
          plan->points[p].y = quad[p].y;
          plan->points[p].depth = quad[p].depth;
          memcpy(plan->points[p].weights, polygon.vertices[source[p]].weights + 1,
              sizeof(plan->points[p].weights));
        }
      }
      (*count)++;
    }
  }
  return true;
}

bool WorldNavigationClipQuad(const Sim3DDepthVertex input[4], const Scene3DClipPoint clip[4],
    ArRenderRectI viewport, Sim3DDepthVertex output[kWorldNavigationClippedQuads * 4], size_t *count) {
  return ClipQuad(input, clip, viewport, output, count, NULL);
}

bool WorldNavigationPrepareClipPlan(const Sim3DDepthVertex input[4], const Scene3DClipPoint *clip,
    ArRenderRectI viewport, WorldNavigationClipPlan plans[kWorldNavigationClippedQuads], size_t *count) {
  if (!clip) {
    plans[0] = (WorldNavigationClipPlan){0};
    for (int p = 0; p < 4; p++) {
      plans[0].points[p].x = input[p].x;
      plans[0].points[p].y = input[p].y;
      plans[0].points[p].depth = input[p].depth;
    }
    *count = 1;
    return true;
  }
  Sim3DDepthVertex output[kWorldNavigationClippedQuads * 4];
  return ClipQuad(input, clip, viewport, output, count, plans);
}

void WorldNavigationApplyShadowUV(const WorldNavigationClipPlan *plan,
    const ArRenderPointF uv[4], ArRenderPointF output[4]) {
  for (int p = 0; p < 4; p++) {
    output[p] = uv[p];
    if (plan->triangle) {
      output[p] = uv[0];
      for (int i = 0; i < 2; i++) {
        const float w = plan->points[p].weights[i];
        output[p].x += w * (uv[plan->triangle + i].x - uv[0].x);
        output[p].y += w * (uv[plan->triangle + i].y - uv[0].y);
      }
    }
  }
}

void WorldNavigationApplyShadowPlan(const WorldNavigationClipPlan *plan,
    const ArRenderPointF uv[4], ArRenderColorF color, Sim3DDepthVertex output[4]) {
  ArRenderPointF mapped[4];
  WorldNavigationApplyShadowUV(plan, uv, mapped);
  for (int p = 0; p < 4; ++p)
    output[p] = (Sim3DDepthVertex){.x = plan->points[p].x, .y = plan->points[p].y,
        .depth = plan->points[p].depth, .color = color, .uv = mapped[p]};
}

bool WorldNavigationAppendClippedQuad(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex input[4],
    const Scene3DClipPoint *clip, ArRenderRectI viewport) {
  Sim3DDepthVertex output[kWorldNavigationClippedQuads * 4];
  size_t count;
  return WorldNavigationClipQuad(input, clip, viewport, output, &count) &&
      (!count || Sim3DDepthPass_AppendQuads(layer, output, count));
}

bool WorldNavigationAppendClippedQuads(
    Sim3DDepthPassLayer layer, const Sim3DDepthVertex *input,
    const Scene3DClipPoint *clip, size_t count, ArRenderRectI viewport) {
  enum { kBatch = 64 };
  Sim3DDepthVertex batch[kBatch * 4], clipped[kWorldNavigationClippedQuads * 4];
  size_t used = 0;
  for (size_t i = 0; i < count; i++) {
    size_t produced;
    if (!WorldNavigationClipQuad(input + i * 4, clip + i * 4, viewport, clipped, &produced))
      return false;
    for (size_t at = 0; at < produced; at++) {
      memcpy(batch + used++ * 4, clipped + at * 4, 4 * sizeof(*batch));
      if (used == kBatch) {
        if (!Sim3DDepthPass_AppendQuads(layer,batch,used)) return false;
        used = 0;
      }
    }
  }
  return !used || Sim3DDepthPass_AppendQuads(layer,batch,used);
}
