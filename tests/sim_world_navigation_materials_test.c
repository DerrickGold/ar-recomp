#include "sim/sim_world_navigation_clouds.h"
#include "sim/sim_world_navigation_sky_clouds.h"
#include "sim/sim_world_navigation_globe.h"
#include "sim/sim_world_navigation_mountains.h"
#include "sim/sim_world_navigation_mountain_transition.h"
#include "sim/sim_world_navigation_terrain.h"
#include "sim/sim_town_ground_art.h"
#include "sim/sim_town_layout.h"
#include "sim/sim_background_mountain_objects.h"
#include "sim/sim_background_mountain_silhouette.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scalar oracle: no row, longitude, octave or lattice reuse. Keep the
 * original interpolation order rather than sharing production helpers. */
static float ReferenceCloudHash(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u +
      (uint32_t)z * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 65535u) / 65535.0f;
}

static float ReferenceCloudSmooth(float x) { return x * x * (3 - 2 * x); }

static float ReferenceCloudNoise(float x, float y, float z) {
  const int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
  const float fx = ReferenceCloudSmooth(x - ix), fy = ReferenceCloudSmooth(y - iy);
  const float fz = ReferenceCloudSmooth(z - iz);
  float plane[2];
  for (int dz = 0; dz < 2; dz++) {
    float row[2];
    for (int dy = 0; dy < 2; dy++) {
      const float a = ReferenceCloudHash(ix, iy + dy, iz + dz);
      row[dy] = a + (ReferenceCloudHash(ix + 1, iy + dy, iz + dz) - a) * fx;
    }
    plane[dz] = row[0] + (row[1] - row[0]) * fy;
  }
  return plane[0] + (plane[1] - plane[0]) * fz;
}

static uint32_t ReferenceCloudTexel(int x, int y, int width, int height, float scale) {
  const float pi = 3.14159265358979323846f;
  const float longitude = x == width - 1 ? -pi : 2 * pi * x / (width - 1) - pi;
  const float angle = pi * y / (height - 1), radius = sinf(angle);
  const bool pole = y == 0 || y == height - 1;
  const float nx = pole ? 0 : radius * cosf(longitude);
  const float ny = pole ? (y == 0 ? 1 : -1) : cosf(angle);
  const float nz = pole ? 0 : radius * sinf(longitude);
  float total = 0, sum = 0, amplitude = .5f, frequency = scale * 1.5f;
  for (int octave = 0; octave < 5; octave++) {
    total += ReferenceCloudNoise(nx * frequency + .37f + octave * .53f,
        ny * frequency + .61f + octave * .29f, nz * frequency + .23f + octave * .71f) * amplitude;
    sum += amplitude; amplitude *= .5f; frequency *= 2;
  }
  const float density = ReferenceCloudSmooth(fminf(1, fmaxf(0, (total / sum - .42f) / .38f)));
  const unsigned alpha = (unsigned)(density * 255 + .5f), tint = 236 + (unsigned)(density * 19);
  return (alpha << 24) | (tint << 16) | (tint << 8) | 255u;
}

static void TestCloudBakeExact(void) {
  const int dimensions[][2] = {{2, 2}, {2, 17}, {17, 2}, {65, 33}, {513, 9}, {1025, 7}, {512, 256}};
  const float scales[] = {4, 2.7f, 6.3f, .0001f, .5f, 32};
  for (size_t d = 0; d < sizeof(dimensions) / sizeof(dimensions[0]); d++)
    for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
      const int width = dimensions[d][0], height = dimensions[d][1], pitch = width + 7;
      const size_t count = (size_t)pitch * height;
      uint32_t *pixels = malloc(count * sizeof(*pixels));
      assert(pixels); memset(pixels, 0xA5, count * sizeof(*pixels));
      assert(SimWorldNavigationClouds_Bake(pixels, pitch, width, height, scales[s]));
      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
          assert(pixels[(size_t)y * pitch + x] == ReferenceCloudTexel(x, y, width, height, scales[s]));
        for (int x = width; x < pitch; x++) assert(pixels[(size_t)y * pitch + x] == 0xA5A5A5A5u);
      }
      free(pixels);
    }
}

static void TestCloudSphere(void) {
  enum { width = 65, height = 33, pitch = 68 };
  uint32_t pixels[pitch * height];
  for (int layer = 0; layer < 3; layer++) {
    memset(pixels, 0xA5, sizeof(pixels));
    assert(SimWorldNavigationClouds_Bake(pixels, pitch, width, height, 2.7f + layer));
    unsigned covered = 0, clear = 0;
    for (int y = 0; y < height; y++) {
      assert(pixels[y * pitch] == pixels[y * pitch + width - 1]);
      for (int x = width; x < pitch; x++) assert(pixels[y * pitch + x] == 0xA5A5A5A5u);
      for (int x = 0; x < width; x++) {
        const unsigned alpha = pixels[y * pitch + x] >> 24;
        covered += alpha > 70;
        clear += alpha < 15;
        if (y == 0 || y == height - 1) assert(pixels[y * pitch + x] == pixels[y * pitch]);
      }
    }
    assert(covered > 100 && clear > 100);
    if (layer == 0) {
      /* Cross two longitude-cache boundaries. Power-of-two spacing samples
       * the same directions exactly, with no platform-specific pixel hash. */
      enum { wide = 1025, wide_pitch = 1028 };
      uint32_t *larger = malloc((size_t)wide_pitch * height * sizeof(*larger));
      assert(larger);
      memset(larger, 0xA5, (size_t)wide_pitch * height * sizeof(*larger));
      assert(SimWorldNavigationClouds_Bake(larger, wide_pitch, wide, height, 2.7f));
      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) assert(larger[y * wide_pitch + x * 16] == pixels[y * pitch + x]);
        for (int x = wide; x < wide_pitch; x++) assert(larger[y * wide_pitch + x] == 0xA5A5A5A5u);
      }
      free(larger);
    }
  }
  assert(!SimWorldNavigationClouds_Bake(NULL, pitch, width, height, 4));
  assert(!SimWorldNavigationClouds_Bake(pixels, width - 1, width, height, 4));
  assert(!SimWorldNavigationClouds_Bake(pixels, pitch, width, height, NAN));
  const float seam_normal[2][3] = {{-1, 0, -0.00001f}, {-1, 0, 0.00001f}};
  const SimWorldNavigationCloudRotation identity = SimWorldNavigationClouds_Rotation(0, 0);
  float u[4], v;
  SimWorldNavigationClouds_UV(seam_normal[0], &identity, &u[0], &v);
  SimWorldNavigationClouds_UV(seam_normal[1], &identity, &u[1], &v);
  u[2] = u[1]; u[3] = u[0];
  assert(fabsf(u[0] - u[1]) > 0.99f);
  SimWorldNavigationClouds_Unwrap(u);
  assert(fabsf(u[0] - u[1]) < 0.0001f);
  /* Every direction, including the uncharted antipodal hemisphere, has a
   * finite cloud sample. Rotation is a rigid advection, not UV pole drift. */
  const SimWorldNavigationCloudRotation rotation = SimWorldNavigationClouds_Rotation(.37f, .61f);
  for (int lat = 0; lat <= 48; lat++)
    for (int lon = 0; lon < 96; lon++) {
      const float a = lat * 3.14159265359f / 48, b = lon * 6.28318530718f / 96;
      const float n[3] = {sinf(a) * cosf(b), cosf(a), sinf(a) * sinf(b)};
      float uu, vv;
      SimWorldNavigationClouds_UV(n, &rotation, &uu, &vv);
      assert(isfinite(uu) && uu >= 0 && uu <= 1 && isfinite(vv) && vv >= 0 && vv <= 1);
    }
}

static void CheckCloudChartTriangle(const SimWorldNavigationCloudCoordinate triangle[3]) {
  SimWorldNavigationCloudPatch patches[kSimWorldNavigationCloudMaxPatches];
  const int count = SimWorldNavigationClouds_SplitTriangle(triangle, patches);
  assert(count > 0 && count <= kSimWorldNavigationCloudMaxPatches);
  double area = 0;
  for (int i = 0; i < count; i++) {
    const SimWorldNavigationCloudPatch *patch = &patches[i];
    float lo = 1, hi = 0;
    for (int p = 0; p < 4; p++) {
      double sum = 0;
      for (int j = 0; j < 3; j++) {
        assert(patch->weight[p][j] >= -1e-12 && patch->weight[p][j] <= 1.000000000001);
        sum += patch->weight[p][j];
      }
      assert(fabs(sum - 1) < 1e-12);
      assert(isfinite(patch->u[p]) && patch->u[p] >= 0 && patch->u[p] <= 1);
      assert(isfinite(patch->v[p]) && patch->v[p] >= 0 && patch->v[p] <= 1);
      lo = fminf(lo, patch->u[p]); hi = fmaxf(hi, patch->u[p]);
    }
    assert(hi - lo <= .250001f);
    for (int t = 0; t < 2; t++) {
      const double *a = patch->weight[0], *b = patch->weight[t + 1], *c = patch->weight[t + 2];
      const double cross = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
      assert(cross >= -1e-12);
      area += cross;
    }
  }
  assert(fabs(area - 1) < 1e-12);
}

