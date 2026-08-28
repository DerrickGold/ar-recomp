/* Shared SIM 3D present geometry: the D5a cull fade, the projected ground
 * plane, and the height/anchor/scale conversions every stage places its
 * vertices through. Split out of present_sim3d.c; the definitions are
 * unchanged.
 *
 * This is the unit the effect, shadow, cloud and terrain stages are built on.
 * A stage that needs to put something at a town position in world space asks
 * here rather than re-deriving the mapping, which is what keeps an actor's
 * feet, its shadow and the ground under it agreeing. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present_sim3d_internal.h"
#include "present_sim3d_project.h"
#include "present_sim3d_shadows.h"
#include "render/render_device.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim3d.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

extern ArRenderDevice g_render_device;


/* Cull proximity at a captured-texture point, 0..1. The conversion back to
 * the emitter's biased coordinates keeps the visual boundary identical to the
 * cull predicate instead of maintaining a second approximation of it. */
float SimCullProximityAt(const SimCullFade *fade, float texture_x,
                                float texture_y, ArRenderRectI source) {
  if (!fade) return 0.0f;
  int16_t biased_x = (int16_t)(texture_x - (float)fade->screen_x0 + 16.0f);
  int16_t biased_y = (int16_t)(texture_y - (float)source.y + 17.0f);
  return Sim3D_CullProximity(biased_x, biased_y, fade->margin_left,
                             fade->margin_right, fade->margin_top,
                             fade->margin_bottom, fade->lead,
                             fade->corner, fade->lift_inset);
}

/* Opacity of full-resolution town ground at the finite town-map extent.
 * Smoothstep has zero slope at both ends, avoiding a second crease where the
 * feather reaches full opacity. A zero feather disables this independent
 * mask (as it must for the world-map underlay itself). */
float SimGroundExtentAlphaAt(const SimCullFade *fade, float texture_x,
                                    float texture_y) {
  if (!fade || fade->extent_feather <= 0.0f) return 1.0f;
  float edge_x = fminf(texture_x - fade->extent_x0,
                       fade->extent_x1 - texture_x);
  float edge_y = fminf(texture_y - fade->extent_y0,
                       fade->extent_y1 - texture_y);
  float edge = edge_x < edge_y ? edge_x : edge_y;
  float ramp = edge / fade->extent_feather;
  if (ramp <= 0.0f) return 0.0f;
  if (ramp >= 1.0f) return 1.0f;
  return ramp * ramp * (3.0f - 2.0f * ramp);
}

/* Inserts a texture-space split into a sorted mesh axis. Coordinates at or
 * outside the existing extent do not create a new segment. */
int InsertSimGroundCoordinate(float *coordinates, int count,
                                     int capacity, float coordinate) {
  if (count <= 0 || count >= capacity ||
      coordinate <= coordinates[0] ||
      coordinate >= coordinates[count - 1])
    return count;
  int index = 1;
  while (index < count && coordinates[index] < coordinate) index++;
  if (fabsf(coordinates[index] - coordinate) < 0.0001f) return count;
  memmove(&coordinates[index + 1], &coordinates[index],
          (size_t)(count - index) * sizeof(coordinates[0]));
  coordinates[index] = coordinate;
  return count + 1;
}

enum {
  /* The cull tint is sampled at these vertices. The old 8x6 projection mesh
   * was adequate for affine UVs but too coarse for a 96px rounded corner: its
   * interpolation would turn the newly shared tint back into a visibly
   * polygonal box. Match the extension mesh density so both layers resolve
   * one boundary. */
  kSimGroundColumns = 64,
  kSimGroundRows = 48,
  kSimGroundVertexCount = (kSimGroundColumns + 1) * (kSimGroundRows + 1),
  kSimGroundIndexCount = kSimGroundColumns * kSimGroundRows * 6,
};

