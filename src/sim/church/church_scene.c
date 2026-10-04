#include "sim/church/church_scene.h"
#include "sim/church/church_lighting.h"
#include "sim/church/church_detail.h"
#include "sim/church/church_exterior.h"
#include "sim/mountains/sim_background_mountain_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "render/scene3d_math.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/town/sim_town_terrain.h"
#include "sim/voxels/sim_background_voxel_model_cache.h"
#include "sim/voxels/sim_background_voxel_palette.h"
#include "sim/voxels/sim_background_voxel_project.h"
#include "sim/voxels/sim_background_voxel_proportions.h"
#include "sim/voxels/sim_background_bridge.h"

typedef struct ChurchGeometry {
  Sim3DDepthSurfaceVertex *vertices;
  size_t count, capacity;
} ChurchGeometry;
static Sim3DDepthSurfaceVertex SurfacePoint(ChurchPoint p, ArRenderColorF color, ArRenderPointF uv);
static ChurchSceneStats s_stats;
ChurchSceneStats ChurchScene_Stats(void) { return s_stats; }
static ChurchFocus s_focus;
static bool s_focus_building;
static Sim3DDepthMesh *s_room_mesh[2], *s_exterior_mesh, *s_foliage_mesh[3];
static ArRenderTexture s_room_image[2];
static int s_room_width, s_room_height;
static bool s_room_images_valid;
static size_t s_room_quads[2], s_exterior_quads, s_foliage_quads[3];
static ArRenderTexture s_white_texture;
static uint32_t s_detail_serial;
static const SimBackgroundVoxelScene *s_detail_town;
static ChurchSceneOptions s_detail_options;
static bool s_detail_valid, s_detail_columns;
static int s_moving_trees[8], s_moving_tree_count;
enum { kActorColumns = 4, kActorRows = 8, kActorQuads = 2 * kActorColumns * kActorRows };
static ArRenderColorF s_actor_tint[2][kActorRows + 1][kActorColumns + 1];
static const float kActorY = 31, kActorX = 3.6f, kActorWidth = 6.8f, kActorHeight = 5.8f;

typedef struct ChurchView {
  ArRenderRectI viewport;
  float aspect, brightness;
  double seconds;
  float origin_x, origin_y, origin_z, landscape;
  uint8_t town;
  const ChurchSceneOptions *options;
  bool valid, interior, shadow_build;
  ChurchGeometry *geometry, *foreground;
  int foliage_pose;
} ChurchView;

static const float kChurchPi = 3.141592654f;
static const float kChurchDoorY = 58;
static const float kChurchTownOffsetY = 26; /* Door minus native 32-unit footprint. */
static const unsigned kChurchSky = 0x4a8fea;
static const float kChurchColumnX = 10.0f;
/* Lower the eye as well as its aim: less of a horizontal altar top is visible,
 * while the doorway still exposes the town and blue daytime sky. */
static const float kChurchEyeY = -3.5f;
static const float kChurchEyeZ = 14.0f;
static const float kChurchCameraTilt = .40f;
static const float kChurchFocalLength = 1.75f;
static const float kChurchHalfWidth = 16.0f;
static const float kChurchCeilingZ = 24.0f;
/* Local zero is the cathedral's SIM plot-anchor elevation. Floor, threshold,
 * column bases, people and altar contacts all use this exact plane. */
static const float kChurchFloorZ = 0.0f;
/* Physical dimensions, independent of the native sprite's projected outline.
 * Author the altar around local Y=0 with level rim/feet, then translate only.
 * Keep the far edge at Y=26 beside the visitors as its depth is adjusted. */
static const float kAltarHalfWidth = 4.60f;
static const float kAltarHalfDepth = 1.55f;
static const float kAltarFarY = 26.0f;

