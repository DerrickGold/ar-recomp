#ifndef SIM_BACKGROUND_VOXEL_LIGHTING_H
#define SIM_BACKGROUND_VOXEL_LIGHTING_H

#include <stdint.h>

#include "sim_background_voxel_models.h"

typedef struct SimBackgroundVoxelLightDirection {
  float x, y, z;
} SimBackgroundVoxelLightDirection;

/* SDL-free lighting policy for authored SIM voxel models. Keeping this outside
 * the renderer makes material response and AO independently testable and keeps
 * the render path focused on projection and batching. */
uint8_t SimBackgroundVoxelLighting_FaceBrightness(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelShading shading,
    uint16_t light_azimuth_deg,
    uint8_t light_elevation_deg);
void SimBackgroundVoxelLighting_ResolveDirection(
    uint16_t light_azimuth_deg, uint8_t light_elevation_deg,
    SimBackgroundVoxelLightDirection *out);
uint8_t SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
    const SimBackgroundVoxelModelFace *face,
    SimBackgroundVoxelShading shading,
    const SimBackgroundVoxelLightDirection *light);

uint8_t SimBackgroundVoxelLighting_VertexBrightness(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelModel *model,
    int point,
    uint8_t directional_brightness,
    SimBackgroundVoxelShading shading);
void SimBackgroundVoxelLighting_VertexBrightnesses(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelModel *model,
    uint8_t directional_brightness,
    SimBackgroundVoxelShading shading,
    uint8_t out[4]);

#endif  /* SIM_BACKGROUND_VOXEL_LIGHTING_H */
