/* Shared SIM 3D present geometry, and the dependency the stage units are
 * built on.
 *
 * Same rule as present_sim3d_internal.h: nothing here may carry live game
 * state. Every present-time decision arrives via the `const FrameSlot *`. */
#ifndef AR_PRESENT_SIM3D_PROJECT_H
#define AR_PRESENT_SIM3D_PROJECT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "present.h"
#include "render/render_types.h"
#include "scene3d_math.h"
#include "sim/sim_background_voxel_renderer.h"
#include "sim/sim_render_metadata.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

enum {
  /* Mesh density for every ground-covering trapezoid: the extension, and the
   * cloud shroud that covers the same one. Dense enough to resolve the cull
   * boundary rather than merely the perspective -- the fade is sampled at
   * these vertices, so the mesh has to be finer than the smallest feature the
   * fade is meant to show. Shared so the shroud cannot be coarser than the
   * ground it sits over and interpolate the rounded window back into a box. */
  kSimUnderlayColumns = 64,
  kSimUnderlayRows = 48,
  /* How far past the visible window the extension reaches, in authentic town
   * pixels. One town is 512, so this is exactly enough to show a neighbouring
   * region and no more; the horizon guard clips it sooner in practice. The
   * shroud covers the same reach. */
  kSimUnderlayMarginPixels = 512,
};

/* Nothing may be drawn closer to the camera plane than this: past it the
 * perspective divide inverts and the mesh folds back over the scene. */
static const float kSimUnderlayMinClipDepth = 0.35f;

/* D5a cull fade. One boundary drives two independent ground treatments:
 * `fade` hands town ground over to the underlay, while `dim` multiplies the
 * surviving colour toward black. Every ground draw that can cover the town
 * canvas must use this same description, or a later opaque draw will restore
 * the square captured-texture boundary over the rounded fade. */
typedef struct SimCullFade {
  int lead;
  int corner;
  int lift_inset;
  float fade;
  float dim;
  /* The full-resolution town map is finite even when the projected ground is
   * not. Feather its own extent into the half-resolution world underlay so
   * the last canvas texel cannot form a hard vertical/horizontal seam. */
  float extent_x0, extent_y0, extent_x1, extent_y1;
  float extent_feather;
  int margin_left, margin_right, margin_top, margin_bottom;
  int screen_x0;
} SimCullFade;

float SimCullProximityAt(
    const SimCullFade *fade, float texture_x, float texture_y, SDL_Rect source);
float SimGroundExtentAlphaAt(
    const SimCullFade *fade, float texture_x, float texture_y);
void DrawSimGroundPlane(
    ArRenderTexture texture, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16], const SimCullFade *fade);

/* Height conversions. A virtual height is authored in source pixels; world
 * units are what the projection consumes. */
float SimHeightWorldUnits(
    SDL_Rect source, int virtual_height, unsigned height_scale_x100);
float SimTerrainGroundHeightWorld(
    const FrameSlot *slot, SDL_Rect source, float map_x, float map_y);
float SimTerrainMaximumHeightWorld(
    const FrameSlot *slot, SDL_Rect source);
/* The altitude an object's own height is measured from: local terrain for a
 * grounded object, the stable town maximum for a flyer. */
float SimObjectAltitudeBaseWorld(
    const FrameSlot *slot, const SimRenderObject *object, SDL_Rect source,
    float map_x, float map_y);

SimBackgroundVoxelRenderParams SimVoxelRenderParams(
    const FrameSlot *slot, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16]);

bool ProjectSimTexturePoint(
    const float matrix[16], SDL_Rect source, SDL_Rect viewport,
    float texture_x, float texture_y, float height_world,
    Scene3DPoint *out_point);
bool ProjectSimAnchorAndScale(
    const float matrix[16], SDL_Rect source, SDL_Rect viewport,
    float texture_x, float texture_y, float height_world,
    float reference_depth, Scene3DPoint *anchor, float *scale_x,
    float *scale_y);
/* The world origin is a parameter rather than a field read, so a caller
 * walking an effect's retained path can project each earlier position
 * without copying the whole instance to move two numbers. */
bool ProjectSimEffectPointAt(
    const FrameSlot *slot, const SimEffectInstance *effect,
    uint16_t world_x, uint16_t world_y,
    const SimEffectLocalPoint *local, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16],
    Scene3DPoint *point, float *scale_x, float *scale_y);

bool ProjectSimEffectPoint(
    const FrameSlot *slot, const SimEffectInstance *effect,
    const SimEffectLocalPoint *local, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16], Scene3DPoint *point,
    float *scale_x, float *scale_y);

#endif /* AR_PRESENT_SIM3D_PROJECT_H */