static ArRenderColorF Color(unsigned rgb) {
  return (ArRenderColorF){((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                          (rgb & 255) / 255.0f, 1};
}

/* +Y goes out of the south-facing cathedral. X is camera-right, hence the
 * sign reversal when importing town models. Eye is raised above the dais. */
static Scene3DClipPoint Project(const ChurchView *view, ChurchPoint p) {
  const float dy = p.y - kChurchEyeY, dz = p.z - kChurchEyeZ;
  const float depth = dy * cosf(kChurchCameraTilt) - dz * sinf(kChurchCameraTilt);
  const float up = dy * sinf(kChurchCameraTilt) + dz * cosf(kChurchCameraTilt);
  const float f = kChurchFocalLength, near = .4f, far = 6000.0f;
  return (Scene3DClipPoint){p.x * f / view->aspect, up * f,
                            (far + near) / (far - near) * depth - 2 * far * near / (far - near),
                            depth};
}

/* Stable aperture coordinates remove aspect from the tiny depth proxy. */
/* Only the aperture can reveal exterior geometry. The padding keeps blur
 * samples and gently moving crowns covered, with conservative near crossings. */
static bool ExteriorVisible(const ChurchView *v, const ChurchPoint p[4]) {
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  bool front = false, back = false;
  for (int i = 0; i < 4; i++) {
    Scene3DClipPoint c = Project(v, p[i]);
    if (c.w <= .4f) {
      back = true;
      continue;
    }
    front = true;
    const float x = c.x / c.w * v->aspect, y = c.y / c.w;
    minx = fminf(minx, x);
    maxx = fmaxf(maxx, x);
    miny = fminf(miny, y);
    maxy = fmaxf(maxy, y);
  }
  if (!front) return false;
  if (back) return true;
  return maxx >= s_focus.left - .025f && minx <= s_focus.right + .025f &&
         maxy >= s_focus.bottom - .025f && miny <= s_focus.top + .025f;
}

static void FocusQuad(const ChurchView *v, const ChurchPoint p[4]) {
  if (!s_focus_building || v->interior) return;
  Scene3DClipPoint c[4];
  for (int i = 0; i < 4; i++) {
    c[i] = Project(v, p[i]);
    c[i].x *= v->aspect;
  }
  ChurchFocus_Quad(&s_focus, c);
}

static bool CaptureQuad(ChurchGeometry *b, const ChurchPoint p[4], const ArRenderColorF lit[4]) {
  if (b->count == b->capacity) {
    size_t next = b->capacity ? b->capacity * 2 : 2048;
    if (next > kSim3DDepthMaximumSourceQuads) return false;
    void *grown = realloc(b->vertices, next * 4 * sizeof(*b->vertices));
    if (!grown) return false;
    b->vertices = grown;
    b->capacity = next;
  }
  for (int i = 0; i < 4; i++)
    b->vertices[b->count * 4 + i] = SurfacePoint(p[i], lit[i], (ArRenderPointF){.5f, .5f});
  b->count++;
  return true;
}

/* Split the opaque room at the native actors' shared billboard depth. Two
 * cached RGBA images can then sandwich the animated actors with exact static
 * altar/wall occlusion, including polygons crossing this plane. */
static bool CaptureRoom(ChurchView *v, const ChurchPoint p[4], const ArRenderColorF colors[4]) {
  const float plane = Project(v, (ChurchPoint){0, 31, 0}).w;
  for (int side = 0; side < 2; side++) {
    ChurchPoint points[6];
    ArRenderColorF lit[6];
    int count = 0;
    for (int i = 0; i < 4; i++) {
      int previous = (i + 3) % 4;
      float a = (Project(v, p[previous]).w - plane) * (side ? -1 : 1);
      float b = (Project(v, p[i]).w - plane) * (side ? -1 : 1);
      if ((a < 0 && b > 0) || (a > 0 && b < 0)) {
        float t = a / (a - b);
        points[count] = (ChurchPoint){p[previous].x + (p[i].x - p[previous].x) * t,
                                      p[previous].y + (p[i].y - p[previous].y) * t,
                                      p[previous].z + (p[i].z - p[previous].z) * t};
        lit[count] =
            (ArRenderColorF){colors[previous].r + (colors[i].r - colors[previous].r) * t,
                             colors[previous].g + (colors[i].g - colors[previous].g) * t,
                             colors[previous].b + (colors[i].b - colors[previous].b) * t, 1};
        count++;
      }
      if (b >= 0) {
        points[count] = p[i];
        lit[count++] = colors[i];
      }
    }
    if (count < 3) continue;
    ChurchGeometry *out = side ? v->foreground : v->geometry;
    if (count == 4) {
      if (!CaptureQuad(out, points, lit)) return false;
    } else
      for (int i = 1; i + 1 < count; i++) {
        const ChurchPoint q[4] = {points[0], points[i], points[i + 1], points[i + 1]};
        const ArRenderColorF c[4] = {lit[0], lit[i], lit[i + 1], lit[i + 1]};
        if (!CaptureQuad(out, q, c)) return false;
      }
  }
  return true;
}

/* Contact occlusion affects ambient fill only. Direct light is blocked by the
 * actual altar mesh and the round column proxies in the two light maps. */
static float FloorContact(const ChurchView *v, ChurchPoint p) {
  if (p.z > .08f) return 1;
  const float dx = fmaxf(0, fabsf(p.x) - kAltarHalfWidth + .25f);
  const float dy = fmaxf(0, fabsf(p.y - kAltarFarY + kAltarHalfDepth) - kAltarHalfDepth + .25f);
  float contact = .80f * expf(-(dx * dx + dy * dy) / .7f);
  if (ArRenderTexture_IsValid(v->options->people)) {
    for (int row = 0; row < 6; row++) {
      const float y = p.y - 13 - row * 9;
      const float x = fabsf(p.x) - kChurchColumnX;
      contact = fmaxf(contact, .85f * expf(-(x * x + y * y) / 2));
    }
  }
  return 1 - contact;
}

static ArRenderColorF Shade(const ChurchView *view, ChurchPoint p, ArRenderColorF color,
                            ChurchPoint normal) {
  if (view->interior) color = ChurchLighting_Shade(p, normal, color, FloorContact(view, p));
  color.r *= view->brightness;
  color.g *= view->brightness;
  color.b *= view->brightness;
  return color;
}

/* Clip in homogeneous space so the near floor/walls can cross the camera
 * without disappearing. Preserve each corner's light through clipping. */
static void LitQuad(ChurchView *view, const ChurchPoint p[4], const ArRenderColorF lit[4]) {
  if (!view->valid) return;
  if (!view->interior && !ExteriorVisible(view, p)) return;
  FocusQuad(view, p);
  if (view->geometry) {
    view->valid =
        view->foreground ? CaptureRoom(view, p, lit) : CaptureQuad(view->geometry, p, lit);
    return;
  }
  Scene3DClipPoint clip[4];
  for (int i = 0; i < 4; i++)
    clip[i] = Project(view, p[i]);
  const int triangles[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (int t = 0; t < 2; t++) {
    Scene3DClipPoint input[3];
    for (int i = 0; i < 3; i++)
      input[i] = clip[triangles[t][i]];
    Scene3DClippedPolygon polygon;
    if (!Scene3D_ClipTriangle(input, &polygon)) {
      view->valid = false;
      return;
    }
    for (int i = 1; i + 1 < polygon.count; i++) {
      const int at[4] = {0, i, i + 1, i + 1};
      Sim3DDepthVertex vertices[4];
      for (int j = 0; j < 4; j++) {
        const Scene3DClippedVertex *vertex = &polygon.vertices[at[j]];
        Scene3DClipPoint c = vertex->point;
        ArRenderColorF color = {0, 0, 0, 1};
        for (int k = 0; k < 3; k++) {
          color.r += lit[triangles[t][k]].r * vertex->weights[k];
          color.g += lit[triangles[t][k]].g * vertex->weights[k];
          color.b += lit[triangles[t][k]].b * vertex->weights[k];
        }
        vertices[j] = (Sim3DDepthVertex){(c.x / c.w + 1) * view->viewport.w * .5f,
                                         (1 - c.y / c.w) * view->viewport.h * .5f,
                                         (c.z / c.w + 1) * .5f,
                                         color,
                                         {-1, -1}};
      }
      if (!Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, vertices)) view->valid = false;
    }
  }
}

static void QuadColors(ChurchView *view, const ChurchPoint p[4], const ArRenderColorF colors[4]) {
  ArRenderColorF lit[4];
  const ChurchPoint a = {p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
  const ChurchPoint b = {p[3].x - p[0].x, p[3].y - p[0].y, p[3].z - p[0].z};
  const ChurchPoint normal = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  for (int i = 0; i < 4; i++) {
    lit[i] = Shade(view, p[i], colors[i], normal);
  }
  LitQuad(view, p, lit);
}

static void Quad(ChurchView *view, const ChurchPoint p[4], ArRenderColorF color) {
  const ArRenderColorF colors[4] = {color, color, color, color};
  QuadColors(view, p, colors);
}

static void Box(ChurchView *v, float x, float y, float z, float w, float d, float h, unsigned rgb) {
  const ChurchPoint p[8] = {
      {x, y, z},     {x + w, y, z},     {x + w, y + d, z},     {x, y + d, z},
      {x, y, z + h}, {x + w, y, z + h}, {x + w, y + d, z + h}, {x, y + d, z + h}};
  const int faces[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6},
                           {3, 0, 4, 7}, {4, 5, 6, 7}, {3, 2, 1, 0}};
  const float shade[6] = {.83f, .70f, .93f, .76f, 1, .55f};
  for (int f = 0; f < 6; f++) {
    ChurchPoint q[4];
    for (int j = 0; j < 4; j++)
      q[j] = p[faces[f][j]];
    ArRenderColorF c = Color(rgb);
    c.r *= shade[f];
    c.g *= shade[f];
    c.b *= shade[f];
    Quad(v, q, c);
  }
}

static void Floor(ChurchView *v, float x, float y, float z, float w, float d, unsigned rgb) {
  const ChurchPoint p[4] = {{x, y, z}, {x + w, y, z}, {x + w, y + d, z}, {x, y + d, z}};
  Quad(v, p, Color(rgb));
}

static void Arch(ChurchView *v) {
  /* A semicircular opening; solid voussoirs occlude the real town behind. */
  for (int i = 0; i < 16; i++) {
    float a = i * kChurchPi / 16, b = (i + 1) * kChurchPi / 16;
    ChurchPoint p[4] = {{5 * cosf(a), kChurchDoorY, 10 + 5 * sinf(a)},
                        {5 * cosf(b), kChurchDoorY, 10 + 5 * sinf(b)},
                        {5 * cosf(b), kChurchDoorY, kChurchCeilingZ},
                        {5 * cosf(a), kChurchDoorY, kChurchCeilingZ}};
    Quad(v, p, Color(0x635242));
    ChurchPoint trim[4] = {{5 * cosf(a), kChurchDoorY - .35f, 10 + 5 * sinf(a)},
                           {5 * cosf(b), kChurchDoorY - .35f, 10 + 5 * sinf(b)},
                           {5.8f * cosf(b), kChurchDoorY - .35f, 10 + 5.8f * sinf(b)},
                           {5.8f * cosf(a), kChurchDoorY - .35f, 10 + 5.8f * sinf(a)}};
    Quad(v, trim, Color(i % 2 ? 0xb5a584 : 0x8c7b6b));
  }
  /* Three units of stone reveal, with a restrained daylight edge at the exit. */
  for (int i = 0; i < 16; i++) {
    const float a = i * kChurchPi / 16, b = (i + 1) * kChurchPi / 16;
    ChurchPoint reveal[4] = {{5 * cosf(a), kChurchDoorY, 10 + 5 * sinf(a)},
                             {5 * cosf(b), kChurchDoorY, 10 + 5 * sinf(b)},
                             {5 * cosf(b), kChurchDoorY + 3, 10 + 5 * sinf(b)},
                             {5 * cosf(a), kChurchDoorY + 3, 10 + 5 * sinf(a)}};
    Quad(v, reveal, Color(0x73634a));
    const ChurchPoint edge[4] = {{5 * cosf(a), kChurchDoorY + 2.75f, 10 + 5 * sinf(a)},
                                 {5 * cosf(b), kChurchDoorY + 2.75f, 10 + 5 * sinf(b)},
                                 {5 * cosf(b), kChurchDoorY + 3, 10 + 5 * sinf(b)},
                                 {5 * cosf(a), kChurchDoorY + 3, 10 + 5 * sinf(a)}};
    ArRenderColorF light[4];
    for (int j = 0; j < 4; j++)
      light[j] = (ArRenderColorF){.20f, .19f, .15f, 1};
    LitQuad(v, edge, light);
  }
  const ChurchPoint left[4] = {{-5, kChurchDoorY, 0},
                               {-5, kChurchDoorY + 3, 0},
                               {-5, kChurchDoorY + 3, 10},
                               {-5, kChurchDoorY, 10}};
  const ChurchPoint right[4] = {{5, kChurchDoorY + 3, 0},
                                {5, kChurchDoorY, 0},
                                {5, kChurchDoorY, 10},
                                {5, kChurchDoorY + 3, 10}};
  Quad(v, left, Color(0x73634a));
  Quad(v, right, Color(0x73634a));
  for (int x = 0; x < 5; x++)
    Floor(v, -5 + x * 2, kChurchDoorY, .01f, 2, 3, x % 2 ? 0x948463 : 0xa59473);
  Box(v, -kChurchHalfWidth, kChurchDoorY, 0, kChurchHalfWidth - 5, 1, kChurchCeilingZ, 0x635242);
  Box(v, 5, kChurchDoorY, 0, kChurchHalfWidth - 5, 1, kChurchCeilingZ, 0x635242);
  Box(v, -5.8f, kChurchDoorY - .45f, 0, .8f, .8f, 10, 0xb2ad94);
  Box(v, 5, kChurchDoorY - .45f, 0, .8f, .8f, 10, 0xb2ad94);
}

static void Interior(ChurchView *v) {
  Box(v, -kChurchHalfWidth, -9, -1, 2 * kChurchHalfWidth, kChurchDoorY + 10, .99f, 0x312921);
  /* Continuous stone: no tile gaps, checkerboard, or visible grid. */
  for (int y = -8; y < (int)kChurchDoorY; y++)
    for (int x = -(int)kChurchHalfWidth; x < (int)kChurchHalfWidth; x++) {
      const int steps = abs(x) < 13 && y >= 20 && y <= 35 ? 2 : 1;
      for (int j = 0; j < steps; j++)
        for (int i = 0; i < steps; i++)
          Floor(v, x + (float)i / steps, y + (float)j / steps, kChurchFloorZ, 1.0f / steps,
                1.0f / steps, 0x635242);
    }
  Box(v, -kChurchHalfWidth, -8, 0, 1, kChurchDoorY + 8, kChurchCeilingZ, 0x635242);
  Box(v, kChurchHalfWidth - 1, -8, 0, 1, kChurchDoorY + 8, kChurchCeilingZ, 0x635242);
  Box(v, -kChurchHalfWidth, -8, kChurchCeilingZ - 1, 2 * kChurchHalfWidth, kChurchDoorY + 9, 1,
      0x312921);
  for (int side = -1; side <= 1; side += 2) {
    Box(v, side < 0 ? 1 - kChurchHalfWidth : kChurchHalfWidth - 1.5f, -8, .1f, .5f,
        kChurchDoorY + 8, .8f, 0x9b9c8a);
  }
  Arch(v);
  /* Three shallow steps locate the unseen throne, immediately below camera. */
  Box(v, -8, -7, .05f, 16, 10, .45f, 0xa4a597);
  Box(v, -7.5f, -7, .5f, 15, 8.7f, .45f, 0xb8b5a0);
  Box(v, -7, -7, .95f, 14, 7.3f, .45f, 0xc4bea5);
}

/* Subdivide broad faces so the lip, mounds and feet can cast shadows onto
 * the same model. The bake consumes the original mesh before any shading. */
static ChurchPoint Bilerp(const ChurchPoint p[4], float x, float y) {
  const float weights[4] = {(1 - x) * (1 - y), x * (1 - y), x * y, (1 - x) * y};
  ChurchPoint result = {0};
  for (int i = 0; i < 4; i++) {
    result.x += p[i].x * weights[i];
    result.y += p[i].y * weights[i];
    result.z += p[i].z * weights[i];
  }
  return result;
}
static int AltarSegments(ChurchPoint a, ChurchPoint b) {
  const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
  return (int)fmaxf(1, ceilf(sqrtf(x * x + y * y + z * z) / .20f));
}

static void AltarQuad(ChurchView *v, const ChurchPoint p[4], const ChurchPoint normals[4],
                      unsigned rgb, float occlusion) {
  ChurchPoint world[4];
  for (int i = 0; i < 4; i++)
    world[i] = (ChurchPoint){p[i].x, p[i].y + kAltarFarY - kAltarHalfDepth, p[i].z};
  if (v->shadow_build) {
    ChurchLighting_AddQuad(world);
    return;
  }
  const int columns = AltarSegments(p[0], p[1]), rows = AltarSegments(p[0], p[3]);
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < columns; x++) {
      const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
      ChurchPoint q[4];
      ArRenderColorF colors[4];
      const unsigned grain = (unsigned)(x * 73856093u) ^ (unsigned)(y * 19349663u) ^ rgb;
      for (int j = 0; j < 4; j++) {
        const float u = (x + dx[j]) / (float)columns, w = (y + dy[j]) / (float)rows;
        q[j] = Bilerp(world, u, w);
        colors[j] = ChurchLighting_Shade(q[j], Bilerp(normals, u, w), Color(rgb), occlusion);
        colors[j] = ChurchDetail_Stone(colors[j], grain);
        colors[j].r *= v->brightness;
        colors[j].g *= v->brightness;
        colors[j].b *= v->brightness;
      }
      LitQuad(v, q, colors);
    }
  }
}

static void AltarFlat(ChurchView *v, const ChurchPoint p[4], ChurchPoint normal, unsigned rgb,
                      float occlusion) {
  const ChurchPoint normals[4] = {normal, normal, normal, normal};
  AltarQuad(v, p, normals, rgb, occlusion);
}

static void AltarTop(ChurchView *v, float x0, float y0, float x1, float y1, float z, unsigned rgb) {
  const ChurchPoint p[4] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
  AltarFlat(v, p, (ChurchPoint){0, 0, 1}, rgb, 1);
}

static void AltarFoot(ChurchView *v, float x, float y) {
  const float z[5] = {kChurchFloorZ, .12f, .36f, .58f, .76f};
  const float radius[5] = {.27f, .43f, .43f, .35f, .30f};
  const float nz[5] = {-.45f, -.15f, .15f, .35f, .55f};
  for (int ring = 0; ring < 4; ring++) {
    for (int side = 0; side < 24; side++) {
      ChurchPoint p[4], normals[4];
      const int ds[4] = {0, 1, 1, 0}, dr[4] = {0, 0, 1, 1};
      for (int j = 0; j < 4; j++) {
        const int r = ring + dr[j];
        const float a = (side + ds[j]) * kChurchPi / 12;
        p[j] = (ChurchPoint){x + radius[r] * cosf(a), y + radius[r] * sinf(a), z[r]};
        normals[j] = (ChurchPoint){cosf(a), sinf(a), nz[r]};
      }
      AltarQuad(v, p, normals, 0xd6c6a5, .76f);
    }
  }
}

/* True recessed front panel: an outer stone frame, sloped reveals and a face
 * set back into the body. The recess is geometry, not a painted rectangle. */
static void AltarFront(ChurchView *v) {
  const float front = -kAltarHalfDepth;
  const float edge = kAltarHalfWidth - .22f, hole = kAltarHalfWidth - .90f;
  const float inner = hole - .22f;
  const ChurchPoint outside[4] = {
      {-edge, front, .82f}, {edge, front, .82f}, {edge, front, 2.28f}, {-edge, front, 2.28f}};
  const ChurchPoint opening[4] = {
      {-hole, front, 1.00f}, {hole, front, 1.00f}, {hole, front, 2.10f}, {-hole, front, 2.10f}};
  const ChurchPoint inset[4] = {{-inner, front + .28f, 1.14f},
                                {inner, front + .28f, 1.14f},
                                {inner, front + .28f, 1.96f},
                                {-inner, front + .28f, 1.96f}};
  const ChurchPoint inward[4] = {{0, -.7f, 1}, {-1, -.7f, 0}, {0, -.7f, -1}, {1, -.7f, 0}};
  for (int edge = 0; edge < 4; edge++) {
    const int next = (edge + 1) % 4;
    const ChurchPoint frame[4] = {outside[edge], outside[next], opening[next], opening[edge]};
    AltarFlat(v, frame, (ChurchPoint){0, -1, 0}, 0xd6c6a5, 1);
    const ChurchPoint reveal[4] = {opening[edge], opening[next], inset[next], inset[edge]};
    AltarFlat(v, reveal, inward[edge], 0xb5a584, .90f);
  }
  AltarFlat(v, inset, (ChurchPoint){0, -1, 0}, 0x8c7b6b, .87f);
}

