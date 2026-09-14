/* Presentation-only, camera-independent town/world placement.
 * Units are SIM cells, not backend pixels. No native state or GPU handles. */
#ifndef PRESENT_SIM_GLOBE_MAPPING_H
#define PRESENT_SIM_GLOBE_MAPPING_H
#include "sim/sim_world_navigation_globe.h"
#include <stdint.h>
typedef struct SimGlobeMapping {
  SimWorldNavigationGlobeFrame frame;
  float origin_x, origin_y, chart_radius, radius, metric, reference_height;
  float landscape, town_landscape;
  uint8_t town;
} SimGlobeMapping;
/* One continuous sphere: no flat footprint, transition collar or alternate
 * height transform at the active town boundary. */
bool SimGlobeMapping_Build(uint8_t town, float origin_x, float origin_y,
    float chart_radius, float reference_height, float landscape,
    SimGlobeMapping *out);
/* Extra rise is already in local SIM cells. Ground is in world terrain units.
 * For continuous terrain, interpolate motion in chart coordinates FIRST,
 * then map its position/ground/altitude. Interpolating mapped endpoints in
 * Cartesian space instead makes a chord that can pass below the surface. */
bool SimGlobeMapping_Point(const SimGlobeMapping *map, float x, float y,
    float ground, float extra, float out[3]);
/* Continuous inverse: owned world point -> chart XY and total radial height
 * in local cells (ground contribution plus extra rise). Consumers with a
 * grounded actor contract subtract their registered support once. */
bool SimGlobeMapping_Source(const SimGlobeMapping *map, const float point[3],
    float *x, float *y, float *radial_height);
/* Encode an arbitrary fixed-town position into the existing radial GPU
 * contract. Draw with identity basis, radius=map.radius, height_scale=1,
 * reference_height=0. This is source publication, never per-frame projection. */
bool SimGlobeMapping_Encode(const SimGlobeMapping *map, float x, float y,
    float ground, float extra, float normal[3], float elevation[2]);
#endif
