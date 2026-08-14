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
} SimBackgroundVoxelModelFace;

typedef struct SimBackgroundVoxelModel {
  uint16_t face_count;
  uint16_t face_budget;
  bool overflow;
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
  SimBackgroundVoxelModelFace faces[kSimBackgroundVoxelModelMaxFaces];
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

#endif  /* SIM_BACKGROUND_VOXEL_MODELS_H */
