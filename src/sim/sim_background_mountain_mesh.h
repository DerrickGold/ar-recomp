#ifndef SIM_BACKGROUND_MOUNTAIN_MESH_H
#define SIM_BACKGROUND_MOUNTAIN_MESH_H

#include "sim_background_mountain_relief.h"
#include "sim_background_mountains.h"

typedef struct SimBackgroundMountainMeshUV { float x, y; } SimBackgroundMountainMeshUV;
typedef struct SimBackgroundMountainMeshDirection {
  float x, y;
} SimBackgroundMountainMeshDirection;

typedef void (*SimBackgroundMountainMeshEmit)(
    void *user, const float x[4], const float y[4], const float z[4],
    const SimBackgroundMountainMeshUV uv[4],
    const uint8_t brightness[4], const uint8_t alpha[4]);

/* Camera-independent local construction, in native town pixels. Consumers
 * choose stack direction and atlas registration, then project the same
 * inclined faces and fitted silhouette walls into a town or onto a globe.
 * No active-town state, WRAM, render device or GPU storage is accessed. */
typedef struct SimBackgroundMountainMeshContext {
  const SimBackgroundMountainRelief *relief;
  SimBackgroundMountainMeshDirection stack_direction;
  float height_scale;
  float atlas_pixels;
  SimBackgroundMountainMeshEmit emit;
  void *user;
} SimBackgroundMountainMeshContext;

void SimBackgroundMountainMesh_PlanePoint(
    float source_x, float source_y, float baseline,
    const SimBackgroundMountainRelief *relief, float offset_x, float offset_y,
    float *local_x, float *local_y, float *local_z);
void SimBackgroundMountainMesh_StackTile(
    const SimBackgroundMountainMeshContext *context,
    float baseline_left, float baseline_right,
    float maximum_rise_left, float maximum_rise_right,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t flags);
void SimBackgroundMountainMesh_SkirtTile(
    const SimBackgroundMountainMeshContext *context,
    float baseline, float maximum_rise,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t source_tile, bool right_edge);

#endif  /* SIM_BACKGROUND_MOUNTAIN_MESH_H */
