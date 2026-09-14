#include "present_sim_globe_project.h"
#include "sim/sim_world_map.h"
#include <math.h>
#include <string.h>

bool PresentSimGlobeProject_Build(const SimGlobeMapping *map,
    const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
    float reference_depth, PresentSimGlobeProjection *out) {
  if (!out || !map || !matrix || !map->town ||
      map->town < 1 || map->town > kSimTownCount ||
      !isfinite(map->chart_radius) || map->chart_radius < 48 ||
      !isfinite(map->radius) || map->radius <= 0 ||
      !isfinite(map->metric) || map->metric <= 0 ||
      !isfinite(map->reference_height) || !isfinite(map->landscape) ||
      map->landscape < 0 || source.w <= 0 || source.h <= 0 ||
      viewport.w <= 0 || viewport.h <= 0 ||
      !isfinite(reference_depth) || reference_depth <= 0) return false;
  for (int i = 0; i < 16; ++i) if (!isfinite(matrix[i])) return false;
  float center[3];
  if (!SimGlobeMapping_Point(map,map->origin_x+16,map->origin_y+16,0,0,center))
    return false;
  for (int i = 0; i < 3; ++i) if (!isfinite(center[i])) return false;
  PresentSimGlobeProjection projection = {.map = *map,.viewport = viewport,
    .pixel_scale = {(float)viewport.w/source.w,(float)viewport.h/source.h},
    .reference_depth = reference_depth,.ready = true};
  memcpy(projection.matrix,matrix,sizeof(projection.matrix));
  *out = projection;
  return true;
}

bool PresentSimGlobeProject_Point(const PresentSimGlobeProjection *projection,
    float native_x, float native_y, float support, float altitude,
    PresentSimGlobeProjectedPoint *out) {
  if (!projection || !projection->ready || !out) return false;
  PresentSimGlobeProjectedPoint point = {0};
  const SimGlobeMapping *map = &projection->map;
  if (!SimGlobeMapping_Point(map,map->origin_x+native_x/16,
          map->origin_y+native_y/16,support,altitude/16,point.world) ||
      !Scene3D_TransformToClip(projection->matrix,point.world[0],point.world[1],
          point.world[2],&point.clip) ||
      point.clip.w <= kScene3DMinimumProjectionDepth) return false;
  const float inverse = 1/point.clip.w;
  point.screen = (Scene3DPoint){
    projection->viewport.x + (point.clip.x*inverse*.5f+.5f)*projection->viewport.w,
    projection->viewport.y + (.5f-point.clip.y*inverse*.5f)*projection->viewport.h};
  point.depth = point.clip.z*inverse*.5f+.5f;
  for (int i = 0; i < 2; ++i)
    point.pixel_scale[i] = projection->pixel_scale[i]*projection->reference_depth*inverse;
  if (!isfinite(point.screen.x) || !isfinite(point.screen.y) || !isfinite(point.depth) ||
      !isfinite(point.pixel_scale[0]) || !isfinite(point.pixel_scale[1])) return false;
  *out = point;
  return true;
}

bool PresentSimGlobeProject_GroundBillboardAxes(
    const PresentSimGlobeProjection *projection,
    float native_x, float native_y, const PresentSimGlobeProjectedPoint *anchor,
    PresentSimGlobeBillboardAxes *out) {
  if (!projection || !projection->ready || !anchor || !out ||
      !isfinite(anchor->clip.w) || anchor->clip.w <= kScene3DMinimumProjectionDepth ||
      !isfinite(anchor->clip.x) || !isfinite(anchor->clip.y)) return false;
  const SimGlobeMapping *map=&projection->map;
  SimWorldNavigationGlobeFrame local;
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(map->chart_radius,
          map->origin_x+native_x/16,map->origin_y+native_y/16,0,&local)) return false;
  float east[3], north[3], radial[3];
  SimWorldNavigationGlobe_TransformNormal(&map->frame,local.right,east);
  SimWorldNavigationGlobe_TransformNormal(&map->frame,local.up,north);
  SimWorldNavigationGlobe_TransformNormal(&map->frame,local.outward,radial);
  const float *m=projection->matrix;
  float north_depth=0, radial_depth=0;
  for (int i=0;i<3;++i) {
    north_depth+=m[i*4+3]*north[i];
    radial_depth+=m[i*4+3]*radial[i];
  }
  /* Rotate only within the north/radial plane, about east. Cancelling its
   * clip-depth gradient gives the pitch-facing up vector while east retains
   * its terrain orientation. Do not replace east with the camera's right. */
  const float length=hypotf(north_depth,radial_depth);
  if (!(length>0.000001f) || !isfinite(length)) return false;
  float up[3];
  for (int i=0;i<3;++i)
    up[i]=(-radial_depth*north[i]+north_depth*radial[i])/length;
  PresentSimGlobeBillboardAxes axes;
  Scene3DPoint *projected[2]={&axes.right,&axes.down};
  const float *directions[2]={east,up};
  for (unsigned axis=0;axis<2;++axis) {
    float dx=0,dy=0,dw=0;
    for (int i=0;i<3;++i) {
      dx+=m[i*4]*directions[axis][i];
      dy+=m[i*4+1]*directions[axis][i];
      dw+=m[i*4+3]*directions[axis][i];
    }
    const float inverse=1/anchor->clip.w;
    const float pixels=(axis ? -1.0f : 1.0f)/16;
    *projected[axis]=(Scene3DPoint){
      (dx-anchor->clip.x*inverse*dw)*inverse*.5f*projection->viewport.w*pixels,
      -(dy-anchor->clip.y*inverse*dw)*inverse*.5f*projection->viewport.h*pixels};
    if (!isfinite(projected[axis]->x) || !isfinite(projected[axis]->y)) return false;
  }
  const float determinant=axes.right.x*axes.down.y-axes.right.y*axes.down.x;
  if (!isfinite(determinant) || determinant <= 0.000001f) return false;
  *out=axes;
  return true;
}