void DrawSimGroundPlane(ArRenderTexture texture, ArRenderRectI source,
                        ArRenderRectI viewport, const float matrix[16],
                        const SimCullFade *fade) {
  if (!ArRenderTexture_IsValid(texture) ||
      source.w <= 0 || source.h <= 0 ||
      viewport.w <= 0 || viewport.h <= 0)
    return;

  float aspect = (float)viewport.w / (float)viewport.h;
  /* Reused by the synchronous presentation path; together these are too large
   * for a routine stack allocation at the density the rounded boundary
   * requires. */
  static ArRenderVertex2D vertices[kSimGroundVertexCount];
  static int32_t indices[kSimGroundIndexCount];
  int vertex_count = 0, index_count = 0;

  for (int row = 0; row <= kSimGroundRows; row++) {
    float fy = (float)row / (float)kSimGroundRows;
    for (int column = 0; column <= kSimGroundColumns; column++) {
      float fx = (float)column / (float)kSimGroundColumns;
      float texture_x = source.x + fx * source.w;
      float texture_y = source.y + fy * source.h;
      Scene3DPoint projected;
      if (!Scene3D_ProjectWorldPoint(
              matrix, (fx - 0.5f) * aspect, 0.5f - fy, 0.0f,
              viewport.w, viewport.h, &projected))
        return;
      float away = SimCullProximityAt(fade, texture_x, texture_y, source);
      float bright = fade ? 1.0f - away * fade->dim : 1.0f;
      float extent_alpha =
          SimGroundExtentAlphaAt(fade, texture_x, texture_y);
      ArRenderColorF tint = {
        bright, bright, bright,
        (fade ? 1.0f - away * fade->fade : 1.0f) * extent_alpha,
      };
      vertices[vertex_count++] = (ArRenderVertex2D){
        { viewport.x + projected.x, viewport.y + projected.y }, tint,
        { texture_x / (float)kSim3DMaxWidth,
          texture_y / (float)kSim3DMaxHeight },
      };
    }
  }
  for (int row = 0; row < kSimGroundRows; row++) {
    for (int column = 0; column < kSimGroundColumns; column++) {
      int top_left = row * (kSimGroundColumns + 1) + column;
      int bottom_left = top_left + kSimGroundColumns + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }
  (void)ArRenderDevice_DrawGeometry(
      &g_render_device, texture, vertices, vertex_count,
      indices, index_count);
}

/* The ground quad spans exactly one world unit vertically over `source.h`
 * captured rows, so a D3c virtual height in authentic SNES pixels converts to
 * world units with the same scale the flat view uses for sprite size. Lifting
 * along +Z leaves the ground anchor (z = 0) available to the D4 shadow pass. */
float SimHeightWorldUnits(ArRenderRectI source, int virtual_height,
                                 unsigned height_scale_x100) {
  if (source.h <= 0 || !virtual_height) return 0.0f;
  return (float)virtual_height * (float)height_scale_x100 /
      ((float)kPercentScale * (float)source.h);
}

SimBackgroundVoxelRenderParams SimVoxelRenderParams(
    const FrameSlot *slot, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16]) {
  return (SimBackgroundVoxelRenderParams){
    .serial = slot->sim.background_voxel_serial,
    .detail = slot->sim.background_voxel_detail,
    .lod = slot->sim.background_voxel_lod,
    .shading = slot->sim.background_voxel_shading,
    .style = slot->sim.background_voxel_style,
    .facing = slot->sim.background_voxel_facing,
    .render_scale = slot->sim.background_voxel_render_scale,
    .town = slot->sim.town,
    .game_frame = slot->sim.game_frame,
    .landscape_height_pct = slot->sim.landscape_height_pct,
    .camera_x = slot->sim.camera_x,
    .camera_y = slot->sim.camera_y,
    .town_screen_x0 = slot->sim.underlay_screen_x0,
    .light_azimuth_deg = slot->sim.light_azimuth_deg,
    .light_elevation_deg = slot->sim.light_elevation_deg,
    .source = {source.x, source.y, source.w, source.h},
    .viewport = {viewport.x, viewport.y, viewport.w, viewport.h},
    .matrix = matrix,
    .shadow_mask = {0},
    .shadow_opacity_pct = slot->sim.shadow_opacity_pct,
  };
}