static void AltarBody(ChurchView *v) {
  enum { kOutline = 36 };
  const float widths[4] = {kAltarHalfWidth - .22f, kAltarHalfWidth, kAltarHalfWidth,
                           kAltarHalfWidth - .18f};
  const float depths[4] = {kAltarHalfDepth - .22f, kAltarHalfDepth, kAltarHalfDepth,
                           kAltarHalfDepth - .18f};
  const float heights[4] = {.60f, .82f, 2.28f, 2.50f};
  const float nz[4] = {-.65f, -.12f, .15f, .75f};
  ChurchPoint points[4][kOutline], normals[4][kOutline];
  for (int ring = 0; ring < 4; ring++) {
    for (int corner = 0; corner < 4; corner++) {
      const float cx = (corner == 0 || corner == 3 ? 1 : -1) * (widths[ring] - .22f);
      const float cy = (corner < 2 ? 1 : -1) * (depths[ring] - .22f);
      for (int step = 0; step <= 8; step++) {
        const float a = (corner + step / 8.0f) * kChurchPi * .5f;
        const int i = corner * 9 + step;
        points[ring][i] = (ChurchPoint){cx + .22f * cosf(a), cy + .22f * sinf(a), heights[ring]};
        normals[ring][i] = (ChurchPoint){cosf(a), sinf(a), nz[ring]};
      }
    }
  }
  for (int ring = 0; ring < 3; ring++) {
    for (int side = 0; side < kOutline; side++) {
      const int next = (side + 1) % kOutline;
      /* The long front wall is supplied by the recessed panel above. */
      if (ring == 1 && side == 26) continue;
      const ChurchPoint p[4] = {points[ring][side], points[ring][next], points[ring + 1][next],
                                points[ring + 1][side]};
      const ChurchPoint n[4] = {normals[ring][side], normals[ring][next], normals[ring + 1][next],
                                normals[ring + 1][side]};
      AltarQuad(v, p, n, 0xd6c6a5, ring == 0 ? .86f : 1);
    }
  }
  AltarFront(v);
}

/* Each end is a continuous rounded stone bolster with rounded longitudinal
 * caps. Analytic normals remove the hard facets from the low-cost mesh. */
static void AltarMound(ChurchView *v, float x) {
  ChurchPoint points[14][25], normals[14][25];
  /* Preserve round end caps and cross-sections when changing the footprint. */
  const float straight = kAltarHalfDepth - .40f - .65f;
  for (int ring = 0; ring < 14; ring++) {
    float y, cap, radius;
    if (ring <= 6) {
      const float a = -kChurchPi * .5f + ring * kChurchPi / 12;
      cap = .65f * sinf(a);
      y = -straight + cap;
      radius = cosf(a);
    } else {
      const float a = (ring - 7) * kChurchPi / 12;
      cap = .65f * sinf(a);
      y = straight + cap;
      radius = cosf(a);
    }
    for (int side = 0; side <= 24; side++) {
      const float a = side * kChurchPi / 24;
      points[ring][side] =
          (ChurchPoint){x + .65f * radius * cosf(a), y, 2.48f + .72f * radius * sinf(a)};
      normals[ring][side] =
          (ChurchPoint){radius * cosf(a) / .65f, cap / (.65f * .65f), radius * sinf(a) / .72f};
    }
  }
  for (int ring = 0; ring < 13; ring++) {
    for (int side = 0; side < 24; side++) {
      const ChurchPoint p[4] = {points[ring][side], points[ring][side + 1],
                                points[ring + 1][side + 1], points[ring + 1][side]};
      const ChurchPoint n[4] = {normals[ring][side], normals[ring][side + 1],
                                normals[ring + 1][side + 1], normals[ring + 1][side]};
      AltarQuad(v, p, n, 0xe7d6b5, 1);
    }
  }
}

static void CarvedAltar(ChurchView *v) {
  const float top_x = kAltarHalfWidth - 1.05f, top_y = kAltarHalfDepth - .40f;
  const float rim_x = kAltarHalfWidth - .18f, rim_y = kAltarHalfDepth - .18f;
  for (int side = -1; side <= 1; side += 2) {
    AltarFoot(v, side * (kAltarHalfWidth - .65f), -kAltarHalfDepth + .48f);
    AltarFoot(v, side * (kAltarHalfWidth - .65f), kAltarHalfDepth - .48f);
  }
  AltarBody(v);
  /* A shallow, gently scooped top joins the raised cream rim. */
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 24; x++) {
      ChurchPoint p[4], n[4];
      const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
      for (int j = 0; j < 4; j++) {
        const float u = (x + dx[j]) / 12.0f - 1, w = (y + dy[j]) / 8.0f - 1;
        const float ex = powf(u, 8), ey = powf(w, 8);
        p[j] = (ChurchPoint){u * top_x, w * top_y, 2.25f + .25f * (ex + ey - ex * ey)};
        n[j] = (ChurchPoint){-2 * powf(u, 7) * (1 - ey) / top_x, -2 * powf(w, 7) * (1 - ex) / top_y,
                             1};
      }
      AltarQuad(v, p, n, 0xd6c6a5, .96f);
    }
  }
  AltarTop(v, -rim_x, -rim_y, -top_x, rim_y, 2.50f, 0xe7d6b5);
  AltarTop(v, top_x, -rim_y, rim_x, rim_y, 2.50f, 0xe7d6b5);
  AltarTop(v, -top_x, -rim_y, top_x, -top_y, 2.50f, 0xe7d6b5);
  AltarTop(v, -top_x, top_y, top_x, rim_y, 2.50f, 0xe7d6b5);
  AltarMound(v, -kAltarHalfWidth + .70f);
  AltarMound(v, kAltarHalfWidth - .70f);
}

/* The reference block has six planar faces and no feet, taper or bevel.
 * Its complete bottom face is exactly coplanar with the room floor. It uses
 * the same projection, diffuse light and shadow pass as the carved altar. */
static void Altar(ChurchView *v) {
  if (!v->options->reference_altar) {
    CarvedAltar(v);
    return;
  }
  const float x = kAltarHalfWidth, y = kAltarHalfDepth;
  const ChurchPoint p[8] = {{-x, -y, kChurchFloorZ}, {x, -y, kChurchFloorZ}, {x, y, kChurchFloorZ},
                            {-x, y, kChurchFloorZ},  {-x, -y, 2.50f},        {x, -y, 2.50f},
                            {x, y, 2.50f},           {-x, y, 2.50f}};
  const int faces[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6},
                           {3, 0, 4, 7}, {4, 5, 6, 7}, {3, 2, 1, 0}};
  const ChurchPoint normals[6] = {{0, -1, 0}, {1, 0, 0}, {0, 1, 0},
                                  {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
  for (int face = 0; face < 6; face++) {
    /* The floor hides the bottom; keep it in the shadow mesh without drawing
     * two coincident faces into the color/depth pass. */
    if (face == 5 && !v->shadow_build) continue;
    ChurchPoint q[4];
    for (int j = 0; j < 4; j++)
      q[j] = p[faces[face][j]];
    AltarFlat(v, q, normals[face], 0xd6c6a5, 1);
  }
}

/* Diagnostic geometry, not a replacement floor material. All guide lines
 * lie on the same horizontal world plane and share the scene's vanishing
 * point. The tiny offset is solely a decal bias, never a contact height. */
static void ReferenceFloor(ChurchView *v) {
  if (!v->options->reference_altar) return;
  const float z = kChurchFloorZ + .003f, half_line = .025f;
  const ArRenderColorF tint = {.16f * v->brightness, .14f * v->brightness, .11f * v->brightness, 1};
  const ArRenderColorF colors[4] = {tint, tint, tint, tint};
  for (int x = -12; x <= 12; x += 4) {
    const ChurchPoint p[4] = {{x - half_line, 12, z},
                              {x + half_line, 12, z},
                              {x + half_line, kChurchDoorY, z},
                              {x - half_line, kChurchDoorY, z}};
    LitQuad(v, p, colors);
  }
  for (int y = 12; y <= 56; y += 4) {
    const ChurchPoint p[4] = {{-12, y - half_line, z},
                              {12, y - half_line, z},
                              {12, y + half_line, z},
                              {-12, y + half_line, z}};
    LitQuad(v, p, colors);
  }
}

static float Terrain(const ChurchView *v, float x, float y) {
#if AR_SIM3D_TERRAIN_ELEVATION
  return ChurchLandscape_Terrain(v->options->landscape, v->town, x, y) * v->landscape / 100;
#else
  (void)v;
  (void)x;
  (void)y;
  return 0;
#endif
}

static bool FindDoor(ChurchView *view, const SimBackgroundVoxelScene *town) {
  if (!town || town->town < 1 || town->town > 6 || town->object_count > kSimBackgroundMaxObjects)
    return false;
  view->town = town->town;
  for (unsigned i = 0; i < town->object_count; i++) {
    const SimBackgroundVoxelObject *o = &town->objects[i];
    if (o->kind != kSimBackgroundVoxel_Cathedral) continue;
    view->origin_x = o->cell_x * 16 + 16;
    view->origin_y = o->cell_y * 16;
    /* Same center-of-plot anchor used by the rigid cathedral model in SIM.
     * The level interior is a foundation floor, not a camera-facing plane. */
    view->origin_z = Terrain(view, view->origin_x, view->origin_y + 16);
    return true;
  }
  return false;
}

static Sim3DDepthMesh *s_ground_mesh;
static ArRenderTexture s_ground_texture;
static uint32_t s_ground_serial;
static float s_ground_landscape;
static uint16_t s_ground_azimuth;
static uint8_t s_ground_elevation, s_ground_style;
static size_t s_ground_quads;
static bool s_shadow_reference;
static Sim3DDepthMesh *s_mountain_mesh, *s_world_mesh;
static ArRenderTexture s_mountain_texture, s_world_texture;
static uint32_t s_landscape_serial;
static float s_landscape_height;
static size_t s_mountain_quads, s_world_quads;
static ArRenderTexture s_door_bloom[3];
static int s_door_bloom_width, s_door_bloom_height;

static void ResetDoorBloom(ArRenderDevice *device) {
  for (int i = 0; i < 3; i++) {
    ArRenderDevice_DestroyTexture(device, s_door_bloom[i]);
    s_door_bloom[i] = ArRenderTexture_Invalid();
  }
  s_door_bloom_width = s_door_bloom_height = 0;
}

void ChurchScene_Reset(ArRenderDevice *device) {
  s_stats = (ChurchSceneStats){0};
  ChurchLighting_Reset();
  for (int i = 0; i < 2; i++) {
    Sim3DDepthPass_DestroyMesh(s_room_mesh[i]);
    s_room_mesh[i] = NULL;
    ArRenderDevice_DestroyTexture(device, s_room_image[i]);
    s_room_image[i] = ArRenderTexture_Invalid();
  }
  Sim3DDepthPass_DestroyMesh(s_exterior_mesh);
  s_exterior_mesh = NULL;
  s_room_images_valid = false;
  s_room_width = s_room_height = 0;
  for (int i = 0; i < 3; i++) {
    Sim3DDepthPass_DestroyMesh(s_foliage_mesh[i]);
    s_foliage_mesh[i] = NULL;
  }
  ArRenderDevice_DestroyTexture(device, s_white_texture);
  s_white_texture = ArRenderTexture_Invalid();
  s_detail_valid = false;
  s_focus_building = false;
  ResetDoorBloom(device);
  Sim3DDepthPass_DestroyMesh(s_mountain_mesh);
  Sim3DDepthPass_DestroyMesh(s_world_mesh);
  s_mountain_mesh = s_world_mesh = NULL;
  ArRenderDevice_DestroyTexture(device, s_mountain_texture);
  ArRenderDevice_DestroyTexture(device, s_world_texture);
  s_mountain_texture = s_world_texture = ArRenderTexture_Invalid();
  s_landscape_serial = 0;
  Sim3DDepthPass_DestroyMesh(s_ground_mesh);
  s_ground_mesh = NULL;
  ArRenderDevice_DestroyTexture(device, s_ground_texture);
  s_ground_texture = ArRenderTexture_Invalid();
  s_ground_serial = 0;
}

static bool AppendSurface(const ChurchView *v, Sim3DDepthMesh *mesh, ArRenderTexture texture,
                          size_t count, Sim3DDepthPassLayer layer) {
  if (!count) return true;
  Sim3DDepthSurfaceBatch batch = {
      .layer = layer,
      .texture = texture,
      .range = {0, count},
      .transform = {.radial = {.basis = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                               .sphere_radius = 1,
                               .height_scale = 1},
                    .extra_scale = 1,
                    .light = {0, 0, 1},
                    .ambient = v->brightness}};
  const Scene3DClipPoint origin = Project(v, (ChurchPoint){0, 0, 0});
  const ChurchPoint axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int axis = 0; axis < 3; axis++) {
    const Scene3DClipPoint c = Project(v, axes[axis]);
    float *column = batch.transform.radial.matrix + axis * 4;
    column[0] = c.x - origin.x;
    column[1] = c.y - origin.y;
    column[2] = c.z - origin.z;
    column[3] = c.w - origin.w;
  }
  float *translation = batch.transform.radial.matrix + 12;
  translation[0] = origin.x;
  translation[1] = origin.y;
  translation[2] = origin.z;
  translation[3] = origin.w;
  return Sim3DDepthPass_AppendSurfaceBatches(mesh, &batch, 1);
}