static void TestCloudCharts(void) {
  /* Poles, signed zero, faces lying on a chart plane and a nearly tangential
   * crossing must partition once, not duplicate coverage on both sides. */
  const SimWorldNavigationCloudCoordinate cases[][3] = {
    {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}},
    {{-0.0f, -1, -0.0f}, {0, -1, .01f}, {0, -1, -.01f}},
    {{-.01f, 1, 0}, {.01f, 1, 0}, {0, 1, 0}},
    {{-.01f, -1, -.01f}, {.01f, -1, -.01f}, {0, -1, .01f}},
    {{-.01f, 1, -.01f}, {.01f, 1, -.01f}, {0, 1, .01f}},
    {{-1e-12f, .3f, -.9f}, {.01f, .3f, -.9f}, {-.01f, .3f, -.9f}},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) CheckCloudChartTriangle(cases[i]);
  /* Reversing an edge's traversal must retain bit-identical projected cuts,
   * even at the poles. Coverage alone would miss one-pixel raster cracks. */
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    SimWorldNavigationCloudPatch forward[kSimWorldNavigationCloudMaxPatches];
    SimWorldNavigationCloudPatch reverse[kSimWorldNavigationCloudMaxPatches];
    const SimWorldNavigationCloudCoordinate reversed[3] = {cases[i][2], cases[i][1], cases[i][0]};
    const int count = SimWorldNavigationClouds_SplitTriangle(cases[i], forward);
    assert(SimWorldNavigationClouds_SplitTriangle(reversed, reverse) == count);
    const float screen[3][3] = {{181.483719f, 493.441345f, .942772f},
        {199.394531f, 486.884277f, .943711f}, {193.047897f, 503.429077f, .945571f}};
    for (int face = 0; face < count; face++)
      for (int p = 0; p < 4; p++) {
        float a[3];
        for (int axis = 0; axis < 3; axis++) {
          double sum = 0;
          for (int j = 0; j < 3; j++) sum += forward[face].weight[p][j] * screen[j][axis];
          a[axis] = (float)sum;
        }
        bool found = false;
        for (int f = 0; f < count && !found; f++)
          for (int q = 0; q < 4 && !found; q++) {
            float b[3];
            for (int axis = 0; axis < 3; axis++) {
              double sum = 0;
              for (int j = 0; j < 3; j++) sum += reverse[f].weight[q][j] * screen[2 - j][axis];
              b[axis] = (float)sum;
            }
            found = !memcmp(a, b, sizeof(a));
          }
        assert(found);
      }
  }
  for (int time = 0; time < 10; time++) {
    const SimWorldNavigationCloudRotation rotation = SimWorldNavigationClouds_Rotation(time * .13f, time * .29f);
    for (int lat = 0; lat < 48; lat++)
      for (int lon = 0; lon < 96; lon++) {
        SimWorldNavigationCloudCoordinate quad[4];
        const int x[4] = {lon, lon + 1, lon + 1, lon};
        const int y[4] = {lat, lat, lat + 1, lat + 1};
        for (int p = 0; p < 4; p++) {
          const float a = y[p] * 3.14159265359f / 48, b = x[p] * 6.28318530718f / 96;
          const float n[3] = {sinf(a) * cosf(b), cosf(a), sinf(a) * sinf(b)};
          quad[p] = SimWorldNavigationClouds_Coordinate(n, &rotation);
        }
        if (!SimWorldNavigationClouds_NeedsSplit(quad)) continue;
        for (int t = 0; t < 2; t++) {
          const SimWorldNavigationCloudCoordinate triangle[3] = {quad[0], quad[t + 1], quad[t + 2]};
          CheckCloudChartTriangle(triangle);
        }
      }
  }
}

typedef struct MeshCapture {
  unsigned count;
  float x[7][4], y[7][4], z[7][4];
  SimBackgroundMountainMeshUV uv[7][4];
} MeshCapture;

static void CaptureMesh(
    void *user, const float x[4], const float y[4], const float z[4],
    const SimBackgroundMountainMeshUV uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]) {
  MeshCapture *out = user;
  assert(out->count < 7);
  memcpy(out->x[out->count], x, sizeof(out->x[0]));
  memcpy(out->y[out->count], y, sizeof(out->y[0]));
  memcpy(out->z[out->count], z, sizeof(out->z[0]));
  memcpy(out->uv[out->count], uv, sizeof(out->uv[0]));
  for (int p = 0; p < 4; p++) {
    assert(isfinite(x[p]) && isfinite(y[p]) && isfinite(z[p]));
    assert(uv[p].x >= 0 && uv[p].x <= 1 && uv[p].y >= 0 && uv[p].y <= 1);
    assert(brightness[p] > 0 && alpha[p] == 255);
  }
  out->count++;
}

static void TestSharedMesh(void) {
  SimBackgroundMountainRelief relief;
  for (int detail = kSimBackgroundVoxelDetail_Low; detail <= kSimBackgroundVoxelDetail_Ultra; detail++) {
    SimBackgroundMountainRelief_Resolve((SimBackgroundVoxelDetail)detail, &relief);
    MeshCapture capture = {0};
    SimBackgroundMountainMeshContext context = {
      .relief = &relief, .height_scale = 1, .atlas_pixels = 512,
      .stack_direction = {0, -1}, .emit = CaptureMesh, .user = &capture,
    };
    SimBackgroundMountainMesh_StackTile(&context, 64, 64, 64, 64, 1, 0, 1, 1, 0);
    assert(capture.count == relief.stack_layer_count);
    for (unsigned face = 0; face < capture.count; face++) {
      /* Peak height and contact convention are independent of LOD. */
      assert(fabsf(capture.z[face][0] - 64 * .30f) < .00001f);
      assert(fabsf(capture.y[face][0] - (64 - 64 * .62f)) < .00001f);
      assert(capture.x[face][0] == 16);
    }
    const unsigned front = capture.count - 1;
    assert(fabsf(capture.y[front][3] - (64 - 48 * .62f)) < .00001f);
    SimBackgroundMountainMesh_SkirtTile(&context, 64, 64, 1, 0, 1, 1, 0x81, false);
    assert(capture.count == relief.stack_layer_count + 1u);
    assert(capture.z[capture.count - 1][0] > 0);
    assert(capture.z[capture.count - 1][2] == 0 && capture.z[capture.count - 1][3] == 0);
  }
}

static void TestMountainTileSampling(void) {
  const int atlas_sizes[] = {512, 1024};
  const int source_cells[][2] = {{0, 0}, {7, 13}, {31, 31}};
  for (int detail = kSimBackgroundVoxelDetail_Low;
       detail <= kSimBackgroundVoxelDetail_Ultra; detail++) {
    SimBackgroundMountainRelief relief;
    SimBackgroundMountainRelief_Resolve((SimBackgroundVoxelDetail)detail, &relief);
    for (size_t a = 0; a < sizeof(atlas_sizes) / sizeof(atlas_sizes[0]); a++)
      for (size_t cell = 0; cell < sizeof(source_cells) / sizeof(source_cells[0]); cell++) {
        const int sx = source_cells[cell][0], sy = source_cells[cell][1];
        MeshCapture capture[2] = {{0}};
        for (int mirrored = 0; mirrored < 2; mirrored++) {
          SimBackgroundMountainMeshContext context = {
            .relief = &relief, .height_scale = 1, .atlas_pixels = atlas_sizes[a],
            .stack_direction = {0, -1}, .emit = CaptureMesh, .user = &capture[mirrored],
          };
          SimBackgroundMountainMesh_StackTile(
              &context, 64, 64, 64, 64, 1, 0, sx, sy,
              mirrored ? kSimBackgroundMountainCapTile_MirrorX : 0);
          assert(capture[mirrored].count == relief.stack_layer_count);
          for (unsigned face = 0; face < capture[mirrored].count; face++)
            for (int p = 0; p < 4; p++) {
              const bool right = (p == 1 || p == 2) != (mirrored != 0);
              const float u = capture[mirrored].uv[face][p].x * atlas_sizes[a];
              const float v = capture[mirrored].uv[face][p].y * atlas_sizes[a];
              /* Every LOD/copy samples texel centres, never the neighbouring
               * tile. Nearest filtering alone did not prevent edge leakage. */
              assert(u == sx * 16 + (right ? 15.5f : .5f));
              assert(v == sy * 16 + (p >= 2 ? 15.5f : .5f));
            }
        }
        /* Mirroring changes the art only, not the joined mesh vertices. */
        assert(!memcmp(capture[0].x, capture[1].x, sizeof(capture[0].x)));
        assert(!memcmp(capture[0].y, capture[1].y, sizeof(capture[0].y)));
        assert(!memcmp(capture[0].z, capture[1].z, sizeof(capture[0].z)));
      }
  }
}

static void CheckMountainSceneSampling(const SimWorldNavigationMountainScene *scene) {
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    const float atlas = kSimWorldNavigationMountainAtlasPixels;
    const float u0 = floorf(face->uv[0].x * atlas / 16) * 16;
    const float v0 = floorf(face->uv[0].y * atlas / 16) * 16;
    for (int p = 0; p < 4; p++) {
      const float u = face->uv[p].x * atlas - u0;
      const float v = face->uv[p].y * atlas - v0;
      /* Includes clipped rear strips, exterior continuations and volcano
       * roofs. Interpolation must stay inside one tile's texel centres. */
      assert(u >= .5f - .0001f && u <= 15.5f + .0001f);
      assert(v >= .5f - .0001f && v <= 15.5f + .0001f);
    }
  }
}

static int FirstOpaqueRow(const SimBackgroundMountainObject *object, int column) {
  const int height = object->height_cells * 16;
  for (int y = 0; y < height; y++) {
    if (!(object->row_occupied_mask[y / 16] & (1u << (column / 16)))) continue;
    bool opaque = false;
    assert(SimBackgroundMountainSilhouette_Lookup(
        object->source_tile[y / 16][column / 16], column % 16, y % 16, &opaque));
    if (opaque) return y;
  }
  return height;
}

