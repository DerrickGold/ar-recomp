#ifndef SIM_BACKGROUND_VOXEL_RENDERER_H
#define SIM_BACKGROUND_VOXEL_RENDERER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct SimBackgroundVoxelRenderParams {
  uint32_t serial;
  uint8_t detail;
  uint16_t camera_x, camera_y;
  uint16_t town_screen_x0;
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg;
  SDL_Rect source;
  SDL_Rect viewport;
  const float *matrix;
} SimBackgroundVoxelRenderParams;

/* Render-thread half of sim_background_voxels. Texture ownership and geometry
 * live here so the general SIM compositor only chooses a painter-order seam. */
void SimBackgroundVoxelRenderer_Upload(SDL_Renderer *renderer);
bool SimBackgroundVoxelRenderer_Ready(uint32_t serial);
SDL_Texture *SimBackgroundVoxelRenderer_GroundTexture(uint32_t serial);
void SimBackgroundVoxelRenderer_Draw(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params);
/* Adds solid-model contact/directional silhouettes to the caller's active
 * SIM shadow-mask target. The caller owns mask allocation, blur and opacity. */
void SimBackgroundVoxelRenderer_DrawShadowMask(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    float light_x, float light_y);
void SimBackgroundVoxelRenderer_Reset(void);

#endif  /* SIM_BACKGROUND_VOXEL_RENDERER_H */