/* Borrow a private copy of the actual SIM ground atlas. Retained textured
 * surfaces preserve perspective-correct UVs and hardware depth; no downsampled
 * colors, palette conversion, or mutation of the SIM renderer's atlas cache. */
static bool Ground(ArRenderDevice *device, const ChurchView *v, const SimBackgroundVoxelScene *town,
                   const uint32_t *pixels) {
  if (!pixels) return true;
  if (!s_ground_mesh) s_ground_mesh = Sim3DDepthPass_CreateSurfaceMesh();
  if (!s_ground_mesh) return false;
  if (!ArRenderTexture_IsValid(s_ground_texture)) {
    const ArRenderTextureDesc desc = {.width = 512,
                                      .height = 512,
                                      .format = kArRenderPixelFormat_Argb8888,
                                      .usage = kArRenderTextureUsage_Streaming,
                                      .filter = kArRenderFilter_Nearest,
                                      .blend = kArRenderBlendMode_Opaque};
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_ground_texture)) return false;
  }
  if (!Sim3DDepthPass_MeshReady(s_ground_mesh) || !s_ground_serial ||
      s_ground_serial != v->options->town_serial || s_ground_landscape != v->landscape ||
      s_ground_azimuth != v->options->light_azimuth_deg ||
      s_ground_elevation != v->options->light_elevation_deg ||
      s_ground_style != v->options->style) {
    Sim3DDepthSurfaceVertex *vertices = malloc(32 * 32 * 4 * sizeof(*vertices));
    if (!vertices) return false;
    const int dx[4] = {0, 16, 16, 0}, dy[4] = {0, 0, 16, 16};
    size_t count = 0;
    for (int y = 0; y < 512; y += 16) {
      if (y < v->origin_y + 32) continue;
      for (int x = 0; x < 512; x += 16) {
        ChurchPoint focus_points[4];
        for (int j = 0; j < 4; j++) {
          const float tx = (float)(x + dx[j]), ty = (float)(y + dy[j]);
          const ChurchPoint p = {v->origin_x - tx, ty - v->origin_y + kChurchTownOffsetY,
                                 Terrain(v, tx, ty) - v->origin_z};
          focus_points[j] = p;
          /* Encode an arbitrary Cartesian point in the shared radial surface
           * adapter: unit direction plus radius exactly reconstructs p. */
          const float z = p.z + 1;
          const float radius = sqrtf(p.x * p.x + p.y * p.y + z * z);
          vertices[count++] =
              (Sim3DDepthSurfaceVertex){.normal = {p.x / radius, p.y / radius, z / radius},
                                        .elevation = {radius - 1, 0},
                                        .shade_normal = {0, 0, 1},
                                        .color = {1, 1, 1, 1},
                                        .uv = {tx / 512, ty / 512}};
        }
        if (!ExteriorVisible(v, focus_points)) {
          count -= 4;
          continue;
        }
        FocusQuad(v, focus_points);
      }
    }
    uint32_t *shaded = malloc(512 * 512 * sizeof(*shaded));
    const bool valid =
        shaded && ChurchExterior_BakeGround(town, v->options, pixels, shaded) &&
        Sim3DDepthPass_UpdateSurfaceMesh(s_ground_mesh, vertices, count / 4) &&
        ArRenderDevice_UpdateTexture(device, s_ground_texture, NULL, shaded, 512 * 4);
    free(shaded);
    free(vertices);
    if (!valid) return false;
    s_ground_quads = count / 4;
    s_ground_serial = v->options->town_serial;
    s_ground_landscape = v->landscape;
    s_ground_azimuth = v->options->light_azimuth_deg;
    s_ground_elevation = v->options->light_elevation_deg;
    s_ground_style = v->options->style;
  }
  return AppendSurface(v, s_ground_mesh, s_ground_texture, s_ground_quads, kSim3DDepthPass_Ground);
}

static Sim3DDepthSurfaceVertex SurfacePoint(ChurchPoint p, ArRenderColorF color,
                                            ArRenderPointF uv) {
  const float z = p.z + 1, radius = sqrtf(p.x * p.x + p.y * p.y + z * z);
  return (Sim3DDepthSurfaceVertex){.normal = {p.x / radius, p.y / radius, z / radius},
                                   .elevation = {radius - 1, 0},
                                   .shade_normal = {0, 0, 1},
                                   .color = color,
                                   .uv = uv};
}

typedef struct ChurchMountainBuilder {
  const ChurchView *view;
  Sim3DDepthSurfaceVertex *vertices;
  size_t count, capacity;
  bool failed;
} ChurchMountainBuilder;

static void MountainFace(void *user, const float x[4], const float y[4], const float z[4],
                         const SimBackgroundProjectionAxis *axis,
                         const SimBackgroundMountainMeshUV uv[4], const uint8_t brightness[4],
                         const uint8_t alpha[4]) {
  ChurchMountainBuilder *b = user;
  if (b->failed) return;
  if (b->count == b->capacity) {
    size_t capacity = b->capacity ? b->capacity * 2 : 1024;
    if (capacity > 131072) {
      b->failed = true;
      return;
    }
    void *vertices = realloc(b->vertices, capacity * 4 * sizeof(*b->vertices));
    if (!vertices) {
      b->failed = true;
      return;
    }
    b->vertices = vertices;
    b->capacity = capacity;
  }
  const ChurchView *v = b->view;
  ChurchPoint points[4];
  for (int i = 0; i < 4; i++) {
    const ChurchPoint p = {v->origin_x - x[i] - z[i] * axis->x_per_height,
                           y[i] + z[i] * axis->y_per_height - v->origin_y + kChurchTownOffsetY,
                           Terrain(v, x[i], y[i]) + z[i] * axis->height_scale - v->origin_z};
    const float shade = brightness[i] / 255.0f;
    points[i] = p;
    b->vertices[b->count * 4 + i] =
        SurfacePoint(p, (ArRenderColorF){shade, shade, shade, alpha[i] / 255.0f},
                     (ArRenderPointF){uv[i].x, uv[i].y});
  }
  if (!ExteriorVisible(v, points)) return;
  FocusQuad(v, points);
  b->count++;
}

static bool LandscapeTexture(ArRenderDevice *device, ArRenderTexture *texture, int size,
                             const uint32_t *pixels) {
  const ArRenderTextureDesc desc = {.width = size,
                                    .height = size,
                                    .format = kArRenderPixelFormat_Argb8888,
                                    .usage = kArRenderTextureUsage_Streaming,
                                    .filter = kArRenderFilter_Nearest,
                                    .blend = kArRenderBlendMode_Alpha};
  return (ArRenderTexture_IsValid(*texture) ||
          ArRenderDevice_CreateTexture(device, &desc, texture)) &&
         ArRenderDevice_UpdateTexture(device, *texture, NULL, pixels, size * 4);
}

static bool PrepareLandscape(ArRenderDevice *device, const ChurchView *v,
                             const SimBackgroundVoxelScene *town) {
  const ChurchLandscape *landscape = v->options->landscape;
  if (!landscape) {
    s_world_quads = s_mountain_quads = 0;
    s_landscape_serial = 0;
    return !town->mountains.cell_count;
  }
  if (s_landscape_serial == v->options->town_serial && s_landscape_serial &&
      s_landscape_height == v->landscape)
    return true;
  /* Express the church camera in the native SIM normalized world convention
   * so the shared mountain recipe resolves the correct lean/stack direction. */
  const ChurchPoint native_origin = {v->origin_x - 256, 256 - v->origin_y + kChurchTownOffsetY,
                                     -v->origin_z};
  const Scene3DClipPoint c = Project(v, native_origin);
  const ChurchPoint axes[3] = {{-512 / v->aspect, 0, 0}, {0, -512, 0}, {0, 0, 512}};
  float matrix[16];
  for (int i = 0; i < 3; i++) {
    const Scene3DClipPoint p =
        Project(v, (ChurchPoint){native_origin.x + axes[i].x, native_origin.y + axes[i].y,
                                 native_origin.z + axes[i].z});
    matrix[i * 4] = p.x - c.x;
    matrix[i * 4 + 1] = p.y - c.y;
    matrix[i * 4 + 2] = p.z - c.z;
    matrix[i * 4 + 3] = p.w - c.w;
  }
  matrix[12] = c.x;
  matrix[13] = c.y;
  matrix[14] = c.z;
  matrix[15] = c.w;
  const SimBackgroundVoxelRenderParams params = {.town = town->town,
                                                 .matrix = matrix,
                                                 .source = {0, 0, 512, 512},
                                                 .viewport = v->viewport,
                                                 .detail = kSimBackgroundVoxelDetail_Ultra,
                                                 .lod = kSimBackgroundVoxelLod_Fixed,
                                                 .facing = kSimBackgroundVoxelFacing_PerModel};
  ChurchMountainBuilder b = {.view = v};
  const int count = SimBackgroundMountainRender_EmitSnapshot(
      &params, town, landscape->mountain_atlas, landscape->mountain_sources, MountainFace, &b);
  bool valid = count >= 0 && !b.failed;
  if (valid && b.count) {
    if (!s_mountain_mesh) s_mountain_mesh = Sim3DDepthPass_CreateSurfaceMesh();
    valid = s_mountain_mesh &&
            Sim3DDepthPass_UpdateSurfaceMesh(s_mountain_mesh, b.vertices, b.count) &&
            LandscapeTexture(device, &s_mountain_texture, 512, landscape->mountain_atlas);
  }
  s_mountain_quads = b.count;
  free(b.vertices);
  if (!valid) return false;
  s_world_quads = 0;
  if (landscape->world_valid) {
    Sim3DDepthSurfaceVertex *vertices = malloc(128 * 128 * 4 * sizeof(*vertices));
    if (!vertices) return false;
    const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
    for (int y = 0; y < 128; y++) {
      for (int x = 0; x < 128; x++) {
        if (x >= landscape->origin_x && x < landscape->origin_x + 32 && y >= landscape->origin_y &&
            y < landscape->origin_y + 32)
          continue;
        ChurchPoint points[4];
        for (int i = 0; i < 4; i++) {
          const int wx = x + dx[i], wy = y + dy[i];
          const float tx = (wx - landscape->origin_x) * 16;
          const float ty = (wy - landscape->origin_y) * 16;
          const float height = landscape->world_heights[wy * 129 + wx] * 16 * v->landscape / 100;
          const ChurchPoint p = {v->origin_x - tx, ty - v->origin_y + kChurchTownOffsetY,
                                 height - v->origin_z};
          points[i] = p;
          vertices[s_world_quads * 4 + i] = SurfacePoint(
              p, (ArRenderColorF){1, 1, 1, 1}, (ArRenderPointF){wx / 128.0f, wy / 128.0f});
        }
        if (!ExteriorVisible(v, points)) continue;
        FocusQuad(v, points);
        s_world_quads++;
      }
    }
    if (!s_world_mesh) s_world_mesh = Sim3DDepthPass_CreateSurfaceMesh();
    valid = s_world_mesh &&
            Sim3DDepthPass_UpdateSurfaceMesh(s_world_mesh, vertices, s_world_quads) &&
            LandscapeTexture(device, &s_world_texture, 1024, landscape->world_pixels);
    free(vertices);
    if (!valid) return false;
  }
  s_landscape_serial = v->options->town_serial;
  s_landscape_height = v->landscape;
  return true;
}

