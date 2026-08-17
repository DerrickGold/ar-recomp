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

static const SimBackgroundVoxelProportions kPalm = {
  .footprint_scale = 0.92f,
  .height_scale = 0.92f,
};

static const SimBackgroundVoxelProportions kBroadTree = {
  .footprint_scale = 0.92f,
  .height_scale = 0.88f,
};

static const SimBackgroundVoxelProportions kShrub = {
  .footprint_scale = 0.88f,
  .height_scale = 0.90f,
};

static const SimBackgroundVoxelProportions kStoryTree = {
  .footprint_scale = 0.94f,
  .height_scale = 0.90f,
};

static const SimBackgroundVoxelProportions kBloodpoolCastle = {
  .footprint_scale = 0.92f,
  .height_scale = 0.86f,
};

static const SimBackgroundVoxelProportions kMarahnaTemple = {
  .footprint_scale = 0.92f,
  .height_scale = 0.88f,
};

static const SimBackgroundVoxelProportions kPyramid = {
  /* A pyramid reads as a pyramid only if its base stays broad, so this is the
   * one landmark that keeps almost its whole plot. */
  .footprint_scale = 0.96f,
  .height_scale = 0.84f,
};

const SimBackgroundVoxelProportions *SimBackgroundVoxelProportions_Get(
    SimBackgroundVoxelKind kind) {
  switch (kind) {
    case kSimBackgroundVoxel_House: return &kHouse;
    case kSimBackgroundVoxel_Cathedral: return &kCathedral;
    case kSimBackgroundVoxel_Windmill: return &kWindmill;
    case kSimBackgroundVoxel_Factory: return &kFactory;
    case kSimBackgroundVoxel_Tree: return &kTree;
    case kSimBackgroundVoxel_BroadTree: return &kBroadTree;
    case kSimBackgroundVoxel_Palm: return &kPalm;
    case kSimBackgroundVoxel_Shrub: return &kShrub;
    case kSimBackgroundVoxel_StoryTree: return &kStoryTree;
    case kSimBackgroundVoxel_BloodpoolCastle: return &kBloodpoolCastle;
    case kSimBackgroundVoxel_MarahnaTemple: return &kMarahnaTemple;
    case kSimBackgroundVoxel_Pyramid: return &kPyramid;
  }
  return &kHouse;
}
