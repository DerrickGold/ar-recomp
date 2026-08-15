#ifndef SIM_BACKGROUND_VOXEL_MODELS_H
#define SIM_BACKGROUND_VOXEL_MODELS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_voxel_quality.h"
#include "sim_background_voxels.h"

enum {
  /* Ultra's absolute ceiling. Each lower quality level has a smaller enforced
   * budget returned by SimBackgroundVoxelModel_FaceBudget. */
  kSimBackgroundVoxelModelMaxFaces = 384,
  /* Ultra trees use a 9x9x9 occupancy volume. Most cells are empty, but the
   * compiler keeps enough solid records to make the limit an invariant rather
   * than a shape-dependent failure. Boxes are build metadata, not GPU faces. */
  kSimBackgroundVoxelModelMaxBoxes = 320,
};

typedef enum SimBackgroundVoxelMaterial {
  kSimVoxelMaterial_Wall,
  kSimVoxelMaterial_WallLight,
  kSimVoxelMaterial_Roof,
  kSimVoxelMaterial_RoofLight,
  kSimVoxelMaterial_Trim,
  kSimVoxelMaterial_Dark,
  kSimVoxelMaterial_Wood,
  kSimVoxelMaterial_Metal,
  kSimVoxelMaterial_Blade,
  kSimVoxelMaterial_Trunk,
  kSimVoxelMaterial_Leaves,
  kSimVoxelMaterial_LeavesLight,
  kSimVoxelMaterial_LeavesDark,
  kSimVoxelMaterial_Paving,
  kSimVoxelMaterial_Gold,
  kSimVoxelMaterial_Glass,
  kSimVoxelMaterial_Snow,
  kSimVoxelMaterial_Contact,
  kSimVoxelMaterial_Count,
} SimBackgroundVoxelMaterial;

typedef struct SimBackgroundVoxelModelPoint {
  float x, y, z;
} SimBackgroundVoxelModelPoint;

typedef struct SimBackgroundVoxelModelFace {
  SimBackgroundVoxelModelPoint points[4];
  uint8_t material;
  /* 255 is the material colour unchanged. Face direction and deliberate
   * stepped-model variation are expressed without allocating more materials. */
  uint8_t brightness;
  /* Geometry-derived concave-corner visibility. 255 means fully exposed;
   * lighting may combine this with its inexpensive height/contact gradient. */
  uint8_t occlusion[4];
} SimBackgroundVoxelModelFace;

typedef struct SimBackgroundVoxelModelBox {
  float x0, y0, z0;
  float x1, y1, z1;
} SimBackgroundVoxelModelBox;

typedef struct SimBackgroundVoxelModel {
  uint16_t face_count;
  uint16_t authored_face_count;
  uint16_t face_budget;
  uint16_t box_count;
  bool overflow;
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
  SimBackgroundVoxelModelFace faces[kSimBackgroundVoxelModelMaxFaces];
  /* Retained for cacheable surface compilation and corner AO. The renderer
   * never submits these records directly. */
  SimBackgroundVoxelModelBox boxes[kSimBackgroundVoxelModelMaxBoxes];
} SimBackgroundVoxelModel;

uint16_t SimBackgroundVoxelModel_FaceBudget(
    SimBackgroundVoxelDetail detail);

/* Builds an object-local model in authentic town pixels. X and Y cover the
 * ground footprint; Z is height. This module deliberately knows nothing about
 * SDL, the camera, or GPU resources. */
void SimBackgroundVoxelModel_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *out);

/* Styling is independent from density: detail owns face budgets and stepped
 * resolution, while style selects optional authored architecture. */
void SimBackgroundVoxelModel_BuildStyled(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModel *out);

#endif  /* SIM_BACKGROUND_VOXEL_MODELS_H */