static bool Landscape(ArRenderDevice *device, ChurchView *v, const SimBackgroundVoxelScene *town) {
  if (!PrepareLandscape(device, v, town)) return false;
  const ChurchLandscape *landscape = v->options->landscape;
  if (landscape && landscape->world_valid) {
    /* The world-map coast stays textured above the SIM ocean's sea datum.
     * Extend uncharted water to the horizon, never up to the temple floor. */
    const float sea = -landscape->datum * 16 * v->landscape / 100 - v->origin_z - .1f;
    const ChurchPoint p[4] = {
        {-6000, 0, sea}, {6000, 0, sea}, {6000, 5800, sea}, {-6000, 5800, sea}};
    Quad(v, p, (ArRenderColorF){.05f * .94f, .15f * .94f, .84f * .94f, 1});
  }
  return v->valid &&
         AppendSurface(v, s_world_mesh, s_world_texture, s_world_quads, kSim3DDepthPass_Ground) &&
         AppendSurface(v, s_mountain_mesh, s_mountain_texture, s_mountain_quads,
                       kSim3DDepthPass_WorldMountain);
}

static void Town(ChurchView *v, const SimBackgroundVoxelScene *town, bool moving) {
  for (unsigned i = 0; i < town->object_count; i++) {
    const SimBackgroundVoxelObject *o = &town->objects[i];
    if (o->kind == kSimBackgroundVoxel_Cathedral || o->cell_y * 16 < v->origin_y + 30) continue;
    bool animated = false;
    for (int j = 0; j < s_moving_tree_count; j++)
      if (s_moving_trees[j] == (int)i) animated = true;
    if (moving != animated) continue;
    /* Always use the highest model detail, independent of town LOD settings. */
    const SimBackgroundVoxelModelShadingKey lighting = {
        .light_azimuth_deg = v->options->light_azimuth_deg,
        .light_elevation_deg = v->options->light_elevation_deg,
        .shading = v->options->shading,
        .biome = SimBackgroundVoxelBiome_ForTown(o->town),
    };
    const SimBackgroundVoxelModelShading *shading;
    const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
        o, kSimBackgroundVoxelDetail_Ultra, (SimBackgroundVoxelStyle)v->options->style, &lighting,
        &shading);
    if (!model || model->overflow || !shading) {
      v->valid = false;
      return;
    }
    SimBackgroundVoxelPalette palette;
    SimBackgroundVoxelPalette_Build(o, SimBackgroundVoxelBiome_ForTown(o->town), &palette);
    SimBackgroundBridgeBounds bounds = {
        o->cell_x * 16.0f, (o->cell_y + o->source_cells_h - o->footprint_cells_d) * 16.0f,
        o->footprint_cells_w * 16.0f, o->footprint_cells_d * 16.0f};
    if (o->kind == kSimBackgroundVoxel_Bridge) bounds = SimBackgroundBridge_ResolveBounds(o);
    const float cx = bounds.width * .5f, cy = bounds.depth * .5f;
    const SimBackgroundVoxelProportions *proportions =
        SimBackgroundVoxelProportions_Get((SimBackgroundVoxelKind)o->kind);
    const float base = Terrain(v, bounds.origin_x + cx, bounds.origin_y + cy) - v->origin_z;
    for (unsigned f = 0; f < model->face_count; f++) {
      const SimBackgroundVoxelModelFace *face = &model->faces[f];
      ChurchPoint p[4];
      for (int j = 0; j < 4; j++)
        p[j] = (ChurchPoint){v->origin_x - bounds.origin_x - cx -
                                 (face->points[j].x - cx) * proportions->footprint_scale,
                             bounds.origin_y + cy +
                                 (face->points[j].y - cy) * proportions->footprint_scale -
                                 v->origin_y + kChurchTownOffsetY,
                             base + face->points[j].z * proportions->height_scale};
      if (moving)
        for (int j = 0; j < 4; j++) {
          const float crown = fminf(1, fmaxf(0, (p[j].z - base - 3) / 12));
          const float phase = v->foliage_pose == 1 ? .24f : v->foliage_pose == 2 ? -.24f : 0;
          p[j].x += phase * crown * crown;
        }
      ArRenderColorF colors[4];
      SimBackgroundVoxelProject_FaceColors(shading->material[f], shading->brightness[f], &palette,
                                           (SimBackgroundVoxelShading)v->options->shading, colors);
      QuadColors(v, p, colors);
    }
  }
}

/* Native eight-tick water frames, presented as sparse low-contrast glints.
 * Exactly bounded: at most 192 quads, no uploads or new render targets. Land
 * and mixed coast cells are excluded using the captured world's water mask. */
static bool Water(ChurchView *v) {
  const ChurchLandscape *l = v->options->landscape;
  if (!l || !l->world_valid || !l->water_valid) return true;
  const uint32_t *frame = l->water_frames[ChurchDetail_Phase(v->seconds * 7.5, 4, 1)];
  unsigned minimum = 765;
  for (int i = 0; i < 64; i++) {
    const uint32_t c = frame[i];
    unsigned value = ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
    if (value < minimum) minimum = value;
  }
  unsigned quads = 0;
  for (int row = 0; row < 8; row++)
    for (int column = -2; column <= 2; column++) {
      const float distance = 80 + row * row * 20;
      const float cx = column * distance * .036f;
      const float tx = v->origin_x - cx, ty = distance + v->origin_y - kChurchTownOffsetY;
      const int wx = (int)floorf(tx / 16) + l->origin_x, wy = (int)floorf(ty / 16) + l->origin_y;
      float height = -l->datum;
      if (wx >= 0 && wx < 128 && wy >= 0 && wy < 128) {
        if (!l->open_water[wy * 128 + wx]) continue;
        const float u = tx / 16 - floorf(tx / 16), w = ty / 16 - floorf(ty / 16);
        const float *h = l->world_heights + wy * 129 + wx;
        height = (h[0] * (1 - u) + h[1] * u) * (1 - w) + (h[129] * (1 - u) + h[130] * u) * w;
      }
      const float z = height * 16 * v->landscape / 100 - v->origin_z + .2f;
      const float scale = fminf(1.2f, distance * .003f);
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
          const uint32_t c = frame[y * 8 + x];
          const unsigned value = ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255);
          if (value <= minimum + 24 || ((x + y + column) & 1)) continue;
          if (quads++ == 192) return true;
          const float px = cx + (x - 4) * scale, py = distance + (y - 4) * scale;
          const ChurchPoint corners[4] = {
              {px, py, z}, {px + scale, py, z}, {px + scale, py + scale, z}, {px, py + scale, z}};
          Sim3DDepthVertex vertices[4];
          for (int j = 0; j < 4; j++) {
            Scene3DClipPoint p = Project(v, corners[j]);
            ArRenderColorF color = Color(c);
            color.r *= v->brightness;
            color.g *= v->brightness;
            color.b *= v->brightness;
            color.a = .20f;
            vertices[j] = (Sim3DDepthVertex){(p.x / p.w + 1) * v->viewport.w * .5f,
                                             (1 - p.y / p.w) * v->viewport.h * .5f,
                                             (p.z / p.w + 1) * .5f,
                                             color,
                                             {-1, -1}};
          }
          if (!Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Effect, vertices)) return false;
        }
    }
  return true;
}

static bool DetailChanged(const ChurchView *v, const SimBackgroundVoxelScene *town) {
  const ChurchSceneOptions *o = v->options, *p = &s_detail_options;
  return !s_detail_valid || (s_exterior_quads && !Sim3DDepthPass_MeshReady(s_exterior_mesh)) ||
         s_detail_town != town || s_detail_serial != o->town_serial ||
         p->landscape_height_pct != o->landscape_height_pct || p->style != o->style ||
         p->shading != o->shading || p->light_azimuth_deg != o->light_azimuth_deg ||
         p->light_elevation_deg != o->light_elevation_deg ||
         p->reference_altar != o->reference_altar ||
         s_detail_columns != ArRenderTexture_IsValid(o->people);
}
static void BeginDetails(ChurchView *v, const SimBackgroundVoxelScene *town) {
  s_detail_valid = false;
  s_room_images_valid = false;
  s_focus_building = true;
  s_ground_serial = s_landscape_serial = 0;
  Scene3DClipPoint a = Project(v, (ChurchPoint){-5, kChurchDoorY + 3, 0});
  Scene3DClipPoint b = Project(v, (ChurchPoint){5, kChurchDoorY + 3, 15});
  ChurchFocus_Begin(&s_focus, a.x / a.w * v->aspect, b.x / b.w * v->aspect, a.y / a.w, b.y / b.w);
  s_moving_tree_count = 0;
  for (unsigned i = 0; i < town->object_count && s_moving_tree_count < 8; i++) {
    const SimBackgroundVoxelObject *o = &town->objects[i];
    if (o->kind != kSimBackgroundVoxel_Tree || o->cell_y * 16 < v->origin_y + 32) continue;
    const float distance = o->cell_y * 16 - v->origin_y + kChurchTownOffsetY;
    if (distance > 250 || fabsf(o->cell_x * 16 - v->origin_x) > distance * .11f + 18) continue;
    s_moving_trees[s_moving_tree_count++] = (int)i;
  }
}
static bool PublishGeometry(Sim3DDepthMesh **mesh, ChurchGeometry *b) {
  if (!b->count) return true;
  if (!*mesh) *mesh = Sim3DDepthPass_CreateSurfaceMesh();
  return *mesh && Sim3DDepthPass_UpdateSurfaceMesh(*mesh, b->vertices, b->count);
}
static void PrepareActorLighting(void) {
  for (int actor = 0; actor < 2; actor++)
    for (int y = 0; y <= kActorRows; y++)
      for (int x = 0; x <= kActorColumns; x++) {
        const float across = 2.0f * x / kActorColumns - 1;
        const ChurchPoint p = {(actor ? kActorX : -kActorX) + across * kActorWidth * .5f, kActorY,
                               (1 - y / (float)kActorRows) * kActorHeight};
        s_actor_tint[actor][y][x] = ChurchLighting_ActorTint(p, across);
      }
}

static bool PrepareDetails(ArRenderDevice *device, ChurchView *v,
                           const SimBackgroundVoxelScene *town) {
  ChurchGeometry b = {0}, near = {0};
  ChurchView build = *v;
  build.geometry = &b;
  build.brightness = 1;
  build.interior = false;
  Town(&build, town, false);
  bool valid = build.valid && PublishGeometry(&s_exterior_mesh, &b);
  s_exterior_quads = b.count;
  b.count = 0;
  build.interior = true;
  build.foreground = &near;
  Interior(&build);
  ReferenceFloor(&build);
  Altar(&build);
  valid = valid && build.valid && PublishGeometry(&s_room_mesh[0], &b) &&
          PublishGeometry(&s_room_mesh[1], &near);
  s_room_quads[0] = b.count;
  s_room_quads[1] = near.count;
  free(near.vertices);
  build.foreground = NULL;
  for (int pose = 0; pose < 3 && valid; pose++) {
    b.count = 0;
    build.interior = false;
    build.foliage_pose = pose;
    Town(&build, town, true);
    valid = build.valid && PublishGeometry(&s_foliage_mesh[pose], &b);
    s_foliage_quads[pose] = b.count;
    s_focus_building = false;
  }
  free(b.vertices);
  if (!ArRenderTexture_IsValid(s_white_texture)) {
    const uint32_t white = 0xffffffff;
    const ArRenderTextureDesc desc = {.width = 1,
                                      .height = 1,
                                      .format = kArRenderPixelFormat_Argb8888,
                                      .usage = kArRenderTextureUsage_Streaming,
                                      .filter = kArRenderFilter_Nearest,
                                      .blend = kArRenderBlendMode_Opaque};
    valid = valid && ArRenderDevice_CreateTexture(device, &desc, &s_white_texture) &&
            ArRenderDevice_UpdateTexture(device, s_white_texture, NULL, &white, 4);
  }
  if (valid) {
    PrepareActorLighting();
    s_detail_options = *v->options;
    s_detail_town = town;
    s_detail_serial = v->options->town_serial;
    s_detail_columns = ArRenderTexture_IsValid(v->options->people);
    s_detail_valid = true;
    s_stats.geometry_builds++;
    s_stats.moving_trees = s_moving_tree_count;
    s_stats.exterior_quads = s_exterior_quads + s_world_quads + s_mountain_quads + s_ground_quads;
  }
  return valid;
}

/* One small batch for feathered light. Nothing has a hard polygon edge:
 * both the floor pool and the implied high-window shaft reach zero smoothly. */
typedef struct ChurchGlowBatch {
  ArRenderVertex2D vertices[4096];
  int32_t indices[4096];
  int count;
} ChurchGlowBatch;

