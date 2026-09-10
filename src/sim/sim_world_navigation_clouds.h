#ifndef SIM_WORLD_NAVIGATION_CLOUDS_H
#define SIM_WORLD_NAVIGATION_CLOUDS_H

#include <stdbool.h>
#include <stdint.h>

enum {
  kSimWorldNavigationCloudWidth = 512,
  kSimWorldNavigationCloudHeight = 256,
};

/* A spherical noise field, not a wrapped rectangle. Longitude endpoints
 * and all samples at either pole agree exactly, including under filtering.
 * Uses bounded row-local working storage, with no heap or retained cache. */
bool SimWorldNavigationClouds_Bake(
    uint32_t *out, int pitch, int width, int height, float scale);

typedef struct SimWorldNavigationCloudRotation {
  float cos_u, sin_u, cos_v, sin_v;
} SimWorldNavigationCloudRotation;

SimWorldNavigationCloudRotation SimWorldNavigationClouds_Rotation(float u, float v);
/* Rigid rotation advects a cloud field without opening a seam at the poles.
 * Input is a fixed world unit normal, independent of the current camera. */
void SimWorldNavigationClouds_UV(
    const float normal[3], const SimWorldNavigationCloudRotation *rotation,
    float *u, float *v);
/* Retain the rotated direction for chart clipping. A moving pole can lie
 * inside a face: unwrapping its four corner longitudes alone is ambiguous. */
typedef struct SimWorldNavigationCloudCoordinate {
  float x, y, z;
  float u, v;
} SimWorldNavigationCloudCoordinate;
SimWorldNavigationCloudCoordinate SimWorldNavigationClouds_Coordinate(
    const float normal[3], const SimWorldNavigationCloudRotation *rotation);
bool SimWorldNavigationClouds_NeedsSplit(const SimWorldNavigationCloudCoordinate quad[4]);

enum { kSimWorldNavigationCloudMaxPatches = 8 };
typedef struct SimWorldNavigationCloudPatch {
  /* Barycentric weights preserve the original screen/depth triangle exactly.
   * Double intermediates avoid raster cracks when adjacent faces traverse an
   * edge in opposite orders. Four corners encode a quad or a triangle with
   * its last corner repeated. */
  double weight[4][3];
  float u[4], v[4];
} SimWorldNavigationCloudPatch;
/* Clip one triangle into bounded longitude quadrants. A pole is duplicated
 * per chart, never connected across a discontinuous longitude interval. */
int SimWorldNavigationClouds_SplitTriangle(
    const SimWorldNavigationCloudCoordinate triangle[3],
    SimWorldNavigationCloudPatch out[kSimWorldNavigationCloudMaxPatches]);
/* Unwrap one small quad across longitude zero before texture interpolation. */
void SimWorldNavigationClouds_Unwrap(float u[4]);

#endif  /* SIM_WORLD_NAVIGATION_CLOUDS_H */
