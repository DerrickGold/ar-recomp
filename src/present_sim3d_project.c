/* Actor/effect placement over the shared SIM camera geometry. Native path
 * positions and altitude classes stay here; geometry.c owns their common
 * texture/world projection without depending on live actor/effect state. */
#include "present_sim3d_internal.h"
#include "present_sim3d_project.h"
#include "sim/sim3d.h"
#include "sim/sim_world_navigation_terrain.h"
#include <math.h>

/* Preserve the established extra flight-height scale. Paired with the
 * shadow's footprint shrink, a rising actor grows while its shadow shrinks.
 * This is a pure placement operation, independent of shadow render targets. */
float SimBillboardHeightPop(ArRenderRectI source, float height_world, unsigned height_pop_pct) {
  if (height_world <= 0 || !height_pop_pct || source.h <= 0) return 1;
  const float reference = (float)kSimVirtualHeight_Flying/source.h;
  return 1 + height_world/reference*height_pop_pct/kPercentScale;
}

/* Adapt SIM's support/altitude policy to one registered curved floor. Only
 * the support lookup is clamped; incoming trajectories retain their XY.
 * Flyers register a stable town-centre datum, not the coastline below them.
 * Actor/mountain rise remains pixels, never scaled by landscape height. */
bool ProjectSimCurvedAnchor(const FrameSlot *slot,
    const PresentSimGlobeProjection *globe,
    float native_x, float native_y, float support_units, float altitude_pixels,
    bool aerial, PresentSimGlobeProjectedPoint *point) {
  if (!slot || !globe) return false;
  const float x = aerial ? 16 : fmaxf(0,fminf(native_x/16,32));
  const float y = aerial ? 16 : fmaxf(0,fminf(native_y/16,32));
  float support;
  return SimWorldNavigationTerrain_RegisterTownFloor(
          globe->map.town,x,y,support_units,&support) &&
      PresentSimGlobeProject_Point(globe,native_x,native_y,support,
          altitude_pixels,point);
}
/* Grounded art follows the audited surface beneath its feet. Flyers instead
 * share one world-space datum above the town's highest relief, so crossing a
 * ridge changes their clearance and shadow but never physically shoves the
 * actor upward. */
float SimObjectAltitudeBaseUnits(
    const FrameSlot *slot, const SimRenderObject *object,
    float map_x, float map_y) {
  SimHeightClass height_class = (SimHeightClass)object->height_class;
  if (Sim3D_HeightClassStandsOnTerrain(height_class))
    return SimTerrainGroundHeightUnits(slot, map_x, map_y);
  if (height_class == kSimHeightClass_Flying ||
      height_class == kSimHeightClass_FlyingProjectile)
    return SimTerrainMaximumHeightUnits(slot);
  return 0.0f;
}

float SimObjectAltitudeBaseWorld(const FrameSlot *slot, const SimRenderObject *object,
    ArRenderRectI source, float map_x, float map_y) {
  return SimTerrainHeightWorld(slot,source,SimObjectAltitudeBaseUnits(slot,object,map_x,map_y));
}

/* Effect-point heights are documented as pixels above their supporting
 * presentation datum. Most are strikes or fires attached to local terrain;
 * Red Demon flame is attached to a flying actor and therefore uses the same
 * town-wide flight datum as that actor. The ballistic volcano fireball is
 * height above its current terrain point so it leaves a raised crater and
 * still converges onto the authentic landing cell. */
static float SimEffectAltitudeBaseUnits(
    const FrameSlot *slot, const SimEffectInstance *effect,
    float map_x, float map_y) {
  if (!effect) return 0.0f;
  if (effect->kind == kSimEffect_RedDemonFire)
    return SimTerrainMaximumHeightUnits(slot);
  return SimTerrainGroundHeightUnits(slot, map_x, map_y);
}


/* The world origin is a parameter rather than a field read, so a caller
 * walking an effect's retained path can project each earlier position without
 * copying the whole instance to move two numbers. */
bool ProjectSimEffectPointAt(
    const FrameSlot *slot, const SimEffectInstance *effect,
    uint16_t world_x, uint16_t world_y,
    const SimEffectLocalPoint *local, const SimSceneProjection *scene,
    Scene3DPoint *point, float *scale_x, float *scale_y) {
  if (!slot || !scene || !point || !local || !effect ||
      scene->source.w <= 0 || scene->source.h <= 0 ||
      scene->viewport.w <= 0 || scene->viewport.h <= 0 ||
      effect->geometry.kind != kSimEffectGeometry_Point) return false;
  const ArRenderRectI source = scene->source, viewport = scene->viewport;
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
      break;
    case kSimEffectSpace_RecordLocal:
      break;
    default:
      return false;
  }

  Scene3DPoint anchor;
  float sx, sy;
  if (!scene->globe && (!scene->matrix || !scene->camera)) return false;
  const float support_units = SimEffectAltitudeBaseUnits(slot,effect,support_x,support_y);
  const float support = SimTerrainHeightWorld(slot,source,support_units);
  if (scene->globe) {
    PresentSimGlobeProjectedPoint projected;
    if (!ProjectSimCurvedAnchor(slot,scene->globe,
            texture_x-slot->ws_extra+slot->sim.camera_x,texture_y+slot->sim.camera_y,
            support_units,height_world*source.h,effect->kind == kSimEffect_RedDemonFire,&projected)) return false;
    anchor = projected.screen; sx = projected.pixel_scale[0]; sy = projected.pixel_scale[1];
  } else if (!ProjectSimAnchorAndScale(
          scene->matrix, source, viewport, texture_x, texture_y, height_world+support,
          Scene3D_AutoFitDistance(scene->camera->fov_y), &anchor, &sx, &sy))
    return false;
  *point = anchor;
  if (effect->geometry.space == kSimEffectSpace_RecordLocal) {
    const float pop = SimBillboardHeightPop(source,height_world+support,slot->sim.height_pop_pct);
    sx *= pop; sy *= pop;
    point->x += local->x*sx; point->y += local->y*sy;
  }
  if (scale_x) *scale_x = sx;
  if (scale_y) *scale_y = sy;
  return true;
}

bool ProjectSimEffectPoint(
    const FrameSlot *slot, const SimEffectInstance *effect,
    const SimEffectLocalPoint *local, const SimSceneProjection *scene,
    Scene3DPoint *point, float *scale_x, float *scale_y) {
  if (!effect) return false;
  return ProjectSimEffectPointAt(
      slot, effect, effect->world_x, effect->world_y, local, scene, point, scale_x, scale_y);
}