static bool Glow(ChurchGlowBatch *batch, const ChurchView *v, const ChurchPoint p[4],
                 ArRenderColorF color, const float alpha[4]) {
  Scene3DClipPoint clip[4];
  for (int i = 0; i < 4; i++)
    clip[i] = Project(v, p[i]);
  const int halves[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (int half = 0; half < 2; half++) {
    Scene3DClipPoint triangle[3];
    for (int j = 0; j < 3; j++)
      triangle[j] = clip[halves[half][j]];
    Scene3DClippedPolygon polygon;
    if (!Scene3D_ClipTriangle(triangle, &polygon)) return false;
    for (int i = 1; i + 1 < polygon.count; i++) {
      const int at[3] = {0, i, i + 1};
      if (batch->count + 3 > 4096) return false;
      for (int j = 0; j < 3; j++) {
        const Scene3DClippedVertex *vertex = &polygon.vertices[at[j]];
        Scene3DClipPoint c = vertex->point;
        ArRenderColorF tint = color;
        tint.a = 0;
        for (int k = 0; k < 3; k++)
          tint.a += vertex->weights[k] * alpha[halves[half][k]];
        tint.r *= v->brightness;
        tint.g *= v->brightness;
        tint.b *= v->brightness;
        batch->indices[batch->count] = batch->count;
        batch->vertices[batch->count++] = (ArRenderVertex2D){
            {(c.x / c.w + 1) * v->viewport.w * .5f, (1 - c.y / c.w) * v->viewport.h * .5f},
            tint,
            {0, 0}};
      }
    }
  }
  return true;
}

static float Feather(float radius) {
  const float edge = fmaxf(0, 1 - radius * radius);
  return expf(-3 * radius * radius) * edge * edge;
}

/* Three soft panes break the shaft into rays without sharp-edged stripes. */
static float WindowBeamProfile(float u) {
  const float edge = fmaxf(0, 1 - u * u);
  const float center = .55f * expf(-u * u / .09f);
  const float left = .32f * expf(-(u + .60f) * (u + .60f) / .07f);
  const float right = .32f * expf(-(u - .60f) * (u - .60f) / .07f);
  return edge * edge * (.15f + center + left + right);
}

static bool SubmitGlow(ArRenderDevice *device, ChurchGlowBatch *batch, ArRenderBlendMode blend) {
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blend};
  const bool valid = !batch->count || ArRenderDevice_DrawGeometryWithState(
                                          device, ArRenderTexture_Invalid(), batch->vertices,
                                          batch->count, batch->indices, batch->count, &state);
  batch->count = 0;
  return valid;
}

static bool Light(ArRenderDevice *device, const ChurchView *v) {
  ChurchGlowBatch batch = {0};
  /* The offscreen high window is divided into softly separated panes. The
   * wide penumbra and longitudinal fade avoid a solid triangle of light. */
  for (int row = 0; row < 10; row++) {
    for (int strip = 0; strip < 32; strip++) {
      ChurchPoint beam[4];
      float alpha[4];
      const int du[4] = {0, 1, 1, 0}, dt[4] = {0, 0, 1, 1};
      for (int j = 0; j < 4; j++) {
        const float u = (strip + du[j]) / 16.0f - 1;
        const float t = (row + dt[j]) / 10.0f;
        const float along = sinf(t * kChurchPi);
        beam[j] = ChurchLighting_WindowPoint(u, t);
        beam[j].z += .1f;
        alpha[j] = .23f * WindowBeamProfile(u) * along *
                   ChurchLighting_Visibility(kChurchLight_Window, beam[j], (ChurchPoint){0, 0, 1});
      }
      if (!Glow(&batch, v, beam, Color(0xffe7b5), alpha)) return false;
    }
  }
  /* Cooler, weaker daylight enters the far doorway and falls diagonally
   * into the empty aisle. It never replaces the warm focus on the people. */
  for (int row = 0; row < 6; row++) {
    for (int strip = 0; strip < 12; strip++) {
      ChurchPoint beam[4];
      float alpha[4];
      const int du[4] = {0, 1, 1, 0}, dt[4] = {0, 0, 1, 1};
      for (int j = 0; j < 4; j++) {
        const float u = (strip + du[j]) / 6.0f - 1;
        const float t = (row + dt[j]) / 6.0f;
        const float along = sinf(t * kChurchPi);
        beam[j] =
            (ChurchPoint){u * (4 + 3 * t) - 3 * t, kChurchDoorY - .8f - 32 * t, 11 * (1 - t) + .1f};
        alpha[j] = .045f * Feather(u) * along * along *
                   ChurchLighting_Visibility(kChurchLight_Door, beam[j], (ChurchPoint){0, 0, 1});
      }
      if (!Glow(&batch, v, beam, Color(0xaccfff), alpha)) return false;
    }
  }
  return SubmitGlow(device, &batch, kArRenderBlendMode_Alpha);
}

static bool ScreenTriangle(ChurchGlowBatch *batch, ArRenderVertex2D a, ArRenderVertex2D b,
                           ArRenderVertex2D c) {
  if (batch->count + 3 > 4096) return false;
  const ArRenderVertex2D vertices[3] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    batch->indices[batch->count] = batch->count;
    batch->vertices[batch->count++] = vertices[i];
  }
  return true;
}

/* Gaussian rings provide a restrained glow; the source sprites stay crisp. */
static bool Halo(ChurchGlowBatch *batch, const ChurchView *v, float x, float y, float radius_x,
                 float radius_y, float opacity, unsigned rgb, int rings, int segments) {
  for (int ring = 0; ring < rings; ring++) {
    const float inner = (float)ring / rings, outer = (float)(ring + 1) / rings;
    for (int segment = 0; segment < segments; segment++) {
      const float angles[4] = {
          segment * 2 * kChurchPi / segments, (segment + 1) * 2 * kChurchPi / segments,
          (segment + 1) * 2 * kChurchPi / segments, segment * 2 * kChurchPi / segments};
      ArRenderVertex2D points[4];
      for (int j = 0; j < 4; j++) {
        const float radius = j < 2 ? inner : outer;
        ArRenderColorF color = Color(rgb);
        color.r *= v->brightness;
        color.g *= v->brightness;
        color.b *= v->brightness;
        color.a = opacity * Feather(radius);
        points[j] = (ArRenderVertex2D){
            {x + cosf(angles[j]) * radius * radius_x, y + sinf(angles[j]) * radius * radius_y},
            color,
            {0, 0}};
      }
      /* The inner ring is a fan, so omit its collapsed first triangle. */
      if (ring && !ScreenTriangle(batch, points[0], points[1], points[2])) return false;
      if (!ScreenTriangle(batch, points[0], points[2], points[3])) return false;
    }
  }
  return true;
}

static float MoteSeed(uint32_t seed) {
  seed ^= seed >> 16;
  seed *= 0x7feb352du;
  seed ^= seed >> 15;
  seed *= 0x846ca68bu;
  seed ^= seed >> 16;
  return (seed & 0xffffffu) / 16777216.0f;
}

static bool DustAndGlow(ArRenderDevice *device, const ChurchView *v) {
  ChurchGlowBatch batch = {0};
  const Scene3DClipPoint pool = Project(v, (ChurchPoint){0, 31, 2.5f});
  const float pool_x = (pool.x / pool.w + 1) * v->viewport.w * .5f;
  const float pool_y = (1 - pool.y / pool.w) * v->viewport.h * .5f;
  if (!Halo(&batch, v, pool_x, pool_y, v->viewport.h * .22f, v->viewport.h * .18f, .055f, 0xffe1ad,
            6, 24))
    return false;

  /* Stateless, deterministic drifting dust in the high-window light volume.
   * Fade around each wrap; never use gameplay RNG or frame-to-frame flicker. */
  for (unsigned i = 0; i < 96; i++) {
    const float seed = MoteSeed(i * 7 + 1), side = MoteSeed(i * 7 + 2) * 2 - 1;
    double cycle = seed + v->seconds * (.024 + .014 * MoteSeed(i * 7 + 3));
    const float age = (float)(cycle - floor(cycle));
    const float along = .42f + age * .56f;
    const float drift = (float)fmod(v->seconds * .35 + seed * 30, 2 * kChurchPi);
    const float bob = (float)fmod(v->seconds * .455 + seed * 39, 2 * kChurchPi);
    ChurchPoint p = ChurchLighting_WindowPoint(side, along);
    p.x += .35f * sinf(drift);
    p.y += (MoteSeed(i * 7 + 4) - .5f) * 7;
    p.z += .3f * cosf(bob);
    const Scene3DClipPoint point = Project(v, p);
    if (point.w <= .4f || fabsf(point.x) > point.w || fabsf(point.y) > point.w) continue;
    const float fade = sinf(age * kChurchPi);
    const float opacity =
        (.28f + .40f * MoteSeed(i * 7 + 5)) * WindowBeamProfile(side) * fade * fade;
    const float x = (point.x / point.w + 1) * v->viewport.w * .5f;
    const float y = (1 - point.y / point.w) * v->viewport.h * .5f;
    const float screen_scale = v->viewport.h / 896.0f;
    const float radius =
        fmaxf(.45f * screen_scale,
              fminf(1.3f * screen_scale, (.022f + seed * .018f) * v->viewport.h / point.w));
    if (batch.count + 48 > 4096 && !SubmitGlow(device, &batch, kArRenderBlendMode_Add))
      return false;
    if (!Halo(&batch, v, x, y, radius * 3.2f, radius * 3.2f, opacity * .16f, 0xffdba0, 1, 8) ||
        !Halo(&batch, v, x, y, radius, radius, opacity, 0xfff0d2, 1, 8))
      return false;
  }
  return SubmitGlow(device, &batch, kArRenderBlendMode_Add);
}

static bool Vignette(ArRenderDevice *device, const ChurchView *v) {
  ChurchGlowBatch batch = {0};
  for (int y = 0; y < 12; y++) {
    for (int x = 0; x < 16; x++) {
      ArRenderVertex2D points[4];
      const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
      for (int j = 0; j < 4; j++) {
        const float sx = (x + dx[j]) / 16.0f, sy = (y + dy[j]) / 12.0f;
        const float nx = (sx - .5f) / .5f, ny = (sy - .47f) / .65f;
        const float radius = sqrtf(nx * nx + ny * ny);
        const float edge = fminf(1, fmaxf(0, (radius - .62f) / .7f));
        const float alpha = .18f * edge * edge * (3 - 2 * edge);
        points[j] =
            (ArRenderVertex2D){{sx * v->viewport.w, sy * v->viewport.h}, {0, 0, 0, alpha}, {0, 0}};
      }
      if (!ScreenTriangle(&batch, points[0], points[1], points[2]) ||
          !ScreenTriangle(&batch, points[0], points[2], points[3]))
        return false;
    }
  }
  return SubmitGlow(device, &batch, kArRenderBlendMode_Alpha);
}

/* Native Palace pixels stay 2D. This round profile supplies surface normals
 * and light occlusion, including the capitals and feet, without new art. */
static float ColumnRadius(float t) {
  const float heights[10] = {0, .015f, .04f, .095f, .115f, .885f, .905f, .96f, .985f, 1};
  const float radii[10] = {1.10f, 1.50f, 1.65f, 1.65f, 1.23f, 1.23f, 1.65f, 1.65f, 1.50f, 1.10f};
  for (int i = 0; i < 9; i++) {
    if (t > heights[i + 1]) continue;
    const float f = fmaxf(0, (t - heights[i]) / (heights[i + 1] - heights[i]));
    return radii[i] + f * (radii[i + 1] - radii[i]);
  }
  return radii[9];
}

static void ColumnShadows(void) {
  const float heights[10] = {0, .015f, .04f, .095f, .115f, .885f, .905f, .96f, .985f, 1};
  for (int row = 0; row < 6; row++) {
    for (int side = -1; side <= 1; side += 2) {
      for (int ring = 0; ring < 9; ring++) {
        for (int segment = 0; segment < 32; segment++) {
          const int ds[4] = {0, 1, 1, 0}, dr[4] = {0, 0, 1, 1};
          ChurchPoint p[4];
          for (int j = 0; j < 4; j++) {
            const float t = heights[ring + dr[j]], r = ColumnRadius(t);
            const float a = (segment + ds[j]) * kChurchPi / 16;
            p[j] = (ChurchPoint){side * kChurchColumnX + r * cosf(a), 13 + row * 9 + r * sinf(a),
                                 18 * t};
          }
          ChurchLighting_AddQuad(p);
          if (ring != 8) continue;
          p[0] = p[3];
          p[1] = p[2];
          p[2] = p[3] = (ChurchPoint){side * kChurchColumnX, 13 + row * 9, 18};
          ChurchLighting_AddQuad(p);
        }
      }
    }
  }
}