static void CheckRearClearance(const SimWorldNavigationTownGround *ground,
                               const SimWorldNavigationMountainScene *scene) {
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    if (!face->rear_slope) continue;
    float min_x = face->x[0], max_x = min_x, min_y = face->y[0], max_y = min_y;
    for (int p = 1; p < 4; p++) {
      min_x = fminf(min_x, face->x[p]); max_x = fmaxf(max_x, face->x[p]);
      min_y = fminf(min_y, face->y[p]); max_y = fmaxf(max_y, face->y[p]);
    }
    /* Strips stay within one x cell. Check every row they span, not just
     * corners: a large quad must never bridge across a protected cell. */
    assert(ceilf(max_x) - floorf(min_x) <= 1);
    for (int y = (int)floorf(min_y); y < (int)ceilf(max_y); y++)
      for (int x = (int)floorf(min_x); x < (int)ceilf(max_x); x++)
        for (uint8_t town = 1; town <= kSimTownCount; town++) {
          int ox, oy;
          assert(SimWorldMap_OriginForTown(town, &ox, &oy));
          if (x < ox || x >= ox + 32 || y < oy || y >= oy + 32) continue;
          assert(ground->enabled_town_mask & (1u << (town - 1)));
          assert(!(ground->object_rows[town - 1][y - oy] & (1u << (x - ox))));
          assert(SimBackgroundMountains_TileFlags(town, ground->terrain[town - 1][(y - oy) * 32 + x - ox]));
        }
  }
}

static bool IsIsolatedInteriorPeak(const SimBackgroundMountainObjectList *objects,
                                   unsigned index) {
  const SimBackgroundMountainObject *a = &objects->objects[index];
  if (a->flags & kSimBackgroundMountainObject_Volcano) return false;
  const int bounds[4] = {a->cell_x, a->cell_y,
      a->cell_x + a->width_cells, a->cell_y + a->height_cells};
  if (bounds[0] <= 0 || bounds[1] <= 0 || bounds[2] >= 32 || bounds[3] >= 32) return false;
  for (unsigned i = 0; i < objects->count; i++) {
    if (i == index) continue;
    const SimBackgroundMountainObject *b = &objects->objects[i];
    const int x_separation = bounds[0] >= b->cell_x + b->width_cells || bounds[2] <= b->cell_x;
    const int y_separation = bounds[1] >= b->cell_y + b->height_cells || bounds[3] <= b->cell_y;
    if (!x_separation && !y_separation) return false;
  }
  return true;
}

static void CheckRearSlopes(const SimWorldNavigationTownGround *ground,
                            const SimWorldNavigationMountainScene *scene) {
  CheckMountainSceneSampling(scene);
  CheckRearClearance(ground, scene);
  size_t rear = 0, front = 0, contacts = 0;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(scene->town_mask & (1u << (town - 1)))) continue;
    SimBackgroundMountainField field;
    SimBackgroundMountainCaps caps;
    SimBackgroundMountainObjectList objects;
    SimBackgroundMountains_ClassifyCells(town, ground->terrain[town - 1], &field);
    SimBackgroundMountains_BuildNorthCaps(&field, &caps);
    assert(SimBackgroundMountainObjects_Build(&field, &caps, &objects));
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    unsigned isolated = 0;
    for (unsigned i = 0; i < objects.count; i++)
      if (IsIsolatedInteriorPeak(&objects, i)) {
        const SimBackgroundMountainObject *object = &objects.objects[i];
        printf("compact mountain: town=%u cell=(%d,%d) size=%ux%u rear-scale=0.70\n",
            town, object->cell_x, object->cell_y, object->width_cells, object->height_cells);
        isolated++;
      }
    printf("rear-fit policy town=%u: isolated=%u protected=%u\n", town, isolated, objects.count-isolated);
    for (size_t at = 0; at < scene->face_count; at++) {
      const SimWorldNavigationMountainFace *face = &scene->faces[at];
      if (face->town != town) continue;
      if (!face->rear_slope) { front++; continue; }
      rear++;
      bool matched = false;
      for (unsigned object_index = 0; object_index < objects.count && !matched; object_index++) {
        const SimBackgroundMountainObject *object = &objects.objects[object_index];
        const int width = object->width_cells * 16;
        const float scale = object->flags & kSimBackgroundMountainObject_Volcano ? 1.12f : 1;
        matched = true;
        for (int p = 0; p < 4; p++) {
          const float x = (face->x[p] - ox - object->cell_x) * 16;
          if (x < -.001f || x > width + .001f) { matched = false; break; }
          const float clamped = fminf(width, fmaxf(0, x));
          const int column = (int)clamped;
          float crest = 0;
          for (int side = 0; side < 2; side++) {
            const int c = column + side < width ? column + side : width;
            const float weight = side ? clamped - column : 1 - (clamped - column);
            crest += weight * .5f * (FirstOpaqueRow(object, c ? c - 1 : 0) +
                FirstOpaqueRow(object, c < width ? c : width - 1));
          }
          if (scene->overhead_crater && (object->flags & kSimBackgroundMountainObject_Volcano)) {
            /* The compact rear halves the run to the original ground
             * contact. Interpolate the rear plane independently from its
             * two source-column endpoints, including clipped vertices. */
            const float rise = fminf((object->height_cells - 1) * 16,
                object->height_cells * 16 - crest);
            const float height = rise * .30f * scale / 16;
            const float original_contact = oy + object->cell_y + crest / 16;
            const float front_y = oy + object->cell_y + object->height_cells - rise * .62f / 16;
            const float fraction = rise / ((object->height_cells - 1) * 16);
            const float back_y = front_y - 14.0f / 16 * fraction * fraction;
            const float contact = (original_contact + back_y) * .5f;
            if (face->summit_roof) {
              if (fabsf(face->z[p] - height) > .00002f ||
                  face->y[p] < back_y - .0001f || face->y[p] > front_y + .0001f) {
                matched = false; break;
              }
            } else {
              const float expected_y = height > 0
                  ? contact + (back_y - contact) * face->z[p] / height : contact;
              /* A cell clip interpolates the one-pixel strip; the exact
               * quadratic profile differs by less than 1/32 source pixel. */
              if (fabsf(face->y[p] - expected_y) > .03125f / 16 + .00002f) {
                matched = false; break;
              }
            }
            continue;
          }
          if (face->summit_roof) { matched = false; break; }
          crest = object->cell_y + crest / 16;
          const float fit_scale = IsIsolatedInteriorPeak(&objects, object_index) ? .70f : 1;
          const float ridge = object->cell_y + object->height_cells -
              (object->cell_y + object->height_cells - crest) * .62f;
          const float contact = ridge + (crest - ridge) * fit_scale;
          /* Independent inverse of the fold: native z gives distance from
           * the front contact; the rear's remaining 38% must recover the
           * original opaque contour, not extend into another ground cell. */
          const float recovered = face->y[p] - oy - face->z[p] / (.30f * scale) * .38f * fit_scale;
          /* Cell clipping adds interpolated vertices along an accepted
           * linear skyline span (at most half a source pixel of error). */
          if (fabsf(recovered - contact) > .5f / 16 + .00002f) { matched = false; break; }
        }
      }
      assert(matched);
      for (int p = 0; p < 4; p++) {
        if (face->summit_roof) assert(face->brightness[p] >= 235 && face->brightness[p] <= 255);
        else assert(face->brightness[p] >= 190 && face->brightness[p] <= 235);
        if (!face->exterior) assert(face->x[p] >= ox && face->x[p] <= ox + 32);
        assert(face->z[p] >= 0 && face->z[p] <= scene->maximum_rise);
        assert(face->uv[p].x >= 0 && face->uv[p].x <= 1);
        assert(face->uv[p].y >= 0 && face->uv[p].y <= 1);
        contacts += face->z[p] == 0;
      }
    }
  }
  assert(rear && contacts && rear < front * 8);
  printf("mountain slopes: towns=%02X front/wall=%zu rear=%zu ground-contacts=%zu\n",
      scene->town_mask, front, rear, contacts);
}

