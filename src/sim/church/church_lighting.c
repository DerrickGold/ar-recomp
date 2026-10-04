#include "sim/church/church_lighting.h"

#include <math.h>
#include <string.h>
#include "render/scene3d_math.h"

enum { kShadowSize = 1024 };
typedef struct ChurchLight {
  ChurchPoint position, forward, right, up;
  float depth[kShadowSize * kShadowSize]; /* Reciprocal light-space depth. */
} ChurchLight;
static ChurchLight s_lights[kChurchLight_Count];
static bool s_ready, s_columns;
static const float kSpread = .80f;
static const ChurchPoint kWindowSource = {0, -5, 24};
static const ChurchPoint kWindowTarget = {0, 28, 0};

ChurchPoint ChurchLighting_WindowPoint(float across, float along) {
  return (ChurchPoint){across * (1.25f + 13.25f * along),
                       kWindowSource.y + (kWindowTarget.y - kWindowSource.y) * along,
                       kWindowSource.z * (1 - along)};
}

static ChurchPoint Subtract(ChurchPoint a, ChurchPoint b) {
  return (ChurchPoint){a.x - b.x, a.y - b.y, a.z - b.z};
}
static float Dot(ChurchPoint a, ChurchPoint b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static ChurchPoint Unit(ChurchPoint p) {
  const float length = sqrtf(Dot(p, p));
  if (length < .00001f) return (ChurchPoint){0, 0, 1};
  return (ChurchPoint){p.x / length, p.y / length, p.z / length};
}
static ChurchPoint Cross(ChurchPoint a, ChurchPoint b) {
  return (ChurchPoint){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

void ChurchLighting_Reset(void) { s_ready = false; }
bool ChurchLighting_Ready(bool columns) { return s_ready && s_columns == columns; }
void ChurchLighting_Begin(bool columns) {
  s_ready = false;
  s_columns = columns;
  const ChurchPoint sources[2] = {kWindowSource, {0, 57.8f, 11}};
  const ChurchPoint targets[2] = {kWindowTarget, {-3, 26, 0}};
  for (int i = 0; i < kChurchLight_Count; i++) {
    ChurchLight *light = &s_lights[i];
    light->position = sources[i];
    light->forward = Unit(Subtract(targets[i], sources[i]));
    light->right = Unit(Cross(light->forward, (ChurchPoint){0, 0, 1}));
    light->up = Cross(light->right, light->forward);
    memset(light->depth, 0, sizeof(light->depth));
  }
}
void ChurchLighting_End(void) { s_ready = true; }

static Scene3DClipPoint LightProject(const ChurchLight *light, ChurchPoint p) {
  const ChurchPoint relative = Subtract(p, light->position);
  const float depth = Dot(relative, light->forward), near = .25f, far = 150;
  return (Scene3DClipPoint){
      Dot(relative, light->right) / kSpread, Dot(relative, light->up) / kSpread,
      (far + near) / (far - near) * depth - 2 * far * near / (far - near), depth};
}

static float Edge(ChurchPoint a, ChurchPoint b, float x, float y) {
  return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}
static void RasterTriangle(ChurchLight *light, const Scene3DClipPoint clip[3]) {
  ChurchPoint p[3];
  for (int i = 0; i < 3; i++)
    p[i] = (ChurchPoint){(clip[i].x / clip[i].w + 1) * kShadowSize * .5f,
                         (clip[i].y / clip[i].w + 1) * kShadowSize * .5f, 1 / clip[i].w};
  const float area = Edge(p[0], p[1], p[2].x, p[2].y);
  if (fabsf(area) < .00001f) return;
  const int x0 = (int)fmaxf(0, floorf(fminf(p[0].x, fminf(p[1].x, p[2].x))));
  const int y0 = (int)fmaxf(0, floorf(fminf(p[0].y, fminf(p[1].y, p[2].y))));
  const int x1 = (int)fminf(kShadowSize - 1, ceilf(fmaxf(p[0].x, fmaxf(p[1].x, p[2].x))));
  const int y1 = (int)fminf(kShadowSize - 1, ceilf(fmaxf(p[0].y, fmaxf(p[1].y, p[2].y))));
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      const float a = Edge(p[1], p[2], x + .5f, y + .5f) / area;
      const float b = Edge(p[2], p[0], x + .5f, y + .5f) / area;
      const float c = 1 - a - b;
      if (a < -.00001f || b < -.00001f || c < -.00001f) continue;
      const float inverse = a * p[0].z + b * p[1].z + c * p[2].z;
      float *depth = &light->depth[y * kShadowSize + x];
      *depth = fmaxf(*depth, inverse);
    }
  }
}

void ChurchLighting_AddQuad(const ChurchPoint points[4]) {
  const int halves[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (int source = 0; source < kChurchLight_Count; source++) {
    ChurchLight *light = &s_lights[source];
    for (int half = 0; half < 2; half++) {
      Scene3DClipPoint input[3];
      for (int j = 0; j < 3; j++)
        input[j] = LightProject(light, points[halves[half][j]]);
      Scene3DClippedPolygon polygon;
      if (!Scene3D_ClipTriangle(input, &polygon)) continue;
      for (int i = 1; i + 1 < polygon.count; i++) {
        const Scene3DClipPoint triangle[3] = {polygon.vertices[0].point, polygon.vertices[i].point,
                                              polygon.vertices[i + 1].point};
        RasterTriangle(light, triangle);
      }
    }
  }
}

/* Compare in the receiver's tangent plane so the soft filter does not make
 * sloping faces shadow themselves. A small normal bias separates the mesh
 * from its rasterized copy without detaching the feet's contact shadows. */
static float Compare(const ChurchLight *light, int x, int y, ChurchPoint n, float plane,
                     float fallback) {
  if (x < 0 || x >= kShadowSize || y < 0 || y >= kShadowSize) return 1;
  const float u = ((x + .5f) * 2 / kShadowSize - 1) * kSpread;
  const float v = ((y + .5f) * 2 / kShadowSize - 1) * kSpread;
  const float inverse = fabsf(plane) > .01f ? (n.z + n.x * u + n.y * v) / plane : fallback;
  return light->depth[y * kShadowSize + x] <= inverse + .000018f ? 1 : 0;
}

float ChurchLighting_Visibility(ChurchLightSource source, ChurchPoint p, ChurchPoint normal) {
  if (!s_ready || source < 0 || source >= kChurchLight_Count) return 1;
  const ChurchLight *light = &s_lights[source];
  normal = Unit(normal);
  p.x += normal.x * .025f;
  p.y += normal.y * .025f;
  p.z += normal.z * .025f;
  const Scene3DClipPoint clip = LightProject(light, p);
  if (clip.w <= .25f || fabsf(clip.x) >= clip.w || fabsf(clip.y) >= clip.w) return 1;
  const float sx = (clip.x / clip.w + 1) * kShadowSize * .5f - .5f;
  const float sy = (clip.y / clip.w + 1) * kShadowSize * .5f - .5f;
  const ChurchPoint n = {Dot(normal, light->right), Dot(normal, light->up),
                         Dot(normal, light->forward)};
  const float plane = Dot(normal, Subtract(p, light->position));
  const int cx = (int)fminf(kShadowSize - 1, fmaxf(0, floorf(sx + .5f)));
  const int cy = (int)fminf(kShadowSize - 1, fmaxf(0, floorf(sy + .5f)));
  const float blocker = light->depth[cy * kShadowSize + cx];
  const float separation = blocker > 0 ? fmaxf(0, clip.w - 1 / blocker) : 2;
  /* Contact and self-shadow edges stay tight. Penumbrae widen with the
   * distance from caster to receiver, rather than blurring a curved surface
   * against itself with the same wide floor-shadow kernel. */
  const float radius = fminf(3.6f, .30f + separation * 1.70f);
  float visible = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const float x = sx + dx * radius, y = sy + dy * radius;
      const int ix = (int)floorf(x), iy = (int)floorf(y);
      const float fx = x - ix, fy = y - iy;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
          visible += Compare(light, ix + i, iy + j, n, plane, 1 / clip.w) * (i ? fx : 1 - fx) *
                     (j ? fy : 1 - fy) / 9;
    }
  }
  return visible;
}