static void PrepareShadows(ChurchView *view) {
  const bool columns = ArRenderTexture_IsValid(view->options->people);
  if (ChurchLighting_Ready(columns) && s_shadow_reference == view->options->reference_altar) return;
  ChurchLighting_Begin(columns);
  view->shadow_build = true;
  Altar(view);
  if (columns) ColumnShadows();
  view->shadow_build = false;
  ChurchLighting_End();
  s_shadow_reference = view->options->reference_altar;
}

static ArRenderColorF ColumnColor(const ChurchView *v, ChurchPoint base, float u, float t) {
  const float radius = ColumnRadius(t), facing = sqrtf(fmaxf(0, 1 - u * u));
  const float slope = (ColumnRadius(t + .002f) - ColumnRadius(t - .002f)) / (.004f * 18);
  const ChurchPoint p = {base.x + radius * u, base.y - radius * facing, 18 * t};
  const ChurchPoint normal = {u, -facing, -slope};
  ArRenderColorF color =
      ChurchLighting_Shade(p, normal, (ArRenderColorF){1, 1, 1, 1}, .65f + .35f * fminf(1, t * 12));
  color.r *= v->brightness;
  color.g *= v->brightness;
  color.b *= v->brightness;
  return color;
}

/* Native art remains camera-facing, retaining its authored perspective.
 * Columns bake into room layers; live actors are composited between them. */
enum { kChurchSpriteCapacity = 12 * 8 * 32 + kActorQuads };
typedef struct ChurchSpriteBatch {
  Sim3DDepthVertex vertices[kChurchSpriteCapacity * 4];
  int count;
} ChurchSpriteBatch;

static bool Sprite(ChurchSpriteBatch *batch, const ChurchView *v, ChurchPoint position, float width,
                   float height, ArRenderRectI source, bool column) {
  Scene3DClipPoint anchor = Project(v, position);
  if (anchor.w <= .4f) return true;
  const int columns = column ? 8 : kActorColumns, rows = column ? 32 : kActorRows;
  if (batch->count + columns * rows > kChurchSpriteCapacity) return false;
  const float scale = kChurchFocalLength * v->viewport.h * .5f / anchor.w;
  const float sx = (anchor.x / anchor.w + 1) * v->viewport.w * .5f;
  const float sy = (1 - anchor.y / anchor.w) * v->viewport.h * .5f;
  const float depth = (anchor.z / anchor.w + 1) * .5f;
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < columns; x++) {
      const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
      Sim3DDepthVertex p[4];
      for (int j = 0; j < 4; j++) {
        const float u = (x + dx[j]) / (float)columns, t = (y + dy[j]) / (float)rows;
        const ArRenderColorF tint = column ? ColumnColor(v, position, 2 * u - 1, 1 - t)
                                           : s_actor_tint[source.x / 32][y + dy[j]][x + dx[j]];
        p[j] = (Sim3DDepthVertex){
            sx + (u - .5f) * width * scale,
            sy + (t - 1) * height * scale,
            depth,
            tint,
            {(source.x + u * source.w) / 256.0f, (source.y + t * source.h) / 512.0f}};
      }
      /* All cells of a cutout retain its anchor depth. Preserve far-to-near
       * order against other cutouts as well as hardware architecture depth. */
      int at = batch->count;
      while (at > 0 && batch->vertices[(at - 1) * 4].depth < depth) {
        memcpy(batch->vertices + at * 4, batch->vertices + (at - 1) * 4, sizeof(p));
        at--;
      }
      memcpy(batch->vertices + at * 4, p, sizeof(p));
      batch->count++;
    }
  }
  return true;
}

static bool NativeColumns(const ChurchView *v, ArRenderTexture atlas, bool near) {
  if (!ArRenderTexture_IsValid(atlas)) return true;
  ChurchSpriteBatch batch = {0};
  for (int row = 5; row >= 0; row--) {
    if ((row < 2) != near) continue;
    for (int side = -1; side <= 1; side += 2)
      if (!Sprite(&batch, v, (ChurchPoint){side * kChurchColumnX, 13 + row * 9.0f, 0}, 3.5f, 18,
                  (ArRenderRectI){0, 272, 28, 144}, true))
        return false;
  }
  return Sim3DDepthPass_AppendBillboards(atlas, batch.vertices, batch.count);
}
/* Project the native silhouette's feet onto the level floor. Transparent
 * padding must not detach a contact shadow from a kneeling visitor. */
static bool ActorContacts(ArRenderDevice *device, const ChurchView *v) {
  ChurchGlowBatch batch = {0};
  const float radii[4] = {0, .30f, .65f, 1}, opacity[4] = {.34f, .29f, .09f, 0};
  for (int actor = 0; actor < 2; actor++) {
    const ArRenderRectI b = v->options->actor_bounds[actor];
    if (b.w <= 0 || b.h <= 0) continue;
    const float padding = (1 - (b.y + b.h) / 32.0f) * kActorHeight;
    const float ray = kChurchEyeZ / (kChurchEyeZ - padding * cosf(kChurchCameraTilt));
    const float x =
        ((actor ? kActorX : -kActorX) + ((b.x + b.w * .5f) / 32 - .5f) * kActorWidth) * ray;
    const float y = kChurchEyeY + (kActorY + padding * sinf(kChurchCameraTilt) - kChurchEyeY) * ray;
    const float width = b.w / 32.0f * kActorWidth * .55f * ray;
    for (int ring = 0; ring < 3; ring++)
      for (int segment = 0; segment < 24; segment++) {
        const int dr[4] = {0, 0, 1, 1}, ds[4] = {0, 1, 1, 0};
        ArRenderVertex2D p[4];
        for (int j = 0; j < 4; j++) {
          const int r = ring + dr[j];
          const float angle = (segment + ds[j]) * kChurchPi / 12;
          const ChurchPoint world = {x + width * radii[r] * cosf(angle),
                                     y + .9f * ray * radii[r] * sinf(angle), kChurchFloorZ};
          const Scene3DClipPoint clip = Project(v, world);
          p[j] = (ArRenderVertex2D){{(clip.x / clip.w + 1) * v->viewport.w * .5f,
                                     (1 - clip.y / clip.w) * v->viewport.h * .5f},
                                    {0, 0, 0, opacity[r]},
                                    {0, 0}};
        }
        if (!ScreenTriangle(&batch, p[0], p[1], p[2]) || !ScreenTriangle(&batch, p[0], p[2], p[3]))
          return false;
      }
  }
  return SubmitGlow(device, &batch, kArRenderBlendMode_Alpha);
}

static bool Actors(ArRenderDevice *device, const ChurchView *v) {
  if (!ArRenderTexture_IsValid(v->options->people)) return true;
  ChurchSpriteBatch batch = {0};
  for (int actor = 0; actor < 2; actor++) {
    if (v->options->actor_bounds[actor].w <= 0 || v->options->actor_bounds[actor].h <= 0) continue;
    if (!Sprite(&batch, v, (ChurchPoint){actor ? kActorX : -kActorX, kActorY, 0}, kActorWidth,
                kActorHeight, (ArRenderRectI){actor * 32, 224, 32, 32}, false))
      return false;
  }
  if (!batch.count) return true;
  ArRenderVertex2D points[kActorQuads * 4];
  int32_t indices[kActorQuads * 6];
  for (int i = 0; i < batch.count * 4; i++)
    points[i] = (ArRenderVertex2D){
        {batch.vertices[i].x, batch.vertices[i].y}, batch.vertices[i].color, batch.vertices[i].uv};
  static const int corners[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < batch.count * 6; i++)
    indices[i] = (i / 6) * 4 + corners[i % 6];
  /* OBJ capture already contains the native fade. The cached lighting tint
   * excludes brightness, preventing a second fade on the people. */
  return ActorContacts(device, v) &&
         ArRenderDevice_DrawGeometry(device, v->options->people, points, batch.count * 4, indices,
                                     batch.count * 6);
}
static bool RoomImages(ArRenderDevice *device, const ChurchView *v) {
  if (s_room_images_valid && s_room_width == v->viewport.w && s_room_height == v->viewport.h)
    return true;
  if (s_room_width != v->viewport.w || s_room_height != v->viewport.h) {
    for (int i = 0; i < 2; i++) {
      ArRenderDevice_DestroyTexture(device, s_room_image[i]);
      s_room_image[i] = ArRenderTexture_Invalid();
    }
    s_room_width = v->viewport.w;
    s_room_height = v->viewport.h;
  }
  s_room_images_valid = false;
  ChurchView neutral = *v;
  neutral.brightness = 1;
  neutral.interior = true;
  const ArRenderTextureDesc desc = {.width = v->viewport.w,
                                    .height = v->viewport.h,
                                    .format = kArRenderPixelFormat_Argb8888,
                                    .usage = kArRenderTextureUsage_Target,
                                    .filter = kArRenderFilter_Nearest,
                                    .blend = kArRenderBlendMode_AlphaPremultiplied};
  for (int i = 0; i < 2; i++) {
    if (!ArRenderTexture_IsValid(s_room_image[i]) &&
        !ArRenderDevice_CreateTexture(device, &desc, &s_room_image[i]))
      return false;
    if (!Sim3DDepthPass_Begin(device, v->viewport.w, v->viewport.h, kArRenderFilter_Nearest) ||
        !AppendSurface(&neutral, s_room_mesh[i], s_white_texture, s_room_quads[i],
                       kSim3DDepthPass_Ground) ||
        !NativeColumns(&neutral, v->options->people, i == 1))
      return false;
    ArRenderTexture result = Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid());
    ArRenderTargetState saved;
    if (!ArRenderTexture_IsValid(result) ||
        ArRenderDevice_BeginTarget(device, s_room_image[i], &saved) != kArRenderTargetBegin_Ready)
      return false;
    const bool ok = ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 0}) &&
                    ArRenderDevice_DrawTexture(device, result, NULL, NULL);
    const bool restored = ArRenderDevice_EndTarget(device, &saved);
    if (!ok || !restored) return false;
  }
  s_room_images_valid = true;
  s_stats.room_bakes++;
  return true;
}
static bool RoomLayer(ArRenderDevice *device, const ChurchView *v, int layer) {
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend | kArRenderDrawState_Tint,
                                   .blend = kArRenderBlendMode_AlphaPremultiplied,
                                   .tint = {v->brightness, v->brightness, v->brightness, 1}};
  return ArRenderDevice_DrawTextureWithState(device, s_room_image[layer], NULL, NULL, &state);
}

/* Continue the daytime blue below the mathematical horizon: the finite town
 * opening also reveals this band, which otherwise becomes a flat pale fill. */
static const unsigned kSkyColors[5] = {0x172eb1, 0x3978e6, 0x5b9feb, 0x77b0ec, 0x77b0ec};

static void SkyRows(const ChurchView *view, float rows[5]) {
  const Scene3DClipPoint projected = Project(view, (ChurchPoint){0, 800, kChurchEyeZ});
  const float horizon = (1 - projected.y / projected.w) * .5f;
  rows[0] = 0;
  rows[1] = horizon;
  rows[2] = horizon + .15f;
  rows[3] = horizon + .32f;
  rows[4] = 1;
}

static ArRenderColorF SkyColor(const ChurchView *view, float y) {
  float rows[5];
  SkyRows(view, rows);
  for (int row = 0; row < 4; row++) {
    if (y > rows[row + 1]) continue;
    const float t = fminf(1, fmaxf(0, (y - rows[row]) / (rows[row + 1] - rows[row])));
    const ArRenderColorF a = Color(kSkyColors[row]), b = Color(kSkyColors[row + 1]);
    return (ArRenderColorF){(a.r + (b.r - a.r) * t) * view->brightness,
                            (a.g + (b.g - a.g) * t) * view->brightness,
                            (a.b + (b.b - a.b) * t) * view->brightness, 1};
  }
  ArRenderColorF color = Color(kSkyColors[4]);
  color.r *= view->brightness;
  color.g *= view->brightness;
  color.b *= view->brightness;
  return color;
}