static void TestNativeMountainScene(void) {
  static const uint32_t rows[32] = {
    0xFFFFFFFFu, 0xFFFC3C3Fu, 0xFFF0000Fu, 0xFFC00003u, 0xF0000000u,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x00000600u, 0x00000F00u, 0x00006F00u, 0x0000FF80u, 0x0000FFE0u, 0x0000FFF8u,
  };
  uint8_t *rom = calloc(0x100000, 1);
  uint8_t *wram = calloc(0x20000, 1);
  assert(rom && wram);
  /* Every synthetic native texel is opaque red. The independent silhouette
   * table, not the palette, must remove the grass around each peak. */
  memset(rom + 0x60000, 0xFF, 0x8000);
  rom[0xE3B93 + 15 * 2] = 31;
  assert(SimTownGroundArt_Init(rom, 0x100000));
  SimWorldNavigationTownGround ground = {.enabled_town_mask = 2};
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      const uint8_t tile = rows[y] & (1u << x) ? 0x89 : 8;
      ground.terrain[1][y * 32 + x] = tile;
      wram[SimTownLayout_CellMapIndex(2, x, y)] = tile;
    }
  SimBackgroundMountainField paged, snapshot;
  SimBackgroundMountains_Classify(2, wram, &paged);
  SimBackgroundMountains_ClassifyCells(2, ground.terrain[1], &snapshot);
  assert(!memcmp(&paged, &snapshot, sizeof(paged)));
  SimWorldNavigationMountainScene scene = {0};
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(scene.town_mask == 2 && scene.face_count > 200 && scene.maximum_rise > 1);
  int ox, oy;
  assert(SimWorldMap_OriginForTown(2, &ox, &oy));
  bool wall = false, raised = false, extended_north = false;
  for (size_t at = 0; at < scene.face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene.faces[at];
    assert(face->town == 2);
    for (int p = 0; p < 4; p++) {
      assert(isfinite(face->x[p]) && isfinite(face->y[p]) && face->z[p] >= 0);
      wall |= face->brightness[p] == 150;
      raised |= face->z[p] > 1;
      extended_north |= face->y[p] < oy;
    }
  }
  assert(wall && raised && extended_north);
  CheckRearSlopes(&ground, &scene);
  /* A reserved model footprint wins even when its underlying metatile is
   * still rock. Clip all crossing rear polygons while preserving every
   * existing native front and side-wall vertex byte-for-byte. */
  SimWorldNavigationMountainScene protected_scene = {0};
  ground.object_rows[1][0] = UINT32_MAX;
  ground.object_rows[1][1] = 0xAAAAAAAAu;
  assert(SimWorldNavigationMountains_Build(&ground, &protected_scene));
  CheckRearClearance(&ground, &protected_scene);
  size_t old_front = 0, new_front = 0;
  for (;;) {
    while (old_front < scene.face_count && scene.faces[old_front].rear_slope) old_front++;
    while (new_front < protected_scene.face_count && protected_scene.faces[new_front].rear_slope) new_front++;
    if (old_front == scene.face_count || new_front == protected_scene.face_count) break;
    const SimWorldNavigationMountainFace *a = &scene.faces[old_front++];
    const SimWorldNavigationMountainFace *b = &protected_scene.faces[new_front++];
    assert(!memcmp(a->x, b->x, sizeof(a->x)) && !memcmp(a->y, b->y, sizeof(a->y)));
    assert(!memcmp(a->z, b->z, sizeof(a->z)) && !memcmp(a->uv, b->uv, sizeof(a->uv)));
    assert(!memcmp(a->brightness, b->brightness, sizeof(a->brightness)));
  }
  assert(old_front == scene.face_count && new_front == protected_scene.face_count);
  assert(protected_scene.face_count != scene.face_count);
  SimWorldNavigationMountains_Destroy(&protected_scene);
  memset(ground.object_rows, 0, sizeof(ground.object_rows));
  assert(scene.replacement[oy * 128 + ox]);
  const int tx = (0x81 & 15) * 16, ty = (0x81 >> 4) * 16;
  assert(scene.atlas[ty * 512 + tx] == 0); /* transparent peak margin */
  assert(scene.atlas[(ty + 15) * 512 + tx + 15] == 0xFFFF0000u);
  uint32_t *ground_pixels = malloc(2048u * 2048 * sizeof(*ground_pixels));
  assert(ground_pixels);
  for (size_t i = 0; i < 2048u * 2048; i++) ground_pixels[i] = 0xFF123456u;
  assert(SimWorldNavigationMountains_ClearGround(ground_pixels, 2048, &ground, scene.town_mask, NULL));
  assert(ground_pixels[(oy * 16) * 2048 + ox * 16] == 0xFFFF0000u);
  assert(ground_pixels[((oy + 10) * 16) * 2048 + (ox + 10) * 16] == 0xFF123456u);
  uint8_t dirty_ground[kSimWorldMapBytes] = {0};
  dirty_ground[oy * 128 + ox] = 1;
  for (size_t i = 0; i < 2048u * 2048; i++) ground_pixels[i] = 0xFF123456u;
  assert(SimWorldNavigationMountains_ClearGround(ground_pixels, 2048, &ground, scene.town_mask, dirty_ground));
  for (int y = 0; y < 2048; y++)
    for (int x = 0; x < 2048; x++)
      assert(ground_pixels[y * 2048 + x] ==
          (y / 16 == oy && x / 16 == ox ? 0xFFFF0000u : 0xFF123456u));
  ground.terrain[1][10 * 32 + 10] = 0x89; /* unknown topology fails closed */
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(scene.town_mask == 0 && scene.face_count == 0);
  ground.enabled_town_mask = 0;
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(scene.town_mask == 0 && scene.face_count == 0);
  SimWorldNavigationMountains_Destroy(&scene);
  assert(!scene.faces && !scene.atlas && !scene.face_count);
  SimTownGroundArt_Shutdown();
  assert(!SimWorldNavigationMountains_Build(&ground, &scene));
  free(ground_pixels); free(wram); free(rom);
}

static void TestMountainTransition(void) {
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom);
  memset(rom + 0x70000, 0x2F, 64); /* sand is not rock, even with identical RGB */
  memset(rom + 0x70000 + 64, 0x44, 32);
  memset(rom + 0x70000 + 96, 0x1D, 32); /* snow mixed into the same cell */
  for (int y = 35; y <= 47; y++) rom[0x33341 + y * 128 + 70] = 1;
  rom[0x33341 + 48 * 128 + 80] = 1; /* unsupported Fillmore must stay exact */
  assert(SimWorldMap_Init(rom, 0x100000));
  free(rom);
  SimWorldNavigationMountainScene scene = {.town_mask = 2};
  scene.atlas = calloc(512 * 512, sizeof(uint32_t));
  assert(scene.atlas);
  scene.atlas[0] = 0xFFFF0000u;
  scene.town_maximum_rise[1] = 1.2f;
  scene.replacement[48 * 128 + 70] = 2;
  SimWorldNavigationTownGround ground = {.enabled_town_mask = 2};
  SimWorldNavigationMountainTransition transition = {0};
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.patch_count > 0);
  assert(transition.ridge_scale[48 * 129 + 70] < .4f);
  assert(transition.ridge_scale[35 * 129 + 70] == 1);
  assert(transition.ridge_scale[48 * 129 + 80] == 1);
  SimWorldNavigationMountainTransition *resized = calloc(1, sizeof(*resized));
  assert(resized);
  assert(SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, 144, resized));
  assert(resized->ridge_scale[48 * 129 + 70] > transition.ridge_scale[48 * 129 + 70]);
  assert(resized->patch_count == transition.patch_count);
  /* Compare authored fields, not the padding after each patch's cell index. */
  for (size_t i = 0; i < transition.patch_count; i++) {
    assert(resized->patches[i].cell == transition.patches[i].cell);
    assert(!memcmp(resized->patches[i].pixels, transition.patches[i].pixels,
        sizeof(transition.patches[i].pixels))); /* Material is radius-independent. */
  }
  assert(SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, 48, resized));
  assert(!memcmp(resized->ridge_scale, transition.ridge_scale, sizeof(transition.ridge_scale)));
  assert(!SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, 0, resized));
  assert(!resized->patches && !resized->patch_count && !resized->join_anchor_count);
  assert(!SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, NAN, resized));
  free(resized);
  for (int y = 36; y <= 48; y++) {
    const float a = transition.ridge_scale[y * 129 + 70];
    const float b = transition.ridge_scale[(y - 1) * 129 + 70];
    assert(a > 0 && a <= 1 && fabsf(a - b) < .13f);
  }
  enum { pitch = 2050 };
  uint32_t *pixels = malloc(pitch * 2048u * sizeof(uint32_t));
  assert(pixels);
  for (size_t i = 0; i < pitch * 2048u; i++) pixels[i] = 0xFF808080u;
  assert(SimWorldNavigationMountainTransition_Apply(&transition, pixels, pitch, NULL));
  assert(pixels[47 * 16 * pitch + 70 * 16] != 0xFF808080u);
  for (int y = 0; y < 2048; y++)
    for (int x = 0; x < pitch; x++)
      if (x < 70 * 16 || x >= 71 * 16 || y < 36 * 16 || y >= 48 * 16 || y % 16 >= 8)
        assert(pixels[y * pitch + x] == 0xFF808080u);
  assert(!SimWorldNavigationMountainTransition_Apply(&transition, pixels, 1024, NULL));
  uint8_t dirty_material[kSimWorldMapBytes] = {0};
  dirty_material[47 * 128 + 70] = 1;
  const uint32_t reference_material = pixels[47 * 16 * pitch + 70 * 16];
  for (size_t i = 0; i < pitch * 2048u; i++) pixels[i] = 0xFF808080u;
  assert(SimWorldNavigationMountainTransition_Apply(&transition, pixels, pitch, dirty_material));
  assert(pixels[47 * 16 * pitch + 70 * 16] == reference_material);
  for (int y = 0; y < 2048; y++)
    for (int x = 0; x < pitch; x++)
      if (x / 16 != 70 || y / 16 != 47) assert(pixels[y * pitch + x] == 0xFF808080u);
  for (int x = 76; x <= 80; x++) {
    const float a = transition.ridge_scale[48 * 129 + x];
    const float b = transition.ridge_scale[48 * 129 + x - 1];
    assert(fabsf(a - b) < .2f); /* no sharp step at an unsupported town */
  }
  /* Two accepted palettes blend by physical proximity, not a nearest-town
   * switch that would leave a new seam between their material fields. */
  scene.town_mask |= 1u << 5;
  scene.town_maximum_rise[5] = 1.2f;
  scene.atlas[256 * 512] = 0xFFFFFFFFu;
  scene.replacement[44 * 128 + 70] = 6;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  uint32_t mixed = 0;
  for (size_t i = 0; i < transition.patch_count; i++)
    if (transition.patches[i].cell == 46 * 128 + 70) mixed = transition.patches[i].pixels[0];
  assert((mixed >> 24) > 200 && ((mixed >> 8) & 255u) > 60 && ((mixed >> 8) & 255u) < 200);
  SimWorldNavigationMountainTransition_Destroy(&transition);
  assert(!transition.patches && !transition.patch_count);
  scene.town_mask = 0;
  assert(!SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  SimWorldNavigationMountains_Destroy(&scene);
  SimWorldMap_Shutdown();
  free(pixels);
}

static void CheckJoinClearance(const SimWorldNavigationTownGround *ground,
                               const SimWorldNavigationMountainScene *scene,
                               const SimWorldNavigationMountainTransition *transition) {
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++) {
        const bool protected = !(ground->enabled_town_mask & scene->town_mask & (1u << (town - 1))) ||
            (ground->object_rows[town - 1][y] & (1u << x)) ||
            !SimBackgroundMountains_TileFlags(town, ground->terrain[town - 1][y * 32 + x]);
        for (int dy = 0; dy <= 1; dy++)
          for (int dx = 0; dx <= 1; dx++) {
            const int vx = x + dx, vy = y + dy;
            assert(transition->continuation_weight[(oy + vy) * 129 + ox + vx] == 0);
            if (protected || (vx > 0 && vx < 32 && vy > 0 && vy < 32)) {
              assert(transition->join_weight[(oy + vy) * 129 + ox + vx] == 0);
            }
          }
      }
  }
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 128; x++) {
      bool outside = true;
      for (uint8_t town = 1; town <= kSimTownCount; town++) {
        int ox, oy; assert(SimWorldMap_OriginForTown(town, &ox, &oy));
        outside &= x < ox || x >= ox + 32 || y < oy || y >= oy + 32;
      }
      if (!scene->replacement[y * 128 + x] &&
          (!outside || SimWorldMap_MountainCoverage(x, y) >= .18f)) continue;
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++)
          assert(transition->continuation_weight[(y + dy) * 129 + x + dx] == 0);
    }
}

