/* CPU-only picking contracts for real projected quad observations. */
#include "voxel_pick.h"
#include "diorama/diorama_render_options.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>

static void Near(float actual, float expected) {
  assert(fabsf(actual - expected) < 0.0001f);
}

static DioramaVoxelPickFace Quad(void) {
  return (DioramaVoxelPickFace){
    .screen_xy = {10, 20, 110, 20, 110, 120, 10, 120},
    .world_xy = {32, 48, 48, 48, 48, 64, 32, 64},
    .depth = {.1f, .3f, .9f, .7f}, .clip_w = {1, 1, 1, 1}};
}

int main(void) {
  DioramaVoxelPickFace face = Quad();
  EditorVoxelPickHit hit = {0};
  /* Both triangles, outer edges, and their shared diagonal remain selectable. */
  assert(EditorVoxelPick_Hit(&face, 85, 45, &hit));
  Near(hit.world_x, 44); Near(hit.world_y, 52); Near(hit.depth, .4f);
  assert(EditorVoxelPick_Hit(&face, 35, 95, &hit));
  Near(hit.world_x, 36); Near(hit.world_y, 60); Near(hit.depth, .6f);
  assert(EditorVoxelPick_Hit(&face, 60, 70, &hit));
  Near(hit.world_x, 40); Near(hit.world_y, 56); Near(hit.depth, .5f);
  assert(EditorVoxelPick_Hit(&face, 10, 20, &hit));
  Near(hit.world_x, 32); Near(hit.world_y, 48);
  assert(EditorVoxelPick_Hit(&face, 110, 120, &hit));
  assert(!EditorVoxelPick_Hit(&face, 111, 80, &hit));
  assert(!EditorVoxelPick_Hit(&face, 50, 19, &hit));

  /* Camera rotation changes homogeneous W. Native coordinates require its
   * inverse, but using it for NDC depth would yield a wrong occlusion order. */
  face.clip_w[2] = 4;
  assert(EditorVoxelPick_Hit(&face, 60, 70, &hit));
  Near(hit.world_x, 35.2f); Near(hit.world_y, 51.2f); Near(hit.depth, .5f);

  /* Mirrored views reverse winding without reversing native source mapping. */
  for (int i = 0; i < 4; ++i) face.screen_xy[i * 2] = 120 - face.screen_xy[i * 2];
  assert(EditorVoxelPick_Hit(&face, 60, 70, &hit));
  Near(hit.world_x, 35.2f); Near(hit.world_y, 51.2f);

  /* A cap can occupy screen area while mapping to a constant native row. */
  face = Quad();
  for (int i = 0; i < 4; ++i) face.world_xy[i * 2 + 1] = 64;
  assert(EditorVoxelPick_Hit(&face, 35, 95, &hit));
  Near(hit.world_x, 36); Near(hit.world_y, 64);
  /* Narrow but noncollapsed geometry must not disappear from picking. */
  for (int i = 0; i < 4; ++i) face.screen_xy[i * 2 + 1] *= .00001f;
  assert(EditorVoxelPick_Hit(&face, 35, .00095f, &hit));

  const EditorVoxelPickHit unchanged = {123, 456, .75f};
  hit = unchanged;
  face = Quad(); face.clip_w[3] = 0;
  assert(!EditorVoxelPick_Hit(&face, 85, 45, &hit));
  face = Quad(); face.clip_w[3] = -1;
  assert(!EditorVoxelPick_Hit(&face, 85, 45, &hit));
  face = Quad(); face.world_xy[7] = NAN;
  assert(!EditorVoxelPick_Hit(&face, 85, 45, &hit));
  face = Quad(); face.screen_xy[6] = INFINITY;
  assert(!EditorVoxelPick_Hit(&face, 85, 45, &hit));
  face = Quad(); face.depth[0] = NAN;
  assert(!EditorVoxelPick_Hit(&face, 85, 45, &hit));
  face = Quad();
  for (int i = 0; i < 4; ++i) face.screen_xy[i * 2 + 1] = 20;
  assert(!EditorVoxelPick_Hit(&face, 35, 20, &hit));
  face = Quad();
  assert(!EditorVoxelPick_Hit(&face, NAN, 45, &hit));
  assert(!EditorVoxelPick_Hit(NULL, 85, 45, &hit));
  assert(!EditorVoxelPick_Hit(&face, 85, 45, NULL));
  Near(hit.world_x, unchanged.world_x); Near(hit.world_y, unchanged.world_y);
  Near(hit.depth, unchanged.depth);
  puts("action editor voxel picking: passed");
  return 0;
}
