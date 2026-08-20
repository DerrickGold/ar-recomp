#include "sim_background_voxel_biome.h"

#include <math.h>

SimBackgroundVoxelBiome SimBackgroundVoxelBiome_ForTown(uint8_t town) {
  switch (town) {
    case 1: return kSimBackgroundVoxelBiome_Temperate;
    case 2: return kSimBackgroundVoxelBiome_Wetland;
    case 3: return kSimBackgroundVoxelBiome_Desert;
    case 4: return kSimBackgroundVoxelBiome_Volcanic;
    case 5: return kSimBackgroundVoxelBiome_Tropical;
    case 6: return kSimBackgroundVoxelBiome_Snow;
  }
  return kSimBackgroundVoxelBiome_Temperate;
}

static bool MaterialCollectsSnow(SimBackgroundVoxelMaterial material) {
  switch (material) {
    case kSimVoxelMaterial_Roof:
    case kSimVoxelMaterial_RoofLight:
    case kSimVoxelMaterial_Leaves:
    case kSimVoxelMaterial_LeavesLight:
    case kSimVoxelMaterial_LeavesDark:
      return true;
    case kSimVoxelMaterial_Wall:
    case kSimVoxelMaterial_WallLight:
    case kSimVoxelMaterial_Trim:
    case kSimVoxelMaterial_Dark:
    case kSimVoxelMaterial_Wood:
    case kSimVoxelMaterial_Metal:
    case kSimVoxelMaterial_Blade:
    case kSimVoxelMaterial_Trunk:
    case kSimVoxelMaterial_Paving:
    case kSimVoxelMaterial_Foundation:
    case kSimVoxelMaterial_Gold:
    case kSimVoxelMaterial_Glass:
    case kSimVoxelMaterial_Snow:
    case kSimVoxelMaterial_Contact:
    case kSimVoxelMaterial_Count:
      return false;
  }
  return false;
}

SimBackgroundVoxelMaterial SimBackgroundVoxelBiome_SurfaceMaterial(
    SimBackgroundVoxelBiome biome,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelMaterial material,
    const SimBackgroundVoxelModelFace *face) {
  if (biome != kSimBackgroundVoxelBiome_Snow ||
      detail < kSimBackgroundVoxelDetail_High || !face ||
      !MaterialCollectsSnow(material))
    return material;
  const SimBackgroundVoxelModelPoint *a = &face->points[0];
  const SimBackgroundVoxelModelPoint *b = &face->points[1];
  const SimBackgroundVoxelModelPoint *d = &face->points[3];
  float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
  float vx = d->x - a->x, vy = d->y - a->y, vz = d->z - a->z;
  float nx = uy * vz - uz * vy;
  float ny = uz * vx - ux * vz;
  float nz = ux * vy - uy * vx;
  float length = sqrtf(nx * nx + ny * ny + nz * nz);
  if (length < 0.0001f || fabsf(nz) / length < 0.52f) return material;
  return kSimVoxelMaterial_Snow;
}