float SimTerrainGroundHeightWorld(
    const FrameSlot *slot, ArRenderRectI source, float map_x, float map_y) {
#if AR_SIM3D_TERRAIN_ELEVATION
  if (!slot || source.h <= 0 || !slot->sim.background_voxel_enabled ||
      !SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial))
    return 0.0f;
  return SimTownTerrain_ScaledHeightPixels(
      SimTownTerrain_HeightUnitsAt(slot->sim.town, map_x, map_y),
      (float)slot->sim.landscape_height_pct) / (float)source.h;
#else
  (void)slot; (void)source; (void)map_x; (void)map_y;
  return 0.0f;
#endif
}

float SimTerrainMaximumHeightWorld(
    const FrameSlot *slot, ArRenderRectI source) {
#if AR_SIM3D_TERRAIN_ELEVATION
  if (!slot || source.h <= 0 || !slot->sim.background_voxel_enabled ||
      !SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial))
    return 0.0f;
  return SimTownTerrain_ScaledHeightPixels(
      SimTownTerrain_MaximumUnits(slot->sim.town),
      (float)slot->sim.landscape_height_pct) / (float)source.h;
#else
  (void)slot; (void)source;
  return 0.0f;
#endif
}

/* Grounded art follows the audited surface beneath its feet. Flyers instead
 * share one world-space datum above the town's highest relief, so crossing a
 * ridge changes their clearance and shadow but never physically shoves the
 * actor upward. */
float SimObjectAltitudeBaseWorld(
    const FrameSlot *slot, const SimRenderObject *object,
    ArRenderRectI source, float map_x, float map_y) {
  SimHeightClass height_class = (SimHeightClass)object->height_class;
  if (Sim3D_HeightClassStandsOnTerrain(height_class))
    return SimTerrainGroundHeightWorld(slot, source, map_x, map_y);
  if (height_class == kSimHeightClass_Flying ||
      height_class == kSimHeightClass_FlyingProjectile)
    return SimTerrainMaximumHeightWorld(slot, source);
  return 0.0f;
}

/* Effect-point heights are documented as pixels above their supporting
 * presentation datum. Most are strikes or fires attached to local terrain;
 * Red Demon flame is attached to a flying actor and therefore uses the same
 * town-wide flight datum as that actor. The ballistic volcano fireball is
 * height above its current terrain point so it leaves a raised crater and
 * still converges onto the authentic landing cell. */
static float SimEffectAltitudeBaseWorld(
    const FrameSlot *slot, const SimEffectInstance *effect,
    ArRenderRectI source, float map_x, float map_y) {
  if (!effect) return 0.0f;
  if (effect->kind == kSimEffect_RedDemonFire)
    return SimTerrainMaximumHeightWorld(slot, source);
  return SimTerrainGroundHeightWorld(slot, source, map_x, map_y);
}

bool ProjectSimTexturePoint(
    const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
    float texture_x, float texture_y, float height_world,
    Scene3DPoint *out_point) {
  float fx = (texture_x - source.x) / source.w;
  float fy = (texture_y - source.y) / source.h;
  float aspect = (float)viewport.w / (float)viewport.h;
  Scene3DPoint point;
  if (!Scene3D_ProjectWorldPoint(
          matrix, (fx - 0.5f) * aspect, 0.5f - fy, height_world,
          viewport.w, viewport.h, &point))
    return false;
  point.x += viewport.x;
  point.y += viewport.y;
  *out_point = point;
  return true;
}

static float SimTexturePointDepthScale(
    const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
    float texture_x, float texture_y, float height_world,
    float reference_depth) {
  float fx = (texture_x - source.x) / source.w;
  float fy = (texture_y - source.y) / source.h;
  float aspect = (float)viewport.w / (float)viewport.h;
  return Scene3D_ProjectBillboardScale(
      matrix, (fx - 0.5f) * aspect, 0.5f - fy, height_world,
      reference_depth);
}

