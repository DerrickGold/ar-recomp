#include "present_sim_globe_mapping.h"
#include "sim/sim_world_map.h"
#include <math.h>
#include <string.h>

bool SimGlobeMapping_Build(uint8_t town, float ox, float oy, float radius,
    float reference, float landscape, SimGlobeMapping *out) {
  if (!out || town < 1 || town > kSimTownCount || !isfinite(ox) || !isfinite(oy) ||
      !isfinite(radius) || radius < 48 || !isfinite(reference) ||
      !isfinite(landscape) || landscape < 0 || landscape > 1.5f) return false;
  SimGlobeMapping map;
  memset(&map, 0, sizeof(map));
  map.town = town; map.origin_x = ox; map.origin_y = oy;
  map.chart_radius = radius; map.reference_height = reference;
  map.landscape = map.town_landscape = landscape;
  float normal[3];
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(radius, ox+16, oy+16, 0, &map.frame) ||
      !SimWorldNavigationGlobe_SampleAtRadius(radius, ox+16, oy+16, normal, &map.metric))
    return false;
  map.radius = radius / map.metric;
  *out = map;
  return true;
}

bool SimGlobeMapping_Point(const SimGlobeMapping *map, float x, float y,
    float ground, float extra, float out[3]) {
  if (!map || !map->town || !out || !isfinite(x) || !isfinite(y) ||
      !isfinite(ground) || !isfinite(extra)) return false;
  float normal[3];
  if (!SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius, x, y, normal, NULL)) return false;
  SimWorldNavigationGlobe_TransformNormal(&map->frame, normal, normal);
  const float radius = map->radius + ground*map->landscape/map->metric + extra;
  const float sphere[3] = {normal[0]*radius, normal[1]*radius,
      normal[2]*radius - map->radius - map->reference_height*map->landscape/map->metric};
  memcpy(out, sphere, sizeof(sphere));
  return true;
}

bool SimGlobeMapping_Source(const SimGlobeMapping *map, const float point[3],
    float *x, float *y, float *radial_height) {
  if (!map || !map->town || !point ||
      !x || !y || !radial_height || !(map->metric>0)) return false;
  float normal[3]={point[0],point[1],point[2]+map->radius+
      map->reference_height*map->landscape/map->metric};
  const float length=hypotf(hypotf(normal[0],normal[1]),normal[2]);
  if (!(length>0) || !isfinite(length)) return false;
  for (int i=0;i<3;++i) normal[i]/=length;
  float chart[3];
  for (int i=0;i<3;++i) chart[i]=map->frame.right[i]*normal[0]+
      map->frame.up[i]*normal[1]+map->frame.outward[i]*normal[2];
  float sx,sy;
  if (!SimWorldNavigationGlobe_SourceAtRadius(map->chart_radius,chart,&sx,&sy)) return false;
  const float height=length-map->radius;
  if (!isfinite(height)) return false;
  *x=sx; *y=sy; *radial_height=height;
  return true;
}

bool SimGlobeMapping_Encode(const SimGlobeMapping *map, float x, float y,
    float ground, float extra, float normal[3], float elevation[2]) {
  float point[3];
  if (!normal || !elevation || !SimGlobeMapping_Point(map,x,y,ground,extra,point)) return false;
  point[2] += map->radius;
  const float length = hypotf(hypotf(point[0],point[1]),point[2]);
  if (!(length > 0) || !isfinite(length)) return false;
  for (int i = 0; i < 3; ++i) normal[i] = point[i]/length;
  elevation[0] = length-map->radius; elevation[1] = 0;
  return true;
}
