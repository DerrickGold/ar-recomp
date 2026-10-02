/* A bounded CPU coverage field shared by all directional lights in a pass.
 * Samples describe finite-depth atmospheric scattering, not infinite opaque
 * wedges under every platform. No scene resolve, upload or GPU-specific API. */
#include "action_effect_render_internal.h"
static void Raster(ActionSceneryShadow *s, ArRenderPointF p[4]) {
  float top = kActionShadowHeight, bottom = 0;
  for (unsigned i = 0; i < 4; ++i) {
    p[i].x = (p[i].x - s->x) / s->step;
    p[i].y = (p[i].y - s->y) / s->step;
    top = fminf(top, p[i].y);
    bottom = fmaxf(bottom, p[i].y);
  }
  for (int y = (int)floorf(fmaxf(0, top)); y < (int)ceilf(fminf(kActionShadowHeight, bottom));
       ++y) {
    for (unsigned tap = 0; tap < 2; ++tap) {
      const float scan = y + .25f + .5f * tap;
      float left = kActionShadowWidth, right = 0;
      unsigned hits = 0;
      for (unsigned e = 0; e < 4; ++e) {
        ArRenderPointF a = p[e], b = p[(e + 1) & 3];
        if ((a.y <= scan && b.y > scan) || (b.y <= scan && a.y > scan)) {
          const float x = a.x + (b.x - a.x) * (scan - a.y) / (b.y - a.y);
          left = fminf(left, x);
          right = fmaxf(right, x);
          ++hits;
        }
      }
      if (hits != 2 || left >= right) continue;
      for (int x = (int)floorf(fmaxf(0, left)); x < (int)ceilf(fminf(kActionShadowWidth, right));
           ++x) {
        const unsigned at = y * kActionShadowWidth + x;
        const unsigned value =
            s->pixels[at] + (unsigned)(127.5f * (fminf(x + 1, right) - fmaxf(x, left)) + .5f);
        s->pixels[at] = (uint8_t)(value > 255 ? 255 : value);
      }
    }
  }
}
bool ActionSceneryShadow_Prepare(ActionSceneryShadow *s, ActionSceneryShadowCache *cache,
                                 const ActionMoonlightOcclusion *o,
                                 ActionEffectProjectPointFn project, ActionEffectClipBoundsFn clip,
                                 void *context) {
  *s = (ActionSceneryShadow){0};
  if (!o || !o->valid || !o->count) return true;
  if (o->count > kActionMoonlightMaxOccluders || !project) return false;
  if (!cache) return false;
  const bool cacheable = project == ActionEffectProjection_ProjectPoint &&
      clip == ActionEffectProjection_ClipBounds && context;
  if (cacheable && cache->ready && cache->occlusion.count == o->count &&
      !memcmp(cache->occlusion.rectangles, o->rectangles, o->count * sizeof(o->rectangles[0])) &&
      ActionEffectProjection_Matches(&cache->projection, context)) {
    *s = (ActionSceneryShadow){cache->pixels, cache->x, cache->y, cache->step, true};
    return true;
  }
  cache->ready = false;
  uint8_t *pixels = cache->pixels;
  ActionEffectInstance caster = {
      .projection_plane = kActionEffectProjectionPlane_Bg1,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClippedMesh,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-2048, -2048, 16384, 16384}}};
  ActionEffectLocalRect bounds = caster.geometry.data.rect;
  if (clip && !clip(context, &caster, &bounds)) return true;
  if (!RectIsSane(&bounds)) return false;
  ArRenderPointF corners[4];
  float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  /* Use the captured caster window, not the whole finite room, for resolution. */
  int wx0 = 16384, wy0 = 16384, wx1 = -2048, wy1 = -2048;
  for (unsigned i = 0; i < o->count; ++i) {
    const ActionMoonlightOccluder *r = &o->rectangles[i];
    if (r->x0 >= r->x1 || r->y0 >= r->y1 || r->x0 < -2048 || r->x1 > 16384 || r->y0 < -2048 ||
        r->y1 > 16384)
      return false;
    if (r->x0 < wx0) wx0 = r->x0;
    if (r->y0 < wy0) wy0 = r->y0;
    if (r->x1 > wx1) wx1 = r->x1;
    if (r->y1 > wy1) wy1 = r->y1;
  }
  const float window[4][2] = {{wx0, wy0}, {wx1, wy0}, {wx1, wy1}, {wx0, wy1}};
  for (unsigned i = 0; i < 4; ++i) {
    if (!project(context, &caster, window[i][0], window[i][1], &corners[i])) return true;
    if (!isfinite(corners[i].x) || !isfinite(corners[i].y)) return false;
    x0 = fminf(x0, corners[i].x);
    y0 = fminf(y0, corners[i].y);
    x1 = fmaxf(x1, corners[i].x);
    y1 = fmaxf(y1, corners[i].y);
  }
  s->x = floorf(x0) - 2;
  s->y = floorf(y0) - 2;
  s->step = fmaxf(
      .01f, fmaxf((x1 - s->x + 2) / kActionShadowWidth, (y1 - s->y + 2) / kActionShadowHeight));
  s->pixels = pixels;
  memset(pixels, 0, kActionShadowWidth * kActionShadowHeight);
  for (unsigned i = 0; i < o->count; ++i) {
    const ActionMoonlightOccluder *r = &o->rectangles[i];
    const float x0 = fmaxf(r->x0, bounds.x0), x1 = fminf(r->x1, bounds.x1),
                y0 = fmaxf(r->y0, bounds.y0), y1 = fminf(r->y1, bounds.y1);
    if (x0 >= x1 || y0 >= y1) continue;
    const float p[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    bool valid = true;
    for (unsigned j = 0; j < 4; ++j)
      if (!project(context, &caster, p[j][0], p[j][1], &corners[j])) {
        valid = false;
        break;
      }
    if (valid) Raster(s, corners);
  }
  s->valid = true;
  if (cacheable) {
    cache->occlusion.count = o->count;
    memcpy(cache->occlusion.rectangles, o->rectangles, o->count * sizeof(o->rectangles[0]));
    cache->x = s->x; cache->y = s->y; cache->step = s->step;
    ActionEffectProjection_Remember(&cache->projection, context);
    cache->ready = true;
  }
  return true;
}
static float Coverage(const ActionSceneryShadow *s, float x, float y) {
  x = (x - s->x) / s->step - .5f;
  y = (y - s->y) / s->step - .5f;
  if (x < 0 || y < 0 || x >= kActionShadowWidth - 1 || y >= kActionShadowHeight - 1) return 0;
  const int ix = (int)x, iy = (int)y;
  const float fx = x - ix, fy = y - iy;
  const uint8_t *p = s->pixels + iy * kActionShadowWidth + ix;
  const float a = p[0] + (p[1] - p[0]) * fx,
              b = p[kActionShadowWidth] + (p[kActionShadowWidth + 1] - p[kActionShadowWidth]) * fx;
  return (a + (b - a) * fy) / 255;
}
void ActionSceneryShadow_Apply(ActionEffectGeometryWriter *w, int begin,
                               const ActionEffectInstance *e, float x, float y, bool multiply,
                               ActionEffectProjectPointFn project, void *context) {
  const ActionSceneryShadow *s = w->shadow;
  if (!s || !s->valid) return;
  ActionEffectInstance source = *e;
  source.flags |= kActionEffectFlag_ClippedMesh;
  ArRenderPointF origin;
  if (!project(context, &source, x, y, &origin)) return;
  static const float transport[] = {.74f, .85f, .96f};
  for (int i = begin; i < w->vertex_count; ++i) {
    ArRenderVertex2D *v = &w->vertices[i];
    float blocked = 0;
    for (unsigned j = 0; j < 3; ++j)
      blocked += Coverage(s, origin.x + (v->position.x - origin.x) * transport[j],
                          origin.y + (v->position.y - origin.y) * transport[j]);
    const float visibility = 1 - .86f * blocked / 3;
    if (multiply) {
      v->color.r *= visibility;
      v->color.g *= visibility;
      v->color.b *= visibility;
    } else
      v->color.a *= visibility;
  }
}
