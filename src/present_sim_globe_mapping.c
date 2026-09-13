#include "present_sim_globe_mapping.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim_town_layout.h"
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
  map.transition_cells = 8;
  float normal[3];
  if (!SimWorldNavigationGlobe_BuildFrameAtRadius(radius, ox+16, oy+16, 0, &map.frame) ||
      !SimWorldNavigationGlobe_SampleAtRadius(radius, ox+16, oy+16, normal, &map.metric))
    return false;
  map.radius = radius / map.metric;
  *out = map;
  return true;
}

float SimGlobeMapping_Weight(const SimGlobeMapping *map, float x, float y) {
  const float dx = fmaxf(0, fmaxf(map->origin_x-x, x-map->origin_x-32));
  const float dy = fmaxf(0, fmaxf(map->origin_y-y, y-map->origin_y-32));
  const float t = fminf(1, hypotf(dx, dy) / map->transition_cells);
  return t*t*(3-2*t);
}

bool SimGlobeMapping_Point(const SimGlobeMapping *map, float x, float y,
    float ground, float extra, float out[3]) {
  if (!map || !map->town || !out || !isfinite(x) || !isfinite(y) ||
      !isfinite(ground) || !isfinite(extra)) return false;
  const float weight = SimGlobeMapping_Weight(map, x, y);
  float normal[3];
  if (!SimWorldNavigationGlobe_SampleAtRadius(map->chart_radius, x, y, normal, NULL)) return false;
  SimWorldNavigationGlobe_TransformNormal(&map->frame, normal, normal);
  const float radius = map->radius + ground*map->landscape/map->metric + extra;
  const float sphere[3] = {normal[0]*radius, normal[1]*radius,
      normal[2]*radius - map->radius - map->reference_height*map->landscape/map->metric};
  /* Sample just inside the outermost owned cell: x/y=512 belongs outside
   * the native 32x32 town. Preserve that cell's actual edge elevation. */
  const float px = fmaxf(0, fminf(511.999f, (x-map->origin_x)*16));
  const float py = fmaxf(0, fminf(511.999f, (y-map->origin_y)*16));
  const float flat[3] = {x-map->origin_x-16, map->origin_y+16-y,
      SimTownTerrain_HeightUnitsAt(map->town, px, py)*map->town_landscape + extra};
  for (int i = 0; i < 3; ++i) out[i] = flat[i] + weight*(sphere[i]-flat[i]);
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