static void TestMountainGeometryJoin(void) {
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom);
  memset(rom + 0x70000, 0x2F, 64);
  memset(rom + 0x70040, 0x44, 64);
  for (int y = 42; y < 48; y++)
    for (int x = 64; x < 79; x++) rom[0x33341 + y * 128 + x] = 1;
  assert(SimWorldMap_Init(rom, 0x100000));
  free(rom);
  SimWorldNavigationMountainScene scene = {.town_mask = 2, .face_count = 1};
  scene.atlas = calloc(512 * 512, sizeof(uint32_t));
  scene.faces = calloc(1, sizeof(*scene.faces));
  assert(scene.atlas && scene.faces);
  scene.atlas[0] = scene.atlas[10] = 0xFF996633;
  scene.town_maximum_rise[1] = 1.5f;
  *scene.faces = (SimWorldNavigationMountainFace){
    .town = 2, .x = {68, 72, 72, 68}, .y = {48, 48, 49, 49},
    .z = {.5f, 1.5f, 0, 0},
    .uv = {{0, 0}, {1.0f / 512, 0}, {1.0f / 512, 1.0f / 512}, {0, 1.0f / 512}},
  };
  SimWorldNavigationTownGround ground = {.enabled_town_mask = 2};
  for (int x = 16; x <= 30; x++) ground.terrain[1][x] = 0x89;
  scene.replacement[48 * 128 + 70] = 2;
  SimWorldNavigationMountainTransition transition = {0};
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.join_anchor_count == 5);
  for (int x = 68; x <= 72; x++) {
    float normal[3], metric;
    assert(SimWorldNavigationGlobe_Sample(x, 48, normal, &metric));
    assert(transition.join_weight[48 * 129 + x] == 1);
    assert(fabsf(transition.join_rise[48 * 129 + x] - (.5f + (x - 68) * .25f) * metric) < .00001f);
  }
  for (int y = 45; y < 48; y++) {
    assert(transition.join_weight[y * 129 + 70] > 0);
    assert(transition.join_weight[y * 129 + 70] < transition.join_weight[(y + 1) * 129 + 70]);
  }
  assert(transition.join_weight[44 * 129 + 70] == 0);
  CheckJoinClearance(&ground, &scene, &transition);
  assert(SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, 144, &transition));
  assert(transition.join_anchor_count == 5);
  for (int x = 68; x <= 72; x++) {
    float normal[3], metric;
    assert(SimWorldNavigationGlobe_SampleAtRadius(144, x, 48, normal, &metric));
    assert(transition.join_weight[48 * 129 + x] == 1);
    assert(fabsf(transition.join_rise[48 * 129 + x] - (.5f + (x - 68) * .25f) * metric) < .00001f);
  }
  CheckJoinClearance(&ground, &scene, &transition);
  /* Every corner of a reserved or non-rock cell is protected, not just
   * its centre. Rebuilding responds to live occupancy even on rock art. */
  ground.object_rows[1][0] = 1u << 22;
  ground.terrain[1][21] = 8;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.join_anchor_count == 2);
  CheckJoinClearance(&ground, &scene, &transition);
  ground.object_rows[1][0] = 0;
  ground.terrain[1][21] = 0x89;
  scene.atlas[0] = 0; /* Transparent edge may not seed a false plateau. */
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.join_anchor_count == 0);
  scene.atlas[0] = scene.atlas[10];
  ground.enabled_town_mask = 0;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.join_anchor_count == 0);
  CheckJoinClearance(&ground, &scene, &transition);
  /* A completed stamp now reaches two rows beyond the town rectangle.
   * Its protected footprint remains untouched, but the next free rock row
   * receives physical limits from the real opaque edge, not the old border. */
  ground.enabled_town_mask = 2;
  scene.exterior_face_count = 1;
  scene.faces[0].exterior = true;
  scene.faces[0].y[0] = scene.faces[0].y[1] = 46.25f;
  scene.faces[0].y[2] = scene.faces[0].y[3] = 48;
  for (int y = 46; y < 48; y++)
    for (int x = 68; x < 72; x++)
      scene.exterior_cells[y * 128 + x] = scene.replacement[y * 128 + x] = 2;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.continuation_anchor_count >= 5);
  for (int x = 68; x <= 72; x++) {
    float normal[3], metric;
    assert(SimWorldNavigationGlobe_Sample(x, 46.25f, normal, &metric));
    assert(transition.continuation_weight[45 * 129 + x] == 1);
    assert(fabsf(transition.continuation_rise[45 * 129 + x] -
        (.5f + (x - 68) * .25f) * metric) < .00001f);
    for (int y = 46; y <= 48; y++) assert(transition.continuation_weight[y * 129 + x] == 0);
  }
  CheckJoinClearance(&ground, &scene, &transition);
  uint8_t map[128 * 128];
  assert(SimWorldNavigationMountainTransition_BuildAtRadius(&scene, &ground, 144, &transition));
  for (int x = 68; x <= 72; x++) {
    float normal[3], metric;
    assert(SimWorldNavigationGlobe_SampleAtRadius(144, x, 46.25f, normal, &metric));
    assert(transition.continuation_weight[45 * 129 + x] == 1);
    assert(fabsf(transition.continuation_rise[45 * 129 + x] -
        (.5f + (x - 68) * .25f) * metric) < .00001f);
    for (int y = 46; y <= 48; y++) assert(transition.continuation_weight[y * 129 + x] == 0);
  }
  CheckJoinClearance(&ground, &scene, &transition);
  memcpy(map, SimWorldMap_Baseline(), sizeof(map));
  map[44 * 128 + 70] = 0; /* A non-rock cell blocks all four adjacent corners. */
  assert(SimWorldMap_PublishBuiltTilemap(map) == 1);
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.continuation_weight[45 * 129 + 70] == 0);
  CheckJoinClearance(&ground, &scene, &transition);
  scene.atlas[0] = 0;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.continuation_anchor_count == 0);
  scene.atlas[0] = scene.atlas[10];
  ground.enabled_town_mask = 0;
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  assert(transition.continuation_anchor_count == 0);
  SimWorldNavigationMountainTransition_Destroy(&transition);
  SimWorldNavigationMountains_Destroy(&scene);
  SimWorldMap_Shutdown();
}

static void CheckExteriorClearance(const SimWorldNavigationMountainScene *scene) {
  size_t exterior = 0;
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    if (!face->exterior) continue;
    exterior++;
    float lo_x = face->x[0], hi_x = lo_x, lo_y = face->y[0], hi_y = lo_y;
    for (int p = 1; p < 4; p++) {
      lo_x = fminf(lo_x, face->x[p]); hi_x = fmaxf(hi_x, face->x[p]);
      lo_y = fminf(lo_y, face->y[p]); hi_y = fmaxf(hi_y, face->y[p]);
    }
    const int end_x = (int)fmaxf(floorf(lo_x) + 1, ceilf(hi_x));
    const int end_y = (int)fmaxf(floorf(lo_y) + 1, ceilf(hi_y));
    for (int y = (int)floorf(lo_y); y < end_y; y++)
      for (int x = (int)floorf(lo_x); x < end_x; x++) {
        assert(x >= 0 && y >= 0 && x < 128 && y < 128);
        assert(SimWorldMap_MountainCoverage(x, y) >= .18f);
        for (uint8_t town = 1; town <= kSimTownCount; town++) {
          int ox, oy;
          assert(SimWorldMap_OriginForTown(town, &ox, &oy));
          assert(x < ox || x >= ox + 32 || y < oy || y >= oy + 32);
        }
      }
  }
  assert(exterior == scene->exterior_face_count);
}

