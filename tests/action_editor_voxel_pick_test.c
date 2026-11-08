/* CPU-only picking contracts for real projected quad observations. */
#include "voxel_pick.h"
#include "diorama/diorama_render_options.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static void Near(float actual, float expected) {
  assert(fabsf(actual - expected) < 0.0001f);
}

static DioramaVoxelPickFace Quad(void) {
  return (DioramaVoxelPickFace){
    .screen_xy = {10, 20, 110, 20, 110, 120, 10, 120},
    .world_xy = {32, 48, 48, 48, 48, 64, 32, 64},
    .depth = {.1f, .3f, .9f, .7f}, .clip_w = {1, 1, 1, 1}};
}

static void CompleteOutline(const ArRenderPointF *observed,unsigned count) {
  ArRenderPointF points[32],hull[64];assert(count<=32);
  memcpy(points,observed,count*sizeof(*points));
  const unsigned n=EditorVoxelPick_Hull(points,count,hull);assert(n>=3);
  for(unsigned p=0;p<count;++p)for(unsigned i=0;i<n;++i) {
    const ArRenderPointF a=hull[i],b=hull[(i+1)%n],c=observed[p];
    const double cross=((double)b.x-a.x)*((double)c.y-a.y)-((double)b.y-a.y)*((double)c.x-a.x);
    assert(cross>=-1e-8); /* Every submitted face remains inside its outline. */
  }
}
static void SegmentedOutlineCorners(void) {
  /* Actual clipped preview vertices: Marahna1 cell42,19 left; Marahna2
   * cell27,39 right; Northwall1 cell48,30 left; Northwall2 cell38,15 left.
   * A one-ULP x difference makes a 15px support corner's cross product only
   * .00048. The former absolute .001 threshold silently removed that corner. */
  static const ArRenderPointF jungle[]={
    {480.f,297.894745f},{480.000031f,282.247864f},{472.746704f,276.444336f},
    {472.766418f,292.126312f},{480.f,313.45697f},{472.786011f,307.723389f},
    {454.655029f,261.968719f},{454.72406f,277.738403f},{454.792725f,293.422424f},
    {460.836273f,298.225769f}};
  static const ArRenderPointF temple[]={
    {489.142975f,306.190216f},{489.118286f,321.712311f},{509.844879f,305.293701f},
    {509.926025f,289.671997f},{489.093658f,337.150665f},{509.76416f,320.830933f},
    {480.f,313.45697f},{480.f,328.935272f},{479.999969f,344.330261f}};
  static const ArRenderPointF snow[]={
    {472.766418f,292.126312f},{472.746704f,276.444336f},{454.655029f,261.968719f},
    {454.72406f,277.738403f},{472.786011f,307.723389f},{454.792725f,293.422424f},
    {480.f,297.894745f},{480.000031f,282.247864f},{480.f,313.45697f}};
  static const ArRenderPointF ice[]={
    {471.475189f,291.096588f},{471.451965f,275.408325f},{452.269989f,260.060364f},
    {452.345551f,275.841675f},{471.498199f,306.69986f},{452.420715f,291.53717f},
    {480.f,297.894745f},{480.000031f,282.247864f},{480.f,313.45697f}};
  CompleteOutline(jungle,sizeof(jungle)/sizeof(*jungle));
  CompleteOutline(temple,sizeof(temple)/sizeof(*temple));
  CompleteOutline(snow,sizeof(snow)/sizeof(*snow));
  CompleteOutline(ice,sizeof(ice)/sizeof(*ice));
}

int main(void) {
  SegmentedOutlineCorners();
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