bool ProjectSimAnchorAndScale(
    const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
    float texture_x, float texture_y, float height_world,
    float reference_depth, Scene3DPoint *anchor,
    float *scale_x, float *scale_y) {
  if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x, texture_y,
                              height_world, anchor))
    return false;
  float depth_scale = SimTexturePointDepthScale(
      matrix, source, viewport, texture_x, texture_y, height_world,
      reference_depth);
  if (depth_scale <= 0.0f) return false;
  if (scale_x) *scale_x = (float)viewport.w / source.w * depth_scale;
  if (scale_y) *scale_y = (float)viewport.h / source.h * depth_scale;
  return true;
}

/* The world origin is a parameter rather than a field read, so a caller
 * walking an effect's retained path can project each earlier position without
 * copying the whole instance to move two numbers. */
bool ProjectSimEffectPointAt(
    const FrameSlot *slot, const SimEffectInstance *effect,
    uint16_t world_x, uint16_t world_y,
    const SimEffectLocalPoint *local, ArRenderRectI source,
    ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    Scene3DPoint *point, float *scale_x, float *scale_y) {
  if (!local || effect->geometry.kind != kSimEffectGeometry_Point) return false;
  int record_screen_x = (int16_t)(uint16_t)(
      world_x - slot->sim.camera_x);
  int record_screen_y = (int16_t)(uint16_t)(
      world_y - slot->sim.camera_y);
  float texture_x = slot->ws_extra + record_screen_x;
  float texture_y = record_screen_y;
  float height_world = SimHeightWorldUnits(
      source, local->height, slot->sim.height_scale_x100);
  float support_x = (float)(int16_t)(uint16_t)(world_x + local->x);
  float support_y = (float)(int16_t)(uint16_t)(world_y + local->y);
  if (effect->kind == kSimEffect_VolcanoFireball) {
    /* Its record origin is the renderer-published crater/flight point. The
     * local point addresses pixels inside the fireball art and must not move
     * the supporting terrain sample away from that authored anchor. */
    support_x = (float)(int16_t)world_x;
    support_y = (float)(int16_t)world_y;
  }

  switch ((SimEffectGeometrySpace)effect->geometry.space) {
    case kSimEffectSpace_Screen:
      point->x = viewport.x + local->x * (float)viewport.w / source.w;
      point->y = viewport.y + local->y * (float)viewport.h / source.h;
      if (scale_x) *scale_x = (float)viewport.w / source.w;
      if (scale_y) *scale_y = (float)viewport.h / source.h;
      return true;
    case kSimEffectSpace_WorldLocal:
      texture_x += local->x;
      texture_y += local->y;
      height_world += SimEffectAltitudeBaseWorld(
          slot, effect, source, support_x, support_y);
      return ProjectSimAnchorAndScale(
          matrix, source, viewport, texture_x, texture_y, height_world,
          Scene3D_AutoFitDistance(camera->fov_y), point, scale_x, scale_y);
    case kSimEffectSpace_RecordLocal:
      height_world += SimEffectAltitudeBaseWorld(
          slot, effect, source, support_x, support_y);
      break;
    default:
      return false;
  }

  Scene3DPoint anchor;
  float sx, sy;
  if (!ProjectSimAnchorAndScale(
          matrix, source, viewport, texture_x, texture_y, height_world,
          Scene3D_AutoFitDistance(camera->fov_y), &anchor, &sx, &sy))
    return false;
  float height_pop = SimBillboardHeightPop(
      source, height_world, slot->sim.height_pop_pct);
  sx *= height_pop;
  sy *= height_pop;
  point->x = anchor.x + local->x * sx;
  point->y = anchor.y + local->y * sy;
  if (scale_x) *scale_x = sx;
  if (scale_y) *scale_y = sy;
  return true;
}

bool ProjectSimEffectPoint(
    const FrameSlot *slot, const SimEffectInstance *effect,
    const SimEffectLocalPoint *local, ArRenderRectI source,
    ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16],
    Scene3DPoint *point, float *scale_x, float *scale_y) {
  return ProjectSimEffectPointAt(
      slot, effect, effect->world_x, effect->world_y, local, source, viewport,
      camera, matrix, point, scale_x, scale_y);
}