static void TestExteriorContinuations(void) {
  /* Audited Aitos occupancy already used by the native object tests. The
   * colour is synthetic; geometry must use the independent source stamps. */
  static const uint32_t rows[32] = {
    0xFFFCFFFFu, 0xFFFCFFFFu, 0xFCFCFFFFu, 0xDE3C3C3Fu,
    0xFF003C0Fu, 0xFF003C03u, 0xFF000000u, 0xFF000000u,
    0xC0000300u, 0xC0000780u, 0x00000FC1u, 0x00000FC3u,
    0x80000FC3u, 0xE0006FC3u, 0xF000F003u, 0xF000F003u,
    0xF000F001u, 0xF0000003u, 0xF0000003u, 0xC0000003u,
    0x18000000u, 0x3C000006u, 0x3C00000Fu, 0x3C00000Fu,
    0x1800000Fu, 0x3E000007u, 0x3F00000Fu, 0x3F00000Fu,
    0x3F98001Fu, 0x3FFE667Fu, 0x0FFFFFFFu, 0x0FFFFFFFu,
  };
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom);
  memset(rom + 0x60000, 0xFF, 0x8000);
  rom[0xE3B93 + 15 * 2] = 31;
  memset(rom + 0x70000, 0x44, 64);
  memset(rom + 0x70040, 0x2F, 64);
  assert(SimTownGroundArt_Init(rom, 0x100000));
  assert(SimWorldMap_Init(rom, 0x100000));
  free(rom);
  SimWorldNavigationTownGround ground = {.enabled_town_mask = 8};
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) ground.terrain[3][y * 32 + x] = rows[y] & (1u << x) ? 0x89 : 8;
  SimWorldNavigationMountainScene scene = {0};
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(scene.town_mask == 8 && !scene.exterior_face_count);
  const size_t original_count = scene.face_count;
  SimWorldNavigationMountainFace *original_faces = malloc(original_count * sizeof(*original_faces));
  uint32_t *original_pixels = malloc(512 * 512 * sizeof(*original_pixels));
  assert(original_faces && original_pixels);
  memcpy(original_faces, scene.faces, original_count * sizeof(*original_faces));
  memcpy(original_pixels, scene.atlas, 512 * 512 * sizeof(*original_pixels));
  uint8_t original_replacement[128 * 128];
  memcpy(original_replacement, scene.replacement, sizeof(original_replacement));
  assert(!SimWorldMap_DevelopedAvailable());
  assert(!SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(SimWorldMap_PublishBuiltTilemap(SimWorldMap_Baseline()) == 0);
  assert(SimWorldMap_DevelopedAvailable());
  /* Make a later claimed town invalid, after Aitos has emitted geometry:
   * failed continuation must restore atlas, faces, rise and ownership. */
  scene.town_mask |= 32; ground.enabled_town_mask |= 32;
  const float original_rise = scene.maximum_rise;
  assert(!SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.face_count == original_count && !scene.exterior_face_count && scene.maximum_rise == original_rise);
  assert(!memcmp(original_faces, scene.faces, original_count * sizeof(*original_faces)));
  assert(!memcmp(original_pixels, scene.atlas, 512 * 512 * sizeof(*original_pixels)));
  assert(!memcmp(original_replacement, scene.replacement, sizeof(original_replacement)));
  scene.town_mask = ground.enabled_town_mask = 8;
  assert(SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.exterior_face_count > 100);
  assert(!memcmp(original_faces, scene.faces, original_count * sizeof(*original_faces)));
  CheckExteriorClearance(&scene);
  CheckRearSlopes(&ground, &scene);
  const size_t completed_count = scene.face_count;
  assert(SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.face_count == completed_count); /* no duplicate append */
  /* Cut a one-cell non-rock lane through the added western silhouettes. */
  uint8_t map[128 * 128] = {0};
  map[36 * 128 + 15] = 1;
  assert(SimWorldMap_PublishBuiltTilemap(map) == 1);
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.exterior_face_count > 0 && scene.face_count != completed_count);
  CheckExteriorClearance(&scene);
  CheckRearSlopes(&ground, &scene);
  SimWorldNavigationMountainTransition transition = {0};
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  for (int y = 0; y < 128; y++)
    for (int x = 0; x < 128; x++) {
      if (!scene.exterior_cells[y * 128 + x]) continue;
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
          assert(transition.join_weight[(y + dy) * 129 + x + dx] == 0);
          assert(transition.continuation_weight[(y + dy) * 129 + x + dx] == 0);
        }
    }
  SimWorldNavigationMountainTransition_Destroy(&transition);
  /* No semantic rock: continuation is an exact no-op, including ownership. */
  memset(map, 1, sizeof(map));
  assert(SimWorldMap_PublishBuiltTilemap(map) > 0);
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.face_count == original_count && !scene.exterior_face_count);
  /* A fresh allocation may have different struct padding. Compare every
   * defined field; the earlier append checks compare the retained bytes. */
  for (size_t i = 0; i < original_count; i++) {
    const SimWorldNavigationMountainFace *a = &original_faces[i], *b = &scene.faces[i];
    assert(!memcmp(a->x, b->x, sizeof(a->x)) && !memcmp(a->y, b->y, sizeof(a->y)));
    assert(!memcmp(a->z, b->z, sizeof(a->z)) && !memcmp(a->uv, b->uv, sizeof(a->uv)));
    assert(!memcmp(a->brightness, b->brightness, sizeof(a->brightness)));
    assert(a->town == b->town && a->rear_slope == b->rear_slope && a->exterior == b->exterior);
    assert(a->summit_roof == b->summit_roof);
  }
  assert(!memcmp(original_pixels, scene.atlas, 512 * 512 * sizeof(*original_pixels)));
  assert(!memcmp(original_replacement, scene.replacement, sizeof(original_replacement)));
  SimWorldMap_Shutdown();
  assert(!SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  SimWorldNavigationMountains_Destroy(&scene);
  SimTownGroundArt_Shutdown();
  free(original_faces); free(original_pixels);
}

static void CheckLavaPalette(SimWorldNavigationMountainScene *scene) {
  const size_t count = 512u * 512;
  uint32_t *original = malloc(count * sizeof(*original));
  assert(original && scene->atlas && scene->lava_tiles);
  memcpy(original, scene->atlas, count * sizeof(*original));
  const SimWorldNavigationMountainScene before = *scene;
  const int sx = scene->overhead_crater ? 496 : (scene->lava_variant & 1) * 256;
  const int sy = scene->overhead_crater ? 496 : 112 + (scene->lava_variant >> 1) * 256;
  const int width = scene->overhead_crater ? 16 : 32;
  int previous = -1;
  /* Both turning points, a full repeated cycle, and the 16-bit clock wrap. */
  for (unsigned step = 0; step < 132; step++) {
    const uint16_t frame = step < 128 ? (uint16_t)(step + 1) : (uint16_t)(step - 130);
    const unsigned phase = (uint16_t)(frame - 1) % 64;
    const unsigned red = phase < 32 ? phase : 63 - phase;
    const uint32_t color = 0xFF000000u | (((red << 3) | (red >> 2)) << 16);
    SimWorldNavigationMountainAtlasUpdate update;
    assert(SimWorldNavigationMountains_UpdateLava(scene, frame, &update) ==
        (previous != (int)red));
    if (previous != (int)red) {
      assert(update.x == sx && update.y == sy && update.width == width && update.height == 16);
    } else assert(update.width == 0);
    for (int y = 0; y < 512; y++)
      for (int x = 0; x < 512; x++) {
        const int dx = x - sx, dy = y - sy;
        const bool lava = dx >= 0 && dx < width && dy >= 0 && dy < 16 &&
            scene->lava_mask[dx / 16][dy * 16 + dx % 16];
        assert(scene->atlas[y * 512 + x] == (lava ? color : original[y * 512 + x]));
      }
    assert(!SimWorldNavigationMountains_UpdateLava(scene, frame, &update));
    assert(update.width == 0); /* Frozen clock produces no upload. */
    previous = (int)red;
  }
  SimWorldNavigationMountainScene after = *scene;
  after.lava_red = before.lava_red;
  after.lava_ready = before.lava_ready;
  assert(!memcmp(&before, &after, sizeof(before))); /* Geometry/ownership untouched. */
  free(original);
}

static void TestLavaPalette(void) {
  for (int variant = 0; variant < 2; variant++) {
    SimWorldNavigationMountainScene scene = {0};
    scene.atlas = malloc(512u * 512 * sizeof(*scene.atlas));
    assert(scene.atlas);
    for (unsigned p = 0; p < 512u * 512; p++) scene.atlas[p] = 0xFF810000u;
    scene.lava_variant = (uint8_t)variant;
    scene.lava_tiles = 3;
    scene.lava_mask[0][3] = scene.lava_mask[0][250] = scene.lava_mask[1][100] = 1;
    CheckLavaPalette(&scene);
    scene.lava_tiles = 0;
    SimWorldNavigationMountainAtlasUpdate update = {1, 2, 3, 4};
    assert(!SimWorldNavigationMountains_UpdateLava(&scene, 20, &update));
    assert(update.width == 0);
    SimWorldNavigationMountains_Destroy(&scene);
  }
}

static int CompareCraterShade(const void *a, const void *b) {
  const uint32_t left = *(const uint32_t *)a, right = *(const uint32_t *)b;
  return (left > right) - (left < right);
}