ArRenderColorF ChurchLighting_Shade(ChurchPoint p, ChurchPoint normal, ArRenderColorF color,
                                    float ambient_occlusion) {
  normal = Unit(normal);
  const float distance = fmaxf(0, 58 - p.y);
  const float bounce = .18f * expf(-distance / 62 - p.x * p.x / 420 - p.z * p.z / 900);
  const float ambient =
      (.018f + bounce * (.42f + .58f * (1 - fabsf(normal.z)))) * ambient_occlusion;
  const float t = fmaxf(.05f, (kWindowSource.z - p.z) / kWindowSource.z);
  const ChurchPoint center = ChurchLighting_WindowPoint(0, t);
  const float radius = ChurchLighting_WindowPoint(1, t).x * .86f;
  const float wx = p.x / radius;
  const float wy = (p.y - center.y) / (1 + 8 * t);
  const float spot = p.z < kWindowSource.z ? expf(-2 * (wx * wx + wy * wy)) : 0;
  const float door = p.y < 58 ? .24f * expf(-distance / 65 - p.x * p.x / 100 - p.z * p.z / 180) : 0;
  float direct[2] = {1.30f * spot, door};
  for (int source = 0; source < kChurchLight_Count; source++) {
    if (direct[source] < .001f) continue;
    const ChurchPoint toward = Unit(Subtract(s_lights[source].position, p));
    const float diffuse = fmaxf(0, (Dot(normal, toward) + .12f) / 1.12f);
    direct[source] *= diffuse * ChurchLighting_Visibility((ChurchLightSource)source, p, normal);
  }
  color.r *= ambient * .90f + direct[0] + direct[1] * .65f;
  color.g *= ambient + direct[0] * .90f + direct[1] * .82f;
  color.b *= ambient * 1.12f + direct[0] * .72f + direct[1];
  return color;
}

ArRenderColorF ChurchLighting_ActorTint(ChurchPoint p, float across) {
  /* A broad, soft profile receives the front window and a little rear fill.
   * This is only a lighting proxy: the visible figure remains native pixel art.
   * The lifted response preserves its painted shading in this very dark room. */
  across = fmaxf(-1, fminf(1, across));
  const float side = across * .55f, facing = sqrtf(1 - side * side);
  const ArRenderColorF white = {1, 1, 1, 1};
  const ArRenderColorF front =
      ChurchLighting_Shade(p, (ChurchPoint){side, -facing, .35f}, white, 1);
  const ArRenderColorF back = ChurchLighting_Shade(p, (ChurchPoint){side, facing, .35f}, white, 1);
  ArRenderColorF tint = {.86f * front.r + .14f * back.r, .86f * front.g + .14f * back.g,
                         .86f * front.b + .14f * back.b, 1};
  tint.r = .35f + .65f * sqrtf(fminf(1, fmaxf(0, tint.r)));
  tint.g = .35f + .65f * sqrtf(fminf(1, fmaxf(0, tint.g)));
  tint.b = .35f + .65f * sqrtf(fminf(1, fmaxf(0, tint.b)));
  return tint;
}
