#include "sim_world_navigation_cliffs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sim_town_terrain.h"
#include "sim_world_navigation_terrain.h"

enum { kCells = kSimWorldMapTiles, kMaxFaces = kCells * kCells * 5 };

void SimWorldNavigationCliffs_Destroy(SimWorldNavigationCliffScene *scene) {
  if (!scene) return;
  free(scene->faces);
  memset(scene, 0, sizeof(*scene));
}

static SimWorldNavigationCliffFace *Append(SimWorldNavigationCliffScene *scene) {
  if (scene->face_count == scene->face_capacity) {
    size_t capacity = scene->face_capacity ? scene->face_capacity * 2 : 256;
    if (capacity > kMaxFaces) capacity = kMaxFaces;
    if (capacity <= scene->face_count) return NULL;
    void *faces = realloc(scene->faces, capacity * sizeof(*scene->faces));
    if (!faces) return NULL;
    scene->faces = faces;
    scene->face_capacity = capacity;
  }
  return &scene->faces[scene->face_count++];
}

static void Corners(const SimWorldNavigationCliffScene *scene, int x, int y,
                    float height[4]) {
  const unsigned cap = scene->replacement[y * kCells + x];
  if (cap) {
    memcpy(height, scene->faces[cap - 1].height, 4 * sizeof(float));
    return;
  }
  static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
  for (int p = 0; p < 4; p++)
    height[p] = SimWorldNavigationTerrain_HeightUnits(x + dx[p], y + dy[p]);
}

static SimTownTerrainFaceKind FaceKind(uint8_t mask, int x, int y) {
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int ox, oy;
    if ((mask & (1u << (town - 1))) && SimWorldMap_OriginForTown(town, &ox, &oy) &&
        x >= ox && x < ox + kSimTownCells && y >= oy && y < oy + kSimTownCells)
      return SimTownTerrain_FaceKind(town, x - ox, y - oy);
  }
  return kSimTownTerrainFace_None;
}

static bool Skirt(SimWorldNavigationCliffScene *scene,
                  int x, int y, int nx, int ny,
                  float x0, float y0, float x1, float y1,
                  const float top[2], const float bottom[2]) {
  float t0, t1;
  if (!SimTownTerrain_ClipVisibleHigherEdge(
          top[0], top[1], bottom[0], bottom[1], &t0, &t1)) return true;
  /* Same material ownership as the full town: borrow the face, not the
   * plateau grass. A cave's jamb is rock; its centre is a dark aperture. */
  SimTownTerrainFaceKind kind = FaceKind(scene->town_mask, x, y);
  const SimTownTerrainFaceKind neighbour = FaceKind(scene->town_mask, nx, ny);
  if (kind == kSimTownTerrainFace_None && neighbour != kSimTownTerrainFace_None) {
    x = nx; y = ny; kind = neighbour;
  }
  const float u = (x + (kind == kSimTownTerrainFace_Cave ? 2.5f / 16 : .5f)) / kCells;
  const float v = (y + (kind == kSimTownTerrainFace_Cave ? 8.5f / 16 : .5f)) / kCells;
  SimWorldNavigationCliffFace *face = Append(scene);
  if (!face) return false;
  const float t[2] = {t0, t1};
  for (int p = 0; p < 2; p++) {
    face->x[p] = face->x[3 - p] = x0 + (x1 - x0) * t[p];
    face->y[p] = face->y[3 - p] = y0 + (y1 - y0) * t[p];
    face->height[p] = top[0] + (top[1] - top[0]) * t[p];
    face->height[3 - p] = bottom[0] + (bottom[1] - bottom[0]) * t[p];
  }
  for (int p = 0; p < 4; p++) { face->u[p] = u; face->v[p] = v; }
  face->shade = x0 == x1 ? .86f : .82f;
  return true;
}

bool SimWorldNavigationCliffs_Build(uint8_t town_mask, SimWorldNavigationCliffScene *scene) {
  if (!scene) return false;
  SimWorldNavigationCliffs_Destroy(scene);
  scene->town_mask = town_mask & ((1u << kSimTownCount) - 1);
  static const int dx[4] = {0, 1, 1, 0}, dy[4] = {0, 0, 1, 1};
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(scene->town_mask & (1u << (town - 1)))) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    for (int cy = 0; cy < kSimTownCells; cy++)
      for (int cx = 0; cx < kSimTownCells; cx++) {
        const int x = ox + cx, y = oy + cy;
        float height[4], shared[4];
        if (!SimWorldNavigationTerrain_TownCellCorners(town, cx, cy, height)) goto fail;
        Corners(scene, x, y, shared);
        bool changed = SimTownTerrain_IsFaceCell(town, cx, cy);
        for (int p = 0; p < 4; p++) changed |= fabsf(height[p] - shared[p]) > .000001f;
        if (!changed) continue;
        SimWorldNavigationCliffFace *face = Append(scene);
        if (!face) goto fail;
        scene->replacement[y * kCells + x] = (uint16_t)scene->face_count;
        for (int p = 0; p < 4; p++) {
          face->x[p] = x + dx[p]; face->y[p] = y + dy[p];
          face->height[p] = height[p];
          /* Half-texel inset prevents the other side's grass bleeding onto
           * the rock cap when magnified. Geometry still meets exactly. */
          face->u[p] = (face->x[p] + (dx[p] ? -.5f : .5f) / kSimTownCellPixels) / kCells;
          face->v[p] = (face->y[p] + (dy[p] ? -.5f : .5f) / kSimTownCellPixels) / kCells;
        }
        face->shade = 1;
      }
  }
  /* Inspect each undirected neighbour edge once. Clipping in both height
   * directions closes crossing edges without self-intersecting bow ties. */
  for (int y = 0; y < kCells; y++)
    for (int x = 0; x < kCells; x++)
      for (int side = 0; side < 2; side++) {
        const int nx = x + (side == 0), ny = y + (side == 1);
        if (nx >= kCells || ny >= kCells ||
            (!scene->replacement[y * kCells + x] && !scene->replacement[ny * kCells + nx]))
          continue;
        float a[4], b[4];
        Corners(scene, x, y, a); Corners(scene, nx, ny, b);
        const float top[2] = {a[side ? 3 : 1], a[2]};
        const float bottom[2] = {b[0], b[side ? 1 : 3]};
        const float x0 = x + (side == 0), y0 = y + (side == 1);
        const float x1 = x + 1, y1 = y + 1;
        if (!Skirt(scene, x, y, nx, ny, x0, y0, x1, y1, top, bottom) ||
            !Skirt(scene, nx, ny, x, y, x0, y0, x1, y1, bottom, top)) goto fail;
      }
  return true;
fail:
  SimWorldNavigationCliffs_Destroy(scene);
  return false;
}