static void CheckOverheadCrater(const SimWorldNavigationMountainScene *scene,
                                const uint8_t *rom, uint8_t tier) {
  assert(scene->overhead_crater && scene->lava_tiles == 1 && !scene->lava_ready);
  const uint8_t tiles[4] = {0xA6, 0xA7, 0xB6, 0xB7};
  const uint32_t *rock = SimTownGroundArt_Metatile(4, tier, 0x89);
  const uint32_t *right_rock = SimTownGroundArt_Metatile(4, tier, 0x8A);
  assert(rock && right_rock);
  uint32_t shades[256];
  unsigned shades_count = 0;
  for (unsigned p = 0; p < 256; ++p) if (rock[p] >> 24) shades[shades_count++] = rock[p];
  assert(shades_count);
  qsort(shades,shades_count,sizeof(*shades),CompareCraterShade);
  unsigned unique = 1;
  for (unsigned p = 1; p < shades_count; ++p)
    if (shades[p] != shades[unique-1]) shades[unique++] = shades[p];
  unsigned lava = 0, blended = 0, matched_edges = 0;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      const int tile = tiles[y / 8 * 2 + x / 8], p = y % 8 * 8 + x % 8;
      const uint8_t index = rom[0x70000 + tile * 64 + p];
      const uint32_t actual = scene->atlas[(496 + y) * 512 + 496 + x];
      assert(scene->lava_mask[0][y * 16 + x] == (index == 0));
      assert(!scene->lava_mask[1][y * 16 + x]);
      lava += index == 0;
      if (index >= 0x40 && index <= 0x45) {
        const uint32_t authored = shades[(index-0x40)*(unique-1)/5];
        const unsigned column = (x*31+7)/15;
        const uint32_t slope = (column < 16 ? rock : right_rock)[column%16];
        const double radius = hypot((x-7.5)/7.5,(y-7.5)/7.5);
        const double t = fmin(1,fmax(0,(1-radius)/.25));
        const double weight = t*t*(3-2*t);
        assert(actual >> 24 == 255); /* Rounded colour, never a cutout hole. */
        for (unsigned shift = 0; shift < 24; shift += 8) {
          const double a = (slope >> shift)&255u, b = (authored >> shift)&255u;
          const int expected = (int)floor(a+(b-a)*weight+.5);
          assert(abs((int)((actual>>shift)&255u)-expected) <= 1);
        }
        if (!x || x == 15 || !y || y == 15) {
          assert(actual == (slope | 0xff000000u));
          ++matched_edges;
        }
        blended += actual != authored;
      } else {
        const unsigned bgr = rom[0xE3F93 + index * 2] | rom[0xE3F94 + index * 2] << 8;
        const unsigned r = bgr & 31, g = bgr >> 5 & 31, b = bgr >> 10 & 31;
        const uint32_t expected = 0xFF000000u | ((r << 3 | r >> 2) << 16) |
            ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
        assert(actual == expected); /* Original overhead rim and opening. */
      }
    }
  assert(lava > 0 && lava < 256);
  assert(blended > 20 && matched_edges == 60);
  printf("rounded crater rock: %u texels blended, %u perimeter texels match native crest; rim/opening unchanged\n",
      blended,matched_edges);
  double area = 0;
  unsigned roof_faces = 0, cap_faces = 0;
  for (size_t at = 0; at < scene->face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene->faces[at];
    if (face->town != 4) { assert(!face->summit_roof); continue; }
    float u = 0, v = 0;
    for (int p = 0; p < 4; p++) { u += face->uv[p].x * 128; v += face->uv[p].y * 128; }
    /* No front, wall or rear can still display either old angled opening. */
    const int tile = ((int)v / 16 % 16) * 16 + (int)u / 16 % 16;
    assert(tile != 0x70 && tile != 0x71);
    if (face->summit_roof) {
      roof_faces++;
      assert(face->rear_slope && !face->exterior);
    }
    if (u < 496 || v < 496) continue;
    cap_faces++;
    assert(face->summit_roof);
    double signed_area = 0;
    for (int p = 0; p < 4; p++) {
      assert(fabsf(face->z[p] - 1.68f) < .00001f);
      assert(face->brightness[p] == (p < 2 ? 255 : 235));
      assert(face->uv[p].x * 512 >= 496.5f && face->uv[p].x * 512 <= 511.5f);
      assert(face->uv[p].y * 512 >= 496.5f && face->uv[p].y * 512 <= 511.5f);
      signed_area += (double)face->x[p] * face->y[(p + 1) % 4] -
          (double)face->y[p] * face->x[(p + 1) % 4];
    }
    area += fabs(signed_area) * .5 * 256;
  }
  /* Exactly one slimmer 32x14 summit, not a second cap on the back or duplicate
   * coincident polygons. Other roof strips carry native shoulder art. */
  assert(cap_faces == 32 && roof_faces > cap_faces);
  assert(fabs(area - 32 * 14) < .01);
  printf("native Aitos overhead crater: %u lava texels, %u cap faces, %.2f pixel area\n",
      lava, cap_faces, area);
}

static void CheckCompactVolcanoGroundAndSeal(const SimWorldNavigationTownGround *ground,
                                            const SimWorldNavigationMountainScene *scene) {
  /* Each inferred roof strip must still meet an actual rear-slope vertex,
   * not merely another point on an independently approximated silhouette. */
  unsigned sealed = 0;
  for (size_t at = 0; at < scene->face_count; ++at) {
    const SimWorldNavigationMountainFace *roof = &scene->faces[at];
    if (!roof->summit_roof) continue;
    for (int p = 2; p < 4; ++p) {
      if (roof->z[p] <= 0) continue;
      bool joined = false;
      for (size_t rear_at = 0; rear_at < scene->face_count && !joined; ++rear_at) {
        const SimWorldNavigationMountainFace *rear = &scene->faces[rear_at];
        if (!rear->rear_slope || rear->summit_roof || rear->town != roof->town) continue;
        for (int q = 0; q < 4; ++q)
          if (fabsf(roof->x[p]-rear->x[q]) < .00001f &&
              fabsf(roof->y[p]-rear->y[q]) < .00001f && fabsf(roof->z[p]-rear->z[q]) < .00001f) {
            assert(roof->brightness[p] == rear->brightness[q]);
            joined = true;
          }
      }
      assert(joined);
      ++sealed;
    }
  }
  assert(sealed >= 64);

  /* The two old crown-source cells are exposed by the shorter rear. Keep
   * source cleanup ownership there, even though the mesh foot moved inward.
   * Both full and dirty-cell refreshes must restore native grass, not the old
   * overhead crater/rock; adjacent pixels must remain untouched. */
  int ox,oy; assert(SimWorldMap_OriginForTown(4,&ox,&oy));
  const uint32_t *plain = SimTownGroundArt_Metatile(4,ground->development_tier[3],8);
  uint32_t *pixels = malloc(2048u*2048*sizeof(*pixels));
  assert(pixels && plain);
  uint8_t dirty[kSimWorldMapBytes] = {0};
  const int cells[][2] = {{8,8}, {9,8}, {13,13}, {14,13}, {27,20}, {28,20}};
  for (unsigned i = 0; i < sizeof(cells)/sizeof(cells[0]); ++i) {
    const int at = (oy+cells[i][1])*128+ox+cells[i][0];
    assert(scene->replacement[at] == 4);
    dirty[at] = 1;
  }
  for (int incremental = 0; incremental < 2; ++incremental) {
    memset(pixels,0x5a,2048u*2048*sizeof(*pixels));
    assert(SimWorldNavigationMountains_ClearGround(pixels,2048,ground,8,incremental ? dirty : NULL));
    for (unsigned i = 0; i < sizeof(cells)/sizeof(cells[0]); ++i)
      for (int py = 0; py < 16; ++py) for (int px = 0; px < 16; ++px) {
      assert(plain[py*16+px] >> 24);
      assert(pixels[((oy+cells[i][1])*16+py)*2048+(ox+cells[i][0])*16+px] == plain[py*16+px]);
    }
    assert(pixels[0] == 0x5a5a5a5au);
  }
  free(pixels);
  assert(SimWorldNavigationTerrain_RebuildWorldPrior(SimWorldMap_BakedPixels(),1024,
      SimWorldMap_GeographySerial()));
  SimWorldNavigationTerrain_SetMountainReplacement(scene->replacement);
  for (unsigned i = 0; i < sizeof(cells)/sizeof(cells[0]); ++i)
    for (int x = 0; x <= 4; ++x) for (int y = 0; y <= 4; ++y) {
    const float wx = ox+cells[i][0]+x*.25f, wy = oy+cells[i][1]+y*.25f;
    assert(fabsf(SimWorldNavigationTerrain_HeightUnits(wx,wy) -
        SimWorldNavigationTerrain_FloorHeightUnits(wx,wy)) < .00001f);
  }
  SimWorldNavigationTerrain_SetMountainReplacement(NULL);
  printf("compact volcano: %u roof/rear joins sealed; vacated crown-source cells restore grass and floor height\n",sealed);
}

