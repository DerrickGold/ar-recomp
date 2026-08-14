#include "sim_background_voxel_proportions.h"

static const SimBackgroundVoxelProportions kHouse = {
  .footprint_scale = 0.90f,
  .height_scale = 0.68f,
};

static const SimBackgroundVoxelProportions kCathedral = {
  .footprint_scale = 0.90f,
  .height_scale = 0.82f,
};

static const SimBackgroundVoxelProportions kWindmill = {
  .footprint_scale = 0.90f,
  .height_scale = 0.84f,
};

static const SimBackgroundVoxelProportions kFactory = {
  .footprint_scale = 0.90f,
  .height_scale = 0.82f,
};

static const SimBackgroundVoxelProportions kTree = {
  .footprint_scale = 0.90f,
  .height_scale = 0.88f,
};

const SimBackgroundVoxelProportions *SimBackgroundVoxelProportions_Get(
    SimBackgroundVoxelKind kind) {
  switch (kind) {
    case kSimBackgroundVoxel_House: return &kHouse;
    case kSimBackgroundVoxel_Cathedral: return &kCathedral;
    case kSimBackgroundVoxel_Windmill: return &kWindmill;
    case kSimBackgroundVoxel_Factory: return &kFactory;
    case kSimBackgroundVoxel_Tree: return &kTree;
  }
  return &kHouse;
}
