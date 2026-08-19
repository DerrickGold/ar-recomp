#ifndef SIM_BACKGROUND_VOXEL_RENDERER_H
#define SIM_BACKGROUND_VOXEL_RENDERER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct SimBackgroundVoxelRenderParams {
  uint32_t serial;
  uint8_t detail;
  uint8_t lod;
  uint8_t shading;
  uint8_t style;
  uint8_t facing;
  uint8_t render_scale;
  uint16_t game_frame;
  uint16_t camera_x, camera_y;
  uint16_t town_screen_x0;
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg;
  SDL_Rect source;
  SDL_Rect viewport;
  const float *matrix;
} SimBackgroundVoxelRenderParams;

/* Which actors the caller should draw at this point in the composite.
 *
 * Ground and Mountain are a TERRAIN split, not a depth split: in the authentic
 * 2D town an actor either stands on a mountain metatile or it does not, and
 * that is exactly the question of whether the terrain in front of it should
 * hide it. Ground actors are drawn BEFORE the composite, so the town's own
 * geometry covers them; mountain actors are drawn after it and lifted onto the
 * surface, so a villager scripted to climb a peak stays visible on the slope. */
typedef enum SimBackgroundVoxelActorBand {
  kSimBackgroundVoxelActorBand_Ground,
  kSimBackgroundVoxelActorBand_Mountain,
  kSimBackgroundVoxelActorBand_Overhead,
} SimBackgroundVoxelActorBand;

typedef void (*SimBackgroundVoxelDepthLayerCallback)(
    void *userdata,
    const SimBackgroundVoxelRenderParams *params,
    float minimum_depth, float maximum_depth,
    SimBackgroundVoxelActorBand band);

/* Render-thread half of sim_background_voxels. Texture ownership, projected
 * geometry and D32 visibility live here; the general SIM compositor owns only
 * the ground base and intentionally promoted actor/selection overlays. */
void SimBackgroundVoxelRenderer_Upload(SDL_Renderer *renderer);
bool SimBackgroundVoxelRenderer_Ready(uint32_t serial);
SDL_Texture *SimBackgroundVoxelRenderer_GroundTexture(uint32_t serial);
void SimBackgroundVoxelRenderer_Draw(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params);
/* Invokes the caller for ground actors, draws the depth-tested background
 * composite over them, then invokes it again for mountain-standing and
 * overhead actors. */
void SimBackgroundVoxelRenderer_DrawInterleaved(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelDepthLayerCallback callback, void *userdata);
/* Adds solid-model contact/directional silhouettes to the caller's active
 * SIM shadow-mask target. The caller owns mask allocation, blur and opacity. */
void SimBackgroundVoxelRenderer_DrawShadowMask(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    float light_x, float light_y);
/* Where the volcano's crater mouth was drawn on the frame just rendered, in
 * the mountain models' own local space: the exact point the crater glow ring
 * and the smoke plume are placed on, already leaned by the camera-facing
 * transform the models use.
 *
 * Published so a presentation stage that wants to emit from the crater --
 * the volcanic eruption's fireball arcs -- can ask the model where its mouth
 * is instead of re-deriving it from the stamp's authored geometry and then
 * drifting away from it whenever the relief is retuned. Local x maps to a
 * town map column by adding `town_screen_x0` and subtracting the widescreen
 * margin; local y IS the map row, because the models' y origin is
 * `-camera_y`. `false` when no volcano was drawn. */
typedef struct SimBackgroundCraterAnchor {
  bool valid;
  float local_x, local_y;
  float height_pixels;
} SimBackgroundCraterAnchor;

bool SimBackgroundVoxelRenderer_CraterAnchor(SimBackgroundCraterAnchor *out);

void SimBackgroundVoxelRenderer_Reset(void);

#endif  /* SIM_BACKGROUND_VOXEL_RENDERER_H */