static void TestCapturedMountainScene(const char *rom_path, const char *wram_path) {
  uint8_t *rom = malloc(0x100000), *wram = malloc(0x20000);
  assert(rom && wram);
  FILE *file = fopen(rom_path, "rb");
  assert(file && fread(rom, 1, 0x100000, file) == 0x100000);
  fclose(file);
  file = fopen(wram_path, "rb");
  assert(file && fread(wram, 1, 0x20000, file) == 0x20000);
  fclose(file);
  assert(SimTownGroundArt_Init(rom, 0x100000));
  assert(SimWorldMap_Init(rom, 0x100000));
  SimWorldMap_PublishBuiltTilemap(wram + 0xC000);
  SimWorldNavigationTownGround ground = {0};
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    ground.development_tier[town - 1] = wram[0x16B18 + (town - 1) * 2];
    if (!ground.development_tier[town - 1]) continue;
    ground.enabled_town_mask |= 1u << (town - 1);
    for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++)
        ground.terrain[town - 1][y * 32 + x] = wram[SimTownLayout_CellMapIndex(town, x, y)];
  }
  SimWorldNavigationMountainScene scene = {0};
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  uint8_t expected = 0;
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(ground.enabled_town_mask & (1u << (town - 1)))) continue;
    SimBackgroundMountainField field;
    SimBackgroundMountains_ClassifyCells(town, ground.terrain[town - 1], &field);
    if (field.cell_count) expected |= 1u << (town - 1);
  }
  assert(scene.town_mask == expected);
  CheckOverheadCrater(&scene, rom, ground.development_tier[3]);
  CheckCompactVolcanoGroundAndSeal(&ground,&scene);
  CheckLavaPalette(&scene);
  CheckRearSlopes(&ground, &scene);
  assert(wram[0x18] == 0 && wram[0x19] == 9);
  const size_t original_count = scene.face_count;
  SimWorldNavigationMountainFace *original_faces = malloc(original_count * sizeof(*original_faces));
  assert(original_faces);
  memcpy(original_faces, scene.faces, original_count * sizeof(*original_faces));
  assert(SimWorldNavigationMountains_ContinueEdges(&ground, &scene));
  assert(scene.exterior_face_count > 0);
  assert(!memcmp(original_faces, scene.faces, original_count * sizeof(*original_faces)));
  free(original_faces);
  printf("native exterior continuations: %zu faces\n", scene.exterior_face_count);
  CheckExteriorClearance(&scene);
  CheckRearSlopes(&ground, &scene);
  const char *audit_path = getenv("AR_TEST_MOUNTAIN_AUDIT");
  if (audit_path) {
    FILE *audit = fopen(audit_path, "w");
    assert(audit);
    for (size_t at = 0; at < scene.face_count; at++) {
      const SimWorldNavigationMountainFace *face = &scene.faces[at];
      fprintf(audit, "%u %u %u %u", face->town, face->rear_slope, face->summit_roof, face->exterior);
      for (int p = 0; p < 4; p++)
        fprintf(audit, " %.9g %.9g %.9g %.9g %.9g %u", face->x[p], face->y[p], face->z[p],
            face->uv[p].x, face->uv[p].y, face->brightness[p]);
      fputc('\n', audit);
    }
    assert(!ferror(audit) && fclose(audit) == 0);
  }
  assert(SimWorldNavigationTerrain_RebuildWorldPrior(SimWorldMap_BakedPixels(), 1024,
      SimWorldMap_GeographySerial()));
  SimWorldNavigationMountainTransition transition = {0};
  assert(SimWorldNavigationMountainTransition_Build(&scene, &ground, &transition));
  SimWorldNavigationTerrain_SetMountainReplacement(scene.replacement);
  SimWorldNavigationTerrain_SetMountainTransition(transition.ridge_scale);
  size_t joined = 0;
  for (int i = 0; i < 129 * 129; i++) joined += transition.join_weight[i] > 0;
  printf("native boundary joins: %zu anchors, %zu supported vertices\n",
      transition.join_anchor_count, joined);
  size_t limited = 0;
  for (int i = 0; i < 129 * 129; i++) limited += transition.continuation_weight[i] > 0;
  printf("continued edge limits: %zu anchors, %zu supported vertices\n",
      transition.continuation_anchor_count, limited);
  size_t boundary = 0, rock_boundary = 0, free_rock = 0;
  for (int y = 1; y < 128; y++)
    for (int x = 1; x < 128; x++) {
      int continued = 0;
      bool rock = true, outside = true;
      for (int dy = -1; dy <= 0; dy++)
        for (int dx = -1; dx <= 0; dx++) {
          const int cx = x + dx, cy = y + dy;
          continued += scene.exterior_cells[cy * 128 + cx] != 0;
          rock &= SimWorldMap_MountainCoverage(cx, cy) >= .18f;
          for (uint8_t town = 1; town <= kSimTownCount; town++) {
            int ox, oy; SimWorldMap_OriginForTown(town, &ox, &oy);
            outside &= cx < ox || cx >= ox + 32 || cy < oy || cy >= oy + 32;
          }
        }
      boundary += continued > 0 && continued < 4;
      rock_boundary += continued > 0 && continued < 4 && rock && outside;
      free_rock += !continued && rock && outside;
    }
  printf("continued footprint audit: %zu boundary vertices, %zu exterior all-rock boundaries, %zu free rock vertices\n",
      boundary, rock_boundary, free_rock);
  assert(transition.continuation_anchor_count > 0 && limited > 0);
  CheckJoinClearance(&ground, &scene, &transition);
  size_t raised = 0;
  float maximum_lift = 0;
  for (size_t at = 0; at < scene.face_count; at++)
    for (int p = 0; p < 4; p++) {
      const SimWorldNavigationMountainFace *face = &scene.faces[at];
      const float lift = SimWorldNavigationTerrain_HeightUnits(face->x[p], face->y[p]) -
          SimWorldNavigationTerrain_FloorHeightUnits(face->x[p], face->y[p]);
      raised += lift > .0001f;
      maximum_lift = fmaxf(maximum_lift, lift);
    }
  printf("native-vertex inferred lift: %zu vertices, max %.4f world units\n", raised, maximum_lift);
  SimWorldNavigationTerrain_SetMountainJoin(transition.join_rise, transition.join_weight, 1);
  for (int y = 0; y <= 128; y++)
    for (int x = 0; x <= 128; x++) {
      const int i = y * 129 + x;
      if (transition.join_weight[i] == 1)
        assert(fabsf(SimWorldNavigationTerrain_HeightUnits(x, y) -
            SimWorldNavigationTerrain_FloorHeightUnits(x, y) - transition.join_rise[i]) < .00001f);
    }
  SimWorldNavigationTerrain_SetMountainJoin(NULL, NULL, 0);
  float original_height[129 * 129];
  for (int y = 0; y <= 128; y++)
    for (int x = 0; x <= 128; x++) original_height[y * 129 + x] = SimWorldNavigationTerrain_HeightUnits(x, y);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(
      transition.continuation_rise, transition.continuation_weight, 1);
  size_t lowered = 0;
  float maximum_lowering = 0;
  for (int y = 0; y <= 128; y++)
    for (int x = 0; x <= 128; x++) {
      const int i = y * 129 + x;
      const float height = SimWorldNavigationTerrain_HeightUnits(x, y);
      assert(height <= original_height[i]);
      if (!transition.continuation_weight[i]) assert(height == original_height[i]);
      lowered += height < original_height[i];
      maximum_lowering = fmaxf(maximum_lowering, original_height[i] - height);
    }
  printf("continued edge relief: %zu vertices lowered, maximum %.4f world units\n", lowered, maximum_lowering);
  assert(lowered > 0);
  SimWorldNavigationTerrain_SetMountainContinuationLimit(NULL, NULL, 0);
  SimWorldNavigationTerrain_SetMountainReplacement(NULL);
  SimWorldNavigationTerrain_SetMountainTransition(NULL);
  SimWorldNavigationMountainTransition_Destroy(&transition);
  /* Kasandora's reconstructed northern peaks reach Aitos's southern edge.
   * Reserve that neighbouring row as buildings and prove their rear faces
   * are cut away even though the originating town's own masks are unchanged. */
  size_t crossing = 0;
  for (size_t at = 0; at < scene.face_count; at++) {
    const SimWorldNavigationMountainFace *face = &scene.faces[at];
    if (!face->rear_slope || face->exterior || face->town != 3) continue;
    float lo = face->y[0], hi = lo;
    for (int p = 1; p < 4; p++) { lo = fminf(lo, face->y[p]); hi = fmaxf(hi, face->y[p]); }
    crossing += lo < 64 && hi > 63;
  }
  assert(crossing > 0);
  ground.object_rows[3][31] = UINT32_MAX;
  /* Also reserve a cell underneath the new summit/rear. The cap must
   * obey the same polygon-level building exclusion as every rear face. */
  ground.object_rows[3][9] |= 1u << 8;
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(scene.overhead_crater);
  CheckRearClearance(&ground, &scene);
  printf("protected Aitos border from %zu crossing Kasandora rear faces\n", crossing);
  SimWorldMap_Shutdown();
  assert(SimWorldNavigationMountains_Build(&ground, &scene));
  assert(!scene.overhead_crater && scene.lava_tiles == 3);
  unsigned legacy_lava = 0;
  for (int tile = 0; tile < 2; tile++)
    for (int p = 0; p < 256; p++) legacy_lava += scene.lava_mask[tile][p] != 0;
  assert(legacy_lava == 51); /* No overhead provider: safe native-art fallback. */
  CheckLavaPalette(&scene);
  SimWorldNavigationMountains_Destroy(&scene);
  SimTownGroundArt_Shutdown();
  free(wram);
  free(rom);
}

static void TestSkyCloudVolume(void) {
  const int pitch = kSimSkyCloudAtlasWidth + 3;
  const size_t bytes = (size_t)pitch * kSimSkyCloudAtlasHeight * sizeof(uint32_t);
  uint32_t *lit = malloc(bytes), *flat = malloc(bytes), *again = malloc(bytes);
  assert(lit && flat && again);
  memset(lit, 0xa5, bytes); memset(flat, 0xa5, bytes); memset(again, 0xa5, bytes);
  const float light[3] = {-.4f, -.8f, -.3f};
  assert(SimWorldNavigationSkyClouds_Bake(lit, pitch, light, true));
  assert(SimWorldNavigationSkyClouds_Bake(flat, pitch, light, false));
  assert(SimWorldNavigationSkyClouds_Bake(again, pitch, light, true));
  assert(!memcmp(lit, again, bytes));
  size_t covered = 0, shaded = 0;
  for (int y = 0; y < kSimSkyCloudAtlasHeight; y++)
    for (int x = 0; x < pitch; x++) {
      const size_t at = (size_t)y * pitch + x;
      if (x >= kSimSkyCloudAtlasWidth) { assert(lit[at] == 0xa5a5a5a5u); continue; }
      const unsigned alpha = lit[at] >> 24;
      assert(alpha == flat[at] >> 24);
      if (x % kSimSkyCloudWidth == 0 || x % kSimSkyCloudWidth == kSimSkyCloudWidth - 1 ||
          y % kSimSkyCloudHeight == 0 || y % kSimSkyCloudHeight == kSimSkyCloudHeight - 1)
        assert(!alpha);
      covered += alpha > 40;
      shaded += alpha > 40 && lit[at] != flat[at];
      /* Alpha-zero texels carry white RGB to prevent black filter fringes. */
      if (!alpha) assert((lit[at] & 0xffffffu) == 0xffffffu);
    }
  assert(covered > 1000 && shaded > 1000);
  assert(!SimWorldNavigationSkyClouds_Bake(lit, pitch, (float[3]){NAN, 0, 1}, true));
  assert(!SimWorldNavigationSkyClouds_Bake(lit, pitch, (float[3]){0}, true));
  assert(!SimWorldNavigationSkyClouds_Bake(lit, kSimSkyCloudAtlasWidth - 1, light, true));
  assert(!SimWorldNavigationSkyClouds_Bake(NULL, pitch, light, true));
  assert(!memcmp(lit, again, bytes));
  free(lit); free(flat); free(again);
}

int main(int argc, char **argv) {
  TestMountainTileSampling();
  TestSkyCloudVolume();
  TestCloudBakeExact();
  TestCloudSphere();
  TestCloudCharts();
  TestSharedMesh();
  TestNativeMountainScene();
  TestMountainTransition();
  TestMountainGeometryJoin();
  TestExteriorContinuations();
  TestLavaPalette();
  if (argc == 3) TestCapturedMountainScene(argv[1], argv[2]);
  return 0;
}