static bool DaylightSky(ArRenderDevice *device, const ChurchView *view) {
  float rows[5];
  SkyRows(view, rows);
  ArRenderVertex2D vertices[10];
  for (int row = 0; row < 5; row++) {
    const ArRenderColorF color = SkyColor(view, rows[row]);
    for (int x = 0; x < 2; x++)
      vertices[row * 2 + x] = (ArRenderVertex2D){
          {(float)(x * view->viewport.w), rows[row] * view->viewport.h}, color, {0, 0}};
  }
  static const int32_t indices[24] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4,
                                      4, 5, 7, 4, 7, 6, 6, 7, 9, 6, 9, 8};
  return ArRenderDevice_DrawGeometry(device, ArRenderTexture_Invalid(), vertices, 10, indices, 24);
}

/* Copy only the projected arched aperture, before the native UI is drawn.
 * Full-scene UVs let the bloom inherit actual sky, model and shadow colors.
 * No interior pixels become emitters, even when the altar is brightly lit. */
static bool DoorAperture(ArRenderDevice *device, const ChurchView *view, ArRenderTexture texture,
                         ArRenderColorF tint, bool sky, ArRenderBlendMode blend) {
  ChurchPoint points[54] = {{-5, (kChurchDoorY + 3), 0}, {5, (kChurchDoorY + 3), 0},
                            {5, (kChurchDoorY + 3), 10}, {-5, (kChurchDoorY + 3), 0},
                            {5, (kChurchDoorY + 3), 10}, {-5, (kChurchDoorY + 3), 10}};
  for (int i = 0; i < 16; i++) {
    const float a = i * kChurchPi / 16, b = (i + 1) * kChurchPi / 16;
    points[6 + i * 3] = (ChurchPoint){0, (kChurchDoorY + 3), 10};
    points[7 + i * 3] = (ChurchPoint){5 * cosf(a), (kChurchDoorY + 3), 10 + 5 * sinf(a)};
    points[8 + i * 3] = (ChurchPoint){5 * cosf(b), (kChurchDoorY + 3), 10 + 5 * sinf(b)};
  }
  ArRenderVertex2D vertices[54];
  int32_t indices[54];
  for (int i = 0; i < 54; i++) {
    const Scene3DClipPoint p = Project(view, points[i]);
    const float x = (p.x / p.w + 1) * .5f, y = (1 - p.y / p.w) * .5f;
    vertices[i] = (ArRenderVertex2D){
        {x * view->viewport.w, y * view->viewport.h}, sky ? SkyColor(view, y) : tint, {x, y}};
    indices[i] = i;
  }
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blend};
  return ArRenderDevice_DrawGeometryWithState(device, texture, vertices, 54, indices, 54, &state);
}

static bool EnsureDoorBloom(ArRenderDevice *device, const ChurchView *view) {
  const int width = (view->viewport.w + 3) / 4, height = (view->viewport.h + 3) / 4;
  if (width == s_door_bloom_width && height == s_door_bloom_height) return true;
  ResetDoorBloom(device);
  const ArRenderTextureDesc desc = {.width = width,
                                    .height = height,
                                    .format = kArRenderPixelFormat_Argb8888,
                                    .usage = kArRenderTextureUsage_Target,
                                    .filter = kArRenderFilter_Linear,
                                    .blend = kArRenderBlendMode_AlphaPremultiplied};
  for (int i = 0; i < 3; i++) {
    if (ArRenderDevice_CreateTexture(device, &desc, &s_door_bloom[i])) continue;
    ResetDoorBloom(device);
    return false;
  }
  s_door_bloom_width = width;
  s_door_bloom_height = height;
  return true;
}

/* Separable Gaussian in quarter-resolution targets. Weighted premultiplied
 * samples preserve energy and soft edges without readback or a new shader. */
static bool BlurDoor(ArRenderDevice *device, ArRenderTexture source, ArRenderTexture target,
                     float step_x, float step_y) {
  ArRenderTargetState saved;
  if (ArRenderDevice_BeginTarget(device, target, &saved) != kArRenderTargetBegin_Ready)
    return false;
  bool valid = ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 0});
  float total = 0;
  for (int tap = -4; tap <= 4; tap++)
    total += expf(-tap * tap / 4.2f);
  for (int tap = -4; tap <= 4 && valid; tap++) {
    const float weight = expf(-tap * tap / 4.2f) / total;
    const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend | kArRenderDrawState_Tint,
                                     .blend = kArRenderBlendMode_AddPremultiplied,
                                     .tint = {weight, weight, weight, weight}};
    const ArRenderRectF destination = {tap * step_x, tap * step_y, (float)s_door_bloom_width,
                                       (float)s_door_bloom_height};
    valid = ArRenderDevice_DrawTextureWithState(device, source, NULL, &destination, &state);
    /* Additive color preserves destination alpha. Accumulate coverage in a
     * separate submission so this texture also works as a defocused image,
     * rather than becoming an unintended extra additive glare layer. */
    ArRenderDrawState coverage = state;
    coverage.blend = kArRenderBlendMode_AlphaAccumulate;
    valid =
        valid && ArRenderDevice_DrawTextureWithState(device, source, NULL, &destination, &coverage);
  }
  const bool restored = ArRenderDevice_EndTarget(device, &saved);
  return valid && restored;
}

static bool AddDoorBloom(ArRenderDevice *device, const ChurchView *view, float strength) {
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend | kArRenderDrawState_Tint,
                                   .blend = kArRenderBlendMode_AddPremultiplied,
                                   .tint = {strength, strength, strength, 1}};
  const ArRenderRectF destination = {0, 0, (float)view->viewport.w, (float)view->viewport.h};
  return ArRenderDevice_DrawTextureWithState(device, s_door_bloom[2], NULL, &destination, &state);
}

/* The same inexpensive blurred aperture supplies varying defocus. A tiny
 * geometry-derived depth proxy controls the blend, without a GPU depth readback
 * or another fullscreen pass. Near silhouettes keep more native detail. */
static bool FocusedDoor(ArRenderDevice *device, const ChurchView *v, ArRenderTexture texture) {
  enum { columns = 16, rows = 24, count = columns * rows };
  ArRenderVertex2D vertices[count * 4];
  int32_t indices[count * 6];
  const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1}, order[6] = {0, 1, 2, 0, 2, 3};
  for (int y = 0; y < rows; y++)
    for (int x = 0; x < columns; x++) {
      const int cell = y * columns + x;
      for (int j = 0; j < 4; j++) {
        const float z = (y + dy[j]) * 15.0f / rows;
        const float width = z <= 10 ? 5 : sqrtf(fmaxf(0, 25 - (z - 10) * (z - 10)));
        const float wx = ((x + dx[j]) * 2.0f / columns - 1) * width;
        const Scene3DClipPoint p = Project(v, (ChurchPoint){wx, kChurchDoorY + 3, z});
        float u = (p.x / p.w + 1) * .5f, w = (1 - p.y / p.w) * .5f;
        float alpha = ChurchFocus_Blur(&s_focus, p.x / p.w * v->aspect, p.y / p.w);
        vertices[cell * 4 + j] = (ArRenderVertex2D){
            {u * v->viewport.w, w * v->viewport.h}, {alpha, alpha, alpha, alpha}, {u, w}};
      }
      for (int j = 0; j < 6; j++)
        indices[cell * 6 + j] = cell * 4 + order[j];
    }
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend,
                                   .blend = kArRenderBlendMode_AlphaPremultiplied};
  return ArRenderDevice_DrawGeometryWithState(device, texture, vertices, count * 4, indices,
                                              count * 6, &state);
}

static bool DoorDaylight(ArRenderDevice *device, const ChurchView *view, ArRenderTexture scene) {
  if (!EnsureDoorBloom(device, view)) return false;
  ChurchView small = *view;
  small.viewport = (ArRenderRectI){0, 0, s_door_bloom_width, s_door_bloom_height};
  const ArRenderColorF white = {1, 1, 1, 1};
  const ArRenderColorF exposure = {.98f * view->brightness, .99f * view->brightness,
                                   view->brightness, .22f};
  ArRenderTargetState saved;
  if (ArRenderDevice_BeginTarget(device, s_door_bloom[0], &saved) != kArRenderTargetBegin_Ready)
    return false;
  const bool valid =
      ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 0}) &&
      DoorAperture(device, &small, ArRenderTexture_Invalid(), white, true,
                   kArRenderBlendMode_Opaque) &&
      DoorAperture(device, &small, scene, white, false, kArRenderBlendMode_AlphaPremultiplied) &&
      DoorAperture(device, &small, ArRenderTexture_Invalid(), exposure, false,
                   kArRenderBlendMode_Alpha);
  const bool restored = ArRenderDevice_EndTarget(device, &saved);
  if (!valid || !restored) return false;
  const float scale = view->viewport.h / (896.0f * 4);
  if (!BlurDoor(device, s_door_bloom[0], s_door_bloom[1], 4 * scale, 0) ||
      !BlurDoor(device, s_door_bloom[1], s_door_bloom[2], 0, 4 * scale))
    return false;
  /* Focus remains at the altar. The distant exterior is mostly replaced by
   * the soft aperture copy, with a small crisp contribution to retain shapes.
   * Restrained glare spills beyond the arch; the room and native people never
   * enter the defocus source. Captured brightness preserves the church fade. */
  return DoorAperture(device, view, ArRenderTexture_Invalid(), exposure, false,
                      kArRenderBlendMode_Alpha) &&
         FocusedDoor(device, view, s_door_bloom[2]) && AddDoorBloom(device, view, .28f) &&
         BlurDoor(device, s_door_bloom[2], s_door_bloom[1], 10 * scale, 0) &&
         BlurDoor(device, s_door_bloom[1], s_door_bloom[2], 0, 10 * scale) &&
         AddDoorBloom(device, view, .12f);
}

bool ChurchScene_Draw(ArRenderDevice *device, ArRenderRectI viewport,
                      const SimBackgroundVoxelScene *town, const uint32_t *ground,
                      const ChurchSceneOptions *options) {
  if (!options || viewport.w <= 0 || viewport.h <= 0) return false;
  ChurchView view = {.viewport = viewport,
                     .aspect = (float)viewport.w / viewport.h,
                     .brightness = options->brightness,
                     .seconds = options->seconds,
                     .landscape = options->landscape_height_pct,
                     .valid = true,
                     .options = options};
#if !AR_SIM3D_TERRAIN_ELEVATION
  view.landscape = 0;
#endif
  if (!FindDoor(&view, town)) return false;
  ArRenderColorF sky = Color(kChurchSky);
  sky.r *= view.brightness;
  sky.g *= view.brightness;
  sky.b *= view.brightness;
  if (!ArRenderDevice_Clear(device, sky) || !DaylightSky(device, &view) ||
      !Sim3DDepthPass_Begin(device, viewport.w, viewport.h, kArRenderFilter_Linear))
    return false;
  const bool rebuild = DetailChanged(&view, town);
  if (rebuild) {
    BeginDetails(&view, town);
    view.valid = Landscape(device, &view, town) && Ground(device, &view, town, ground);
    PrepareShadows(&view);
    view.valid = view.valid && PrepareDetails(device, &view, town);
  }
  if (!view.valid || !RoomImages(device, &view) ||
      !Sim3DDepthPass_Begin(device, viewport.w, viewport.h, kArRenderFilter_Linear))
    return false;
  view.valid = Landscape(device, &view, town) && Ground(device, &view, town, ground);
  static const int poses[8] = {0, 0, 1, 1, 0, 0, 2, 2};
  const int pose = poses[ChurchDetail_Phase(view.seconds, 8, 6)];
  view.valid = view.valid &&
               AppendSurface(&view, s_exterior_mesh, s_white_texture, s_exterior_quads,
                             kSim3DDepthPass_Ground) &&
               AppendSurface(&view, s_foliage_mesh[pose], s_white_texture, s_foliage_quads[pose],
                             kSim3DDepthPass_Ground);
  view.valid = view.valid && Water(&view);
  view.interior = true;
  ArRenderTexture texture = Sim3DDepthPass_Submit(device, ArRenderTexture_Invalid());
  const ArRenderRectF dst = {0, 0, (float)viewport.w, (float)viewport.h};
  return view.valid && ArRenderTexture_IsValid(texture) &&
         ArRenderDevice_DrawTexture(device, texture, NULL, &dst) && RoomLayer(device, &view, 0) &&
         Actors(device, &view) && RoomLayer(device, &view, 1) &&
         DoorDaylight(device, &view, texture) && Light(device, &view) &&
         DustAndGlow(device, &view) && Vignette(device, &view);
}
