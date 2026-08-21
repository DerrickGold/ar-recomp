/* T2a: the SIM-mode 3D town + world-navigation renderer, split verbatim out of
 * present.c (which was ~4,550 lines, the majority of it this renderer). Every
 * definition here was moved unchanged; the split introduced no behaviour.
 *
 * The D6 no-live-globals invariant that present.c carries applies here too:
 * this file must NOT declare or extern g_ppu, g_settings, g_snes_width,
 * g_ws_extra, g_active_pixel_aspect, or call Settings_Visible*(). Every
 * present-time decision comes from the `const FrameSlot *` handed in.
 * present_internal.h is the present.c<->present_sim3d.c boundary; it exposes
 * present.c internals to this file, never live game state. */

#include <SDL3/SDL.h>
#include "present_sim3d_internal.h"
#include "present_sim3d_clouds.h"
#include "present_sim3d_effects.h"
#include "present_sim3d_shadows.h"
#include "present_sim3d_terrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crt_post.h"
#include "render_capabilities.h"
#include "sim/sim_background_voxels.h"
#include "sim/sim3d.h"
#include "sim/sim3d_camera_limits.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_world_navigation_scene.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
#include "present_internal.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif


extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
extern SDL_Texture *g_sim_obj_atlas_texture;

extern SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
extern SDL_Texture *g_sim3d_flat_texture;





/* D4c draws the same billboards a second and third time to build a rim band,
 * so the geometry lives in one loop rather than being re-derived. A NULL pass
 * is the ordinary coloured draw. */
typedef enum SimBillboardPassKind {
  kSimBillboardPass_Fill,  /* light-coloured silhouette, offset toward the light */
  kSimBillboardPass_Mask,  /* keep only the part inside the sprite's own body */
} SimBillboardPassKind;

/* Which tier a billboard band draws. The world and the menu now composite at
 * different depths -- the menu group is deferred past the atmospheric effects
 * -- so a band has to be able to draw one without the other. */
typedef enum SimObjectTierFilter {
  kSimTierFilter_World,
  kSimTierFilter_Fixed,
} SimObjectTierFilter;

typedef struct SimBillboardPass {
  SimBillboardPassKind kind;
  float offset_x, offset_y;
  /* W4-1: the blend mode this pass needs on the atlas texture.
   *
   * It has to travel WITH the pass rather than be set by the caller
   * beforehand, because DrawSimObjectPriority sets the atlas blend mode itself
   * on entry (it is the ordinary draw's mode, and the two main-path callers
   * rely on that). A caller that set a mode and then called in had it silently
   * overwritten before anything was drawn — which is exactly how the rim-light
   * mask pass lost its blend mode and stopped trimming the rim. */
  SDL_BlendMode blend;
} SimBillboardPass;

/* Defined with the rim-light code below, since the capability it latches belongs
 * to that effect; declared here because the billboard draw is the only caller. */
/* Single source of truth for "what blend mode does this draw use", so the
 * caller-side pass setup and the callee-side set cannot disagree. A NULL pass is
 * the ordinary coloured draw. */
static SDL_BlendMode SimBillboardPassBlend(const SimBillboardPass *pass) {
  return (pass && pass->blend != SDL_BLENDMODE_INVALID)
      ? pass->blend
      : SDL_BLENDMODE_BLEND;
}

/* Strict "a must be drawn after b" for the in-band painter sort. Strict, not
 * "greater or equal": returning true for equal keys would make the insertion
 * sort unstable and lose the reverse-OAM tiebreak that keeps a multi-part
 * actor's authored overlap. */
static bool SimObjectSortsAfter(const SimRenderObject *a,
                                const SimRenderObject *b,
                                float a_depth, float b_depth) {
  bool a_overhead = (a->traits & kSimObjectTrait_Overhead) != 0;
  bool b_overhead = (b->traits & kSimObjectTrait_Overhead) != 0;
  if (a_overhead != b_overhead) return a_overhead;
  return a_depth < b_depth;
}

typedef enum SimObjectOverheadFilter {
  kSimObjectOverhead_All,
  kSimObjectOverhead_GroundOnly,
  kSimObjectOverhead_Only,
} SimObjectOverheadFilter;

typedef enum SimObjectSelectionFilter {
  kSimObjectSelection_Exclude,
  kSimObjectSelection_Only,
} SimObjectSelectionFilter;


static float SimObjectGroundDepth(
    const FrameSlot *slot, const SimRenderObject *object,
    SDL_Rect source, SDL_Rect viewport, const float matrix[16]) {
  int world_x, world_y;
  SimObjectDrawnWorld(object, &world_x, &world_y);
  float depth_map_y = (float)world_y;
  float mountain_height = 0.0f;
  if (Sim3D_HeightClassStandsOnTerrain(
          (SimHeightClass)object->height_class)) {
    float surface_map_y;
    if (SimBackgroundVoxels_MountainSurface(
            world_x, world_y, &surface_map_y, &mountain_height))
      depth_map_y = surface_map_y;
  }
  int screen_x = (int16_t)(uint16_t)(world_x - slot->sim.camera_x);
  float screen_y = depth_map_y - slot->sim.camera_y;
  float texture_x = slot->ws_extra + screen_x;
  float texture_y = screen_y;
  float fx = (texture_x - source.x) / source.w;
  float fy = (texture_y - source.y) / source.h;
  float aspect = (float)viewport.w / viewport.h;
  float ground_height = SimTerrainGroundHeightWorld(
      slot, source, (float)world_x, depth_map_y);
  if (source.h > 0) ground_height += mountain_height / (float)source.h;
  return Scene3D_ClipDepth(
      matrix, (fx - 0.5f) * aspect, 0.5f - fy, ground_height);
}

/* Which terrain an actor is standing on, from the authentic 2D cell map. */
typedef enum SimObjectTerrainFilter {
  kSimObjectTerrain_Any,
  kSimObjectTerrain_GroundOnly,
  kSimObjectTerrain_MountainOnly,
} SimObjectTerrainFilter;

/* An actor's foot is already in town-map space -- SimObjectGroundDepth uses
 * foot_x/foot_y interchangeably with world_x/world_y -- so its cell is one
 * shift away, and the published scene answers whether that cell is mountain. */
static bool SimObjectOnMountainTerrain(const SimRenderObject *object) {
  /* The DRAWN cell, for the same reason the depth sort uses it: this decides
   * which side of the terrain art the sprite composes on, and art held away
   * from its record's cell has to be tested where it actually appears. */
  int map_x, map_y;
  SimObjectDrawnWorld(object, &map_x, &map_y);
  int cell_x = map_x / kSimTownCellPixels;
  int cell_y = map_y / kSimTownCellPixels;
  /* Draw above the terrain art when standing ON a mountain, and also when no
   * mountain lies between this cell and the camera: a mass north of the actor
   * is behind it and must not clip the top of its sprite. */
  return SimBackgroundVoxels_CellIsMountain(cell_x, cell_y) ||
      !SimBackgroundVoxels_MountainInFrontOf(cell_x, cell_y);
}

/* Sixteen positional arguments, four of them bare bools, was past the point
 * where a call site could be read. The projection inputs travel together and
 * never vary within a frame; the filters are what each call is actually
 * choosing. */
typedef struct SimObjectDrawScene {
  const FrameSlot *slot;
  SDL_Rect source;
  SDL_Rect viewport;
  const Scene3DCamera *camera;
  const float *matrix;
  bool project_world;
  bool virtual_height;
} SimObjectDrawScene;

typedef struct SimObjectDrawFilters {
  SimObjectTierFilter tier;
  SimObjectOverheadFilter overhead;
  SimObjectSelectionFilter selection;
  SimObjectTerrainFilter terrain;
  bool depth;
  float minimum_depth, maximum_depth;
} SimObjectDrawFilters;

static void DrawSimObjectPriorityFiltered(
    const SimObjectDrawScene *scene, int priority,
    const SimObjectDrawFilters *filters,
    const SimBillboardPass *pass) {
  const FrameSlot *slot = scene->slot;
  SDL_Rect source = scene->source, viewport = scene->viewport;
  const Scene3DCamera *camera = scene->camera;
  const float *matrix = scene->matrix;
  bool project_world = scene->project_world;
  bool virtual_height = scene->virtual_height;
  SimObjectTierFilter tier_filter = filters->tier;
  SimObjectOverheadFilter overhead_filter = filters->overhead;
  SimObjectSelectionFilter selection_filter = filters->selection;
  SimObjectTerrainFilter terrain_filter = filters->terrain;
  bool depth_filter = filters->depth;
  float minimum_depth = filters->minimum_depth;
  float maximum_depth = filters->maximum_depth;
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  /* W4-2: a rejected blend mode means this pass cannot draw correctly, so bail
   * rather than draw with whatever mode happened to be set. */
  if (!SimApplyAtlasBlendMode(SimBillboardPassBlend(pass))) return;
  float flat_scale_x = (float)viewport.w / source.w;
  float flat_scale_y = (float)viewport.h / source.h;

  /* Earlier OAM slots own overlapping opaque pixels. SDL's later draw wins,
   * so the base traversal is reverse OAM order.
   *
   * Projected billboards additionally sort back-to-front by ground depth
   * within the band. On the flat SNES screen, OAM order alone decides overlap
   * and the result is correct because everything shares one plane; once the
   * map is projected, two actors on different map rows genuinely are at
   * different distances, and honouring OAM order there lets a far actor paint
   * over a near one. Sorting is confined to the band, so the hardware priority
   * bands still decide the coarse layering, and reverse OAM order remains the
   * tiebreak so co-located sprites (multi-part actors) keep their authored
   * overlap and the order stays stable frame to frame. */
  int order[kSimMaxRenderObjects];
  float depth[kSimMaxRenderObjects];
  int order_count = 0;
  for (int i = (int)slot->sim.object_count - 1; i >= 0; i--) {
    const SimRenderObject *object = &slot->sim.objects[i];
    bool fixed = object->tier != kSimRecordTier_World;
    if (!object->atlas_valid || object->priority != priority ||
        fixed != (tier_filter == kSimTierFilter_Fixed) ||
        SimObjectIsPromotedHud(slot, object))
      continue;
    /* Only art that actually STANDS on terrain belongs in the bands terrain is
     * allowed to hide. The Overhead trait covers authored overhead art, but it
     * is not the whole set: angel arrows and thrown orbs fly over the town
     * without carrying it, and putting them under the composite let evergreens
     * and mountains swallow a projectile passing in front of them. Height
     * class is the ROM-derived answer to "is this on the ground", so the two
     * together decide the band. */
    bool overhead = (object->traits & kSimObjectTrait_Overhead) != 0 ||
        !Sim3D_HeightClassIsOccludable(
            (SimHeightClass)object->height_class);
    if (overhead_filter == kSimObjectOverhead_GroundOnly && overhead)
      continue;
    if (overhead_filter == kSimObjectOverhead_Only && !overhead)
      continue;
    bool selection =
        (object->traits & kSimObjectTrait_SelectionOverlay) != 0;
    if (selection_filter == kSimObjectSelection_Exclude && selection)
      continue;
    if (selection_filter == kSimObjectSelection_Only && !selection)
      continue;
    if (terrain_filter != kSimObjectTerrain_Any) {
      bool on_mountain = SimObjectOnMountainTerrain(object);
      if ((terrain_filter == kSimObjectTerrain_MountainOnly) != on_mountain)
        continue;
    }
    depth[i] = project_world
        ? SimObjectGroundDepth(slot, object, source, viewport, matrix)
        : 0.0f;
    if (depth_filter &&
        (depth[i] < minimum_depth || depth[i] >= maximum_depth))
      continue;
    order[order_count++] = i;
  }
  if (project_world) {
    /* Sort key: overhead art last, then projected ground depth far-to-near.
     *
     * Clip depth uses both ground coordinates, so yawing the camera cannot
     * leave overlap tied to the old top-down Y axis. Foot anchors make this a
     * ground-contact comparison rather than a record-origin approximation.
     * Overhead art is exempt -- its composition hangs above the row the record
     * sits on, so depth-sorting it lets a ground object draw over a cloud.
     *
     * Insertion sort: a band holds a few dozen objects at most, and a stable
     * sort is what preserves the OAM tiebreak above -- including among the
     * overhead objects themselves, which keep the ROM's authored overlap. */
    for (int i = 1; i < order_count; i++) {
      int index = order[i];
      int j = i - 1;
      while (j >= 0 && SimObjectSortsAfter(
                           &slot->sim.objects[order[j]],
                           &slot->sim.objects[index],
                           depth[order[j]], depth[index])) {
        order[j + 1] = order[j];
        j--;
      }
      order[j + 1] = index;
    }
  }
  for (int n = 0; n < order_count; n++) {
    const SimRenderObject *object = &slot->sim.objects[order[n]];

    /* The rim is a silhouette effect: it must not inherit the sprite's
     * colour-math alpha, and map-plane art lies on the ground rather than
     * standing up, so it has no silhouette to light. */
    if (pass && (object->traits & kSimObjectTrait_MapPlane)) continue;
    /* Art the ROM emits that this view must not draw. */
    if (object->hidden) continue;
    bool half_add = !pass && slot->sim.object_half_add &&
        object->color_math_eligible;
    SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, half_add ? 128 : 255);

    int record_screen_x = (int16_t)(uint16_t)(
        object->world_x + object->offset_x - slot->sim.camera_x);
    int record_screen_y = (int16_t)(uint16_t)(
        object->world_y + object->offset_y - slot->sim.camera_y);
    if (project_world && (object->traits & kSimObjectTrait_MapPlane)) {
      DrawSimMapPlaneObject(slot, object, slot->ws_extra + record_screen_x,
                            record_screen_y, source, viewport, matrix);
      continue;
    }

    /* The classified anchor is part of the object descriptor, not of the
     * VirtualHeight switch: projectiles and ground-targeted effects keep the
     * record origin even when their height resolves to zero. */
    bool foot_anchor = object->tier == kSimRecordTier_World &&
        !(object->traits & kSimObjectTrait_RecordOriginAnchor);
    int foot_dx = foot_anchor ? object->foot_x - (int)object->world_x : 0;
    int foot_dy = foot_anchor ? object->foot_y - (int)object->world_y : 0;
    int screen_anchor_x = (int16_t)(uint16_t)(
        object->world_x + foot_dx - slot->sim.camera_x);
    int screen_anchor_y = (int16_t)(uint16_t)(
        object->world_y + foot_dy - slot->sim.camera_y);
    float texture_anchor_x = slot->ws_extra + screen_anchor_x;
    float texture_anchor_y = screen_anchor_y;
    float scale_x = flat_scale_x;
    float scale_y = flat_scale_y;
    float height_world = 0.0f;
    Scene3DPoint anchor;
    if (project_world && object->tier == kSimRecordTier_World) {
      const float map_anchor_x =
          foot_anchor ? object->foot_x : (float)object->world_x;
      const float map_anchor_y =
          foot_anchor ? object->foot_y : (float)object->world_y;
      /* An actor the 2D map puts on a mountain is standing on art the renderer
       * shears upward, so it has to take the same shear or it is drawn at the
       * mass's foot with the slope rising behind it. Terrain altitude is NOT
       * scaled by the player's object-height setting: it has to agree with the
       * mountain geometry, which converts its own pixels straight through
       * `height / source.h`. */
      float surface_map_y = 0.0f, surface_height = 0.0f;
      bool mountain_surface = Sim3D_HeightClassStandsOnTerrain(
              (SimHeightClass)object->height_class) &&
          SimBackgroundVoxels_MountainSurface(
              foot_anchor ? object->foot_x : (int)object->world_x,
              foot_anchor ? object->foot_y : (int)object->world_y,
              &surface_map_y, &surface_height) && source.h > 0;
      if (mountain_surface) {
        int map_y = foot_anchor ? object->foot_y : (int)object->world_y;
        texture_anchor_y += surface_map_y - (float)map_y;
      }
      /* Sample terrain where the sheared mountain surface is actually drawn,
       * not at the pre-shear flat-map row. This is invisible on a flat datum
       * but essential when a mountain or volcano is based on a cliff. */
      height_world = SimObjectAltitudeBaseWorld(
          slot, object, source, map_anchor_x,
          mountain_surface ? surface_map_y : map_anchor_y);
      height_world += virtual_height
          ? SimHeightWorldUnits(source, object->virtual_height,
                                slot->sim.height_scale_x100)
          : 0.0f;
      if (mountain_surface)
        height_world += surface_height / (float)source.h;
      /* A bubble the ROM pins to a structure's record cell rides on top of
       * that structure's model instead of sitting inside its foot. */
      if ((object->traits & kSimObjectTrait_StructureOverlay) && source.h > 0)
        /* From the BOTTOM of the bubble art, not its anchor. These
         * compositions are a 16x32 stack anchored at the top, so the record
         * origin sits a good two cells above the roof the bubble is resting
         * on -- sampling there finds empty ground. */
        height_world += SimBackgroundVoxels_StructureHeight(
            (int)object->world_x,
            (int)object->world_y + object->local_y1) / (float)source.h;
      if (!ProjectSimAnchorAndScale(
              matrix, source, viewport, texture_anchor_x, texture_anchor_y,
              height_world, Scene3D_AutoFitDistance(camera->fov_y),
              &anchor, &scale_x, &scale_y))
        continue;
      float height_pop = SimBillboardHeightPop(
          source, height_world, slot->sim.height_pop_pct);
      scale_x *= height_pop;
      scale_y *= height_pop;
    } else {
      anchor.x = viewport.x +
          (texture_anchor_x - source.x) * flat_scale_x;
      anchor.y = viewport.y +
          (texture_anchor_y - source.y) * flat_scale_y;
    }

    SDL_FRect atlas = {
      object->atlas_x, object->atlas_y,
      object->atlas_w, object->atlas_h,
    };
    SDL_FRect destination = {
      anchor.x + (object->local_x0 - foot_dx) * scale_x,
      anchor.y + (object->local_y0 - foot_dy) * scale_y,
      (object->local_x1 - object->local_x0) * scale_x,
      (object->local_y1 - object->local_y0) * scale_y,
    };
    if (pass) {
      destination.x += pass->offset_x;
      destination.y += pass->offset_y;
    }
    if (destination.w <= 0.0f || destination.h <= 0.0f) continue;

    /* Travelling art is NOT rotated here. The one family that flies its own
     * trajectory -- the eruption fireball -- has its billboard withheld and is
     * drawn instead at the head of its arc by DrawSimEffectFireballHeads,
     * which turns it there. Rotating in this pass as well would be a second
     * implementation of the same angle, reachable by nothing. */
    if (SDL_RenderTexture(g_renderer, g_sim_obj_atlas_texture,
                          &atlas, &destination)) {
      Sim3DPerformance_AddDraw(0, 0);
    }
  }
  SDL_SetTextureAlphaMod(g_sim_obj_atlas_texture, 255);
}

static void DrawSimObjectPriorityTerrain(
    const FrameSlot *slot, int priority, SimObjectTierFilter tier_filter,
    SimObjectTerrainFilter terrain_filter,
    bool project_world,
    bool virtual_height, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16],
    const SimBillboardPass *pass) {
  SimObjectDrawScene scene = {
    slot, source, viewport, camera, matrix, project_world, virtual_height,
  };
  SimObjectDrawFilters filters = {
    tier_filter, kSimObjectOverhead_All, kSimObjectSelection_Exclude,
    terrain_filter, false, 0.0f, 0.0f,
  };
  DrawSimObjectPriorityFiltered(&scene, priority, &filters, pass);
}

static void DrawSimObjectPriority(
    const FrameSlot *slot, int priority, SimObjectTierFilter tier_filter,
    bool project_world,
    bool virtual_height, SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16],
    const SimBillboardPass *pass) {
  DrawSimObjectPriorityTerrain(
      slot, priority, tier_filter, kSimObjectTerrain_Any, project_world,
      virtual_height, source, viewport, camera, matrix, pass);
}

typedef struct SimVoxelBillboardLayerContext {
  const FrameSlot *slot;
  int priority;
  bool virtual_height;
  bool rim_light;
  const Scene3DCamera *camera;
} SimVoxelBillboardLayerContext;

static void DrawSimRimLight(
    const FrameSlot *slot, int priority, bool virtual_height,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16], SimObjectTerrainFilter terrain_filter);

static void DrawSimVoxelBillboardLayer(
    void *userdata,
    const SimBackgroundVoxelRenderParams *params,
    float minimum_depth, float maximum_depth,
    SimBackgroundVoxelActorBand band) {
  Sim3DPerformanceScope performance =
      Sim3DPerformance_Begin(kSim3DPerformance_Billboard);
  SimVoxelBillboardLayerContext *context = userdata;
  bool overhead = band == kSimBackgroundVoxelActorBand_Overhead;
  /* Overhead art hangs above the row its record sits on and is authored to
   * clear the terrain, so it takes no terrain split. */
  SimObjectTerrainFilter terrain = overhead
      ? kSimObjectTerrain_Any
      : (band == kSimBackgroundVoxelActorBand_Mountain
             ? kSimObjectTerrain_MountainOnly
             : kSimObjectTerrain_GroundOnly);
  SimObjectDrawScene scene = {
    context->slot, params->source, params->viewport, context->camera,
    params->matrix, true, context->virtual_height,
  };
  SimObjectDrawFilters filters = {
    kSimTierFilter_World,
    overhead ? kSimObjectOverhead_Only : kSimObjectOverhead_GroundOnly,
    kSimObjectSelection_Exclude, terrain,
    !overhead, minimum_depth, maximum_depth,
  };
  DrawSimObjectPriorityFiltered(&scene, context->priority, &filters, NULL);
  /* Immediately after its own sprites, so the rim is covered by whatever
   * covers them. Overhead art keeps the whole-band rim it has always had. */
  if (context->rim_light && !overhead)
    DrawSimRimLight(context->slot, context->priority, context->virtual_height,
                    params->source, params->viewport, context->camera,
                    params->matrix, terrain);
  Sim3DPerformance_End(performance);
}

/* Selection art is projected onto the map like any other MapPlane object, but
 * it is interaction feedback rather than world scenery. Draw it after every
 * world layer (and atmospheric cover) while retaining authentic priority and
 * reverse-OAM order among selectors themselves. Fixed menu planes remain last
 * and can still cover a selector with an actual UI panel. */
static void DrawSimSelectionOverlays(
    const FrameSlot *slot, bool virtual_height,
    SDL_Rect source, SDL_Rect viewport,
    const Scene3DCamera *camera, const float matrix[16]) {
  static const SimObjectTierFilter tiers[] = {
    kSimTierFilter_World,
    kSimTierFilter_Fixed,
  };
  uint8_t priority_masks[2] = {0};
  for (size_t at = 0; at < slot->sim.object_count; at++) {
    const SimRenderObject *object = &slot->sim.objects[at];
    if (!object->atlas_valid || object->priority >= 4 ||
        !(object->traits & kSimObjectTrait_SelectionOverlay) ||
        SimObjectIsPromotedHud(slot, object))
      continue;
    int tier = object->tier == kSimRecordTier_World ? 0 : 1;
    priority_masks[tier] |= (uint8_t)(1u << object->priority);
  }
  for (int priority = 0; priority < 4; priority++)
    for (size_t tier = 0; tier < sizeof(tiers) / sizeof(tiers[0]); tier++) {
      if (!(priority_masks[tier] & (1u << priority))) continue;
      SimObjectDrawScene scene = {
        slot, source, viewport, camera, matrix, true, virtual_height,
      };
      SimObjectDrawFilters filters = {
        tiers[tier], kSimObjectOverhead_All, kSimObjectSelection_Only,
        kSimObjectTerrain_Any, false, 0.0f, 0.0f,
      };
      DrawSimObjectPriorityFiltered(&scene, priority, &filters, NULL);
    }
}

/* D4c rim light. Sprites have no normals, so the only physically meaningful
 * lighting product left is an edge: the band of a silhouette that faces the
 * light. It is built with two silhouette draws rather than a shader — one
 * offset toward the light, then intersected with the sprite's own body —
 * leaving a band just inside the lit edge, composited additively.
 *
 * Intersecting rather than subtracting is what keeps this honest. The first
 * version subtracted, which put the band OUTSIDE the silhouette: a halo
 * painted onto the background, which reads as the sprite glowing rather than
 * being lit, and which scales with strength so no amount of dialling it back
 * fixes the look. The action-stage rim shader (src/shaders/rim.frag.glsl)
 * has the same in-place property by construction — its `edge` term is
 * multiplied by the pixel's own alpha — so both paths now light only pixels
 * the sprite already owns.
 *
 * Restricted to world billboards by construction: the pass loop skips
 * map-plane art, and the band is composited immediately after its own priority
 * band, so it can never light the ground, the HUD, or a later band's sprite. */
static SDL_Texture *s_sim_rim_texture;
static int s_sim_rim_w, s_sim_rim_h;

static const SDL_Color kSimRimColor = { 255, 244, 214, 255 };

static SDL_Texture *EnsureSimRimTexture(int w, int h) {
  if (!g_renderer || w <= 0 || h <= 0) return NULL;
  if (s_sim_rim_texture && s_sim_rim_w == w && s_sim_rim_h == h)
    return s_sim_rim_texture;
  if (s_sim_rim_texture) SDL_DestroyTexture(s_sim_rim_texture);
  s_sim_rim_texture = CreateSimShadowTarget(w, h);
  s_sim_rim_w = w;
  s_sim_rim_h = h;
  return s_sim_rim_texture;
}

/* Multiplies destination alpha by source alpha while leaving destination
 * colour, i.e. keeps only the overlap. Applied to the offset silhouette with
 * the sprite at its true position, this trims the rim band back inside the
 * sprite so it can never touch a background pixel.
 *
 * W4-2: SDL_ComposeCustomBlendMode only COMPOSES a value — SDL_blendmode.h
 * documents that "not all renderers support" custom modes and directs callers to
 * the per-renderer support notes, and the composing call itself cannot report
 * that. Support is discovered only when the mode is handed to
 * SDL_SetTextureBlendMode, whose bool return we must therefore check. Until that
 * happens the mode is "composed but unproven", which is why this returns
 * SDL_BLENDMODE_INVALID once a set has actually failed rather than optimistically
 * forever. */
/* Reads true until a set actually fails. Settings sees this only through the
 * atomic, read-only capability accessor below. */
SDL_AtomicInt s_sim_rim_mask_supported = { .value = 1 };

static SDL_BlendMode SimRimMaskBlend(void) {
  if (!Present_SimRimMaskSupported()) return SDL_BLENDMODE_INVALID;
  static SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
  if (mode == SDL_BLENDMODE_INVALID)
    mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
  return mode;
}

/* Applies a pass's blend mode and reports whether the renderer accepted it.
 * A custom mode that the backend cannot honour must disable the effect rather
 * than silently draw with whatever mode was set before — that would produce
 * exactly the untrimmed silhouette W4-1 fixed. */
bool SimApplyAtlasBlendMode(SDL_BlendMode blend) {
  if (SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, blend)) return true;
  if (SDL_CompareAndSwapAtomicInt(&s_sim_rim_mask_supported, 1, 0)) {
    fprintf(stderr,
            "[sim3d-rim] this renderer rejected the rim mask blend mode (%s) — "
            "rim light disabled\n", SDL_GetError());
  }
  return false;
}

/* Screen-space direction the rim sits on. The lateral part is the opposite of
 * the shadow shear (the light is on the far side from its own shadow), plus a
 * constant upward bias so an overhead light — the shipped default, where the
 * shear is nearly zero — still lights the top edge rather than nothing. */
static void SimRimOffset(const FrameSlot *slot, float distance,
                         float *offset_x, float *offset_y) {
  float light_x, light_y;
  SimShadowLight(slot, &light_x, &light_y);
  float x = -light_x;
  /* +world y is up-screen, so a light biased away from the camera lifts the
   * rim; the constant term is the overhead component. */
  float y = -(light_y + 1.0f);
  float length = sqrtf(x * x + y * y);
  if (length < 0.0001f) { *offset_x = 0.0f; *offset_y = -distance; return; }
  *offset_x = x / length * distance;
  *offset_y = y / length * distance;
}

/* `terrain_filter` matters as much as the sprite draw it accompanies. The rim
 * is composited from its own full-viewport target, so a band built from every
 * actor and added after the town composite paints the outlines of actors the
 * composite has just hidden -- sprite silhouettes glowing through a mountain.
 * Each band builds and composites only its own actors, at its own point in the
 * painter order. */
static void DrawSimRimLight(
    const FrameSlot *slot, int priority, bool virtual_height,
    SDL_Rect source, SDL_Rect viewport, const Scene3DCamera *camera,
    const float matrix[16], SimObjectTerrainFilter terrain_filter) {
  if (!slot->sim.rim_strength_pct) return;
  if (!g_sim_obj_atlas_texture || !slot->sim.atlas_valid) return;
  bool any_rim = false;
  for (size_t i = 0; i < slot->sim.object_count; i++) {
    const SimRenderObject *object = &slot->sim.objects[i];
    if (object->atlas_valid && object->priority == priority &&
        object->tier == kSimRecordTier_World &&
        !(object->traits & kSimObjectTrait_MapPlane) &&
        (terrain_filter == kSimObjectTerrain_Any ||
         (terrain_filter == kSimObjectTerrain_MountainOnly) ==
             SimObjectOnMountainTerrain(object))) {
      any_rim = true;
      break;
    }
  }
  /* The painter loop visits all four hardware OBJ priorities, but a town frame
   * commonly uses only one or two. An empty band used to clear and composite a
   * full-output target anyway; three of four bands were empty on every frame
   * in the representative replay. */
  if (!any_rim) return;
  SDL_Texture *rim = EnsureSimRimTexture(viewport.w, viewport.h);
  SDL_BlendMode mask_blend = SimRimMaskBlend();
  if (!rim || mask_blend == SDL_BLENDMODE_INVALID) return;
  /*
   * Probe the custom mask blend before drawing the fill. If the renderer
   * rejects it, compositing after the mask pass would otherwise expose the
   * unmasked fill for one priority band.
   */
  if (!SimApplyAtlasBlendMode(mask_blend)) return;

  /* Band width scales with the output so the rim does not thin out to nothing
   * as the window grows. */
  float distance = (float)viewport.h / (float)source.h * 1.25f;
  /* The fill lays down the offset silhouette with ordinary alpha blending; the
   * mask then multiplies it down to the part inside the sprite's own body,
   * which is what makes it read as a rim rather than a drop shadow in reverse.
   * W4-1: each pass carries its own blend mode, because the callee sets the
   * atlas mode on entry and would otherwise overwrite one set here. */
  SimBillboardPass fill = { kSimBillboardPass_Fill, 0.0f, 0.0f,
                            SDL_BLENDMODE_BLEND };
  SimRimOffset(slot, distance, &fill.offset_x, &fill.offset_y);
  SimBillboardPass mask = { kSimBillboardPass_Mask, 0.0f, 0.0f, mask_blend };

  SDL_Rect saved_clip;
  bool clipped = SDL_RenderClipEnabled(g_renderer);
  if (clipped) SDL_GetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetRenderTarget(g_renderer, rim);
  SDL_SetRenderClipRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 0);
  SDL_RenderClear(g_renderer);

  SDL_SetTextureColorMod(g_sim_obj_atlas_texture, kSimRimColor.r,
                         kSimRimColor.g, kSimRimColor.b);
  SDL_Rect local_viewport = { 0, 0, viewport.w, viewport.h };
  DrawSimObjectPriorityTerrain(slot, priority, kSimTierFilter_World,
                               terrain_filter, true, virtual_height, source,
                               local_viewport, camera, matrix, &fill);
  DrawSimObjectPriorityTerrain(slot, priority, kSimTierFilter_World,
                               terrain_filter, true, virtual_height, source,
                               local_viewport, camera, matrix, &mask);
  /* Restore the shared atlas state this function borrowed. The blend mode is
   * left at the ordinary draw mode rather than whatever the mask pass used. */
  SDL_SetTextureColorMod(g_sim_obj_atlas_texture, 255, 255, 255);
  SDL_SetTextureBlendMode(g_sim_obj_atlas_texture, SDL_BLENDMODE_BLEND);

  SDL_SetRenderTarget(g_renderer, CrtPost_BaseTarget());
  if (clipped) SDL_SetRenderClipRect(g_renderer, &saved_clip);

  SDL_SetTextureBlendMode(rim, SDL_BLENDMODE_ADD);
  SDL_SetTextureAlphaMod(
      rim, (Uint8)(slot->sim.rim_strength_pct * 255 / kPercentScale));
  SDL_FRect destination = ToFRect(viewport);
  if (SDL_RenderTexture(g_renderer, rim, NULL, &destination))
    Sim3DPerformance_AddDraw(0, 0);
  SDL_SetTextureAlphaMod(rim, 255);
  SDL_SetTextureBlendMode(rim, SDL_BLENDMODE_BLEND);
}

/* World-map underlay (ground extension).
 *
 * The town's ground quad is the captured 256-or-wider screen window; this
 * draws the same ground plane carried on past that window, textured with the
 * owned developed world map at half the town's linear resolution. The whole
 * mapping is one affine chain in authentic pixels:
 *
 *   town pixel  = camera + (captured column - screen_x0)
 *   world pixel = origin tile * 8 + town pixel / 2
 *
 * so a captured-texture coordinate converts straight to a world-map UV, and
 * the existing ProjectSimTexturePoint puts it in the same world units the
 * ground mesh uses. Alignment therefore cannot drift from the town: both are
 * driven by the same camera and the same transform. */
enum {
  /* The extension is much larger than the unit ground quad, and
   * SDL_RenderGeometry interpolates UVs affinely, so it needs a finer mesh
   * than the ground's 8x6 to keep the perspective foreshortening honest. */
  /* kSimUnderlayMarginPixels and kSimUnderlayColumns/Rows are in
   * present_sim3d_project.h: the cloud shroud covers this same trapezoid,
   * reaches as far, and must not be coarser. */
  /* The canvas can insert both sides of the live captured rectangle into
   * each axis, then omit its alpha-masked cells. Exact split coordinates keep
   * the independently drawn meshes watertight. */
  kSimUnderlayMaxColumns = kSimUnderlayColumns + 2,
  kSimUnderlayMaxRows = kSimUnderlayRows + 2,
  kSimUnderlayVertexCount =
      (kSimUnderlayMaxColumns + 1) * (kSimUnderlayMaxRows + 1),
  kSimUnderlayIndexCount =
      kSimUnderlayMaxColumns * kSimUnderlayMaxRows * 6,
  /* One world-map tile occupies 16 town pixels. Cross-fading over that exact
   * footprint hides the resolution handoff without smearing multiple terrain
   * features together. */
  kSimTownExtentFeatherPixels =
      kSimWorldMapTilePixels * kSimWorldMapTownScale,
  /* Box-downsample factor for the out-of-focus copy of the world map. Four
   * is enough to lose the 8x8 tile grid -- the detail that reads as "nearby"
   * -- while keeping coastlines and landmasses legible as shapes. */
  kSimUnderlayBlurDivisor = 4,
  kSimUnderlayBlurPixels = kSimWorldMapPixels / kSimUnderlayBlurDivisor,
};


static SDL_Texture *s_sim_underlay_texture;
/* Downsampled copy of the same bake, upscaled with linear filtering to stand
 * in for a blur. The far field is out of focus rather than merely dim: a
 * distant thing that is sharp reads as a small thing nearby, which is exactly
 * the wrong statement about ground the camera can never reach. */
SDL_Texture *s_sim_underlay_blur_texture;
static uint32_t s_sim_underlay_serial;
static bool s_sim_underlay_alloc_failed;
static SDL_Texture *s_sim_canvas_texture;
static bool s_sim_canvas_alloc_failed;
/* Dirty rectangles are consumed as they are uploaded. If the driver rejects
 * one, republish the complete CPU canvas on the next presentation so a
 * transient backend/device failure cannot leave an indefinitely stale hole. */
static bool s_sim_canvas_force_full_upload;

typedef enum SimGroundMeshCacheKind {
  kSimGroundMeshCache_UnderlayBlur,
  kSimGroundMeshCache_UnderlaySharp,
  kSimGroundMeshCache_Town,
  kSimGroundMeshCache_Count,
} SimGroundMeshCacheKind;

/* The extension's topology, UVs, fade colours and projected positions depend
 * only on the camera/layout inputs below, not on the texture's pixels. Retained
 * presentations and quiet game ticks therefore reuse the complete CPU mesh;
 * camera motion still rebuilds it through the exact original path. Separate
 * slots keep the blurred and sharp underlay passes from evicting one another. */
typedef struct SimGroundMeshCache {
  bool valid;
  float texture_x_at_zero, texture_y_at_zero, span;
  uint8_t alpha;
  SDL_Rect source, viewport;
  float matrix[16];
  bool has_fade;
  SimCullFade fade;
  bool has_exclude;
  SDL_FRect exclude;
  int vertex_count, index_count;
  SDL_Vertex vertices[kSimUnderlayVertexCount];
  int indices[kSimUnderlayIndexCount];
} SimGroundMeshCache;

static SimGroundMeshCache s_sim_ground_mesh_cache[kSimGroundMeshCache_Count];

static bool SimCullFadeEquals(const SimCullFade *a, const SimCullFade *b) {
  return a->lead == b->lead && a->corner == b->corner &&
      a->lift_inset == b->lift_inset && a->fade == b->fade &&
      a->dim == b->dim && a->extent_x0 == b->extent_x0 &&
      a->extent_y0 == b->extent_y0 && a->extent_x1 == b->extent_x1 &&
      a->extent_y1 == b->extent_y1 &&
      a->extent_feather == b->extent_feather &&
      a->margin_left == b->margin_left &&
      a->margin_right == b->margin_right &&
      a->margin_top == b->margin_top &&
      a->margin_bottom == b->margin_bottom &&
      a->screen_x0 == b->screen_x0;
}

static bool SimGroundMeshCacheMatches(
    const SimGroundMeshCache *cache,
    float texture_x_at_zero, float texture_y_at_zero, float span,
    uint8_t alpha, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16], const SimCullFade *fade,
    const SDL_FRect *exclude) {
  if (!cache->valid || cache->texture_x_at_zero != texture_x_at_zero ||
      cache->texture_y_at_zero != texture_y_at_zero ||
      cache->span != span || cache->alpha != alpha ||
      cache->source.x != source.x || cache->source.y != source.y ||
      cache->source.w != source.w || cache->source.h != source.h ||
      cache->viewport.x != viewport.x || cache->viewport.y != viewport.y ||
      cache->viewport.w != viewport.w || cache->viewport.h != viewport.h ||
      memcmp(cache->matrix, matrix, sizeof(cache->matrix)) != 0 ||
      cache->has_fade != (fade != NULL) ||
      cache->has_exclude != (exclude != NULL))
    return false;
  if (fade && !SimCullFadeEquals(&cache->fade, fade)) return false;
  return !exclude || (cache->exclude.x == exclude->x &&
      cache->exclude.y == exclude->y &&
      cache->exclude.w == exclude->w &&
      cache->exclude.h == exclude->h);
}

static void SimGroundMeshCacheSetKey(
    SimGroundMeshCache *cache,
    float texture_x_at_zero, float texture_y_at_zero, float span,
    uint8_t alpha, SDL_Rect source, SDL_Rect viewport,
    const float matrix[16], const SimCullFade *fade,
    const SDL_FRect *exclude) {
  cache->texture_x_at_zero = texture_x_at_zero;
  cache->texture_y_at_zero = texture_y_at_zero;
  cache->span = span;
  cache->alpha = alpha;
  cache->source = source;
  cache->viewport = viewport;
  memcpy(cache->matrix, matrix, sizeof(cache->matrix));
  cache->has_fade = fade != NULL;
  if (fade) cache->fade = *fade;
  cache->has_exclude = exclude != NULL;
  if (exclude) cache->exclude = *exclude;
  cache->valid = true;
}

/* Uploaded at the frame-slot handoff, like every other game-thread pixel
 * buffer, and only over the region written since the last upload — a still
 * camera in a quiet town uploads nothing at all. */
void UploadSimTownCanvas(void) {
  if (s_sim_canvas_alloc_failed || !SimTownCanvas_Serial()) return;
  if (!s_sim_canvas_texture) {
    s_sim_canvas_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimTownCanvasPixels, kSimTownCanvasPixels);
    if (!s_sim_canvas_texture) {
      s_sim_canvas_alloc_failed = true;
      fprintf(stderr, "[sim3d-canvas] town canvas texture unavailable: %s\n",
              SDL_GetError());
      return;
    }
    SDL_SetTextureBlendMode(s_sim_canvas_texture, SDL_BLENDMODE_BLEND);
    /* Matches the ground mesh's own sampling: this is the same captured
     * pixels, just held in town space instead of screen space. */
    SDL_SetTextureScaleMode(s_sim_canvas_texture, SDL_SCALEMODE_LINEAR);
    /* A new streaming texture holds uninitialized memory, and from here on
     * only dirty sub-rectangles are uploaded — so anything the camera never
     * covers would keep whatever garbage the driver allocated (it showed as
     * magenta). Publish the complete current canvas once, then consume the
     * already-covered dirty regions so the first frame is not uploaded twice. */
    if (SDL_UpdateTexture(s_sim_canvas_texture, NULL,
                          SimTownCanvas_Pixels(),
                          kSimTownCanvasPixels * (int)sizeof(uint32_t))) {
      Sim3DPerformance_AddUpload(
          (uint64_t)kSimTownCanvasPixels * kSimTownCanvasPixels *
          sizeof(uint32_t));
      int x, y, w, h;
      while (SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h)) {}
      s_sim_canvas_force_full_upload = false;
    } else {
      s_sim_canvas_force_full_upload = true;
    }
    return;
  }
  if (s_sim_canvas_force_full_upload) {
    if (!SDL_UpdateTexture(s_sim_canvas_texture, NULL,
                           SimTownCanvas_Pixels(),
                           kSimTownCanvasPixels * (int)sizeof(uint32_t))) {
      return;
    }
    Sim3DPerformance_AddUpload(
        (uint64_t)kSimTownCanvasPixels * kSimTownCanvasPixels *
        sizeof(uint32_t));
    int x, y, w, h;
    while (SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h)) {}
    s_sim_canvas_force_full_upload = false;
    return;
  }
  int x = 0, y = 0, w = 0, h = 0;
  const uint32_t *pixels = SimTownCanvas_Pixels();
  while (SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h)) {
    if (!SDL_UpdateTexture(
            s_sim_canvas_texture, &(SDL_Rect){ x, y, w, h },
            pixels + (size_t)y * kSimTownCanvasPixels + (size_t)x,
            kSimTownCanvasPixels * (int)sizeof(uint32_t))) {
      s_sim_canvas_force_full_upload = true;
      break;
    }
    Sim3DPerformance_AddUpload(
        (uint64_t)w * (uint64_t)h * sizeof(uint32_t));
  }
}

/* Rebuilt only when the baked image would differ, which the serial reports.
 * The image is town-independent — only where it is sampled changes when the
 * player moves between towns — so a town change costs nothing here. */
SDL_Texture *EnsureSimUnderlayTexture(const FrameSlot *slot) {
  if (s_sim_underlay_texture &&
      s_sim_underlay_serial == slot->sim.underlay_serial)
    return s_sim_underlay_texture;
  if (s_sim_underlay_alloc_failed) return NULL;

  if (!s_sim_underlay_texture) {
    s_sim_underlay_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimWorldMapPixels, kSimWorldMapPixels);
    if (!s_sim_underlay_texture) {
      s_sim_underlay_alloc_failed = true;
      fprintf(stderr, "[sim3d-underlay] world map texture unavailable: %s\n",
              SDL_GetError());
      return NULL;
    }
    SDL_SetTextureBlendMode(s_sim_underlay_texture, SDL_BLENDMODE_BLEND);
    /* Nearest keeps the world map's own 8x8 tile grid crisp under the 2x
     * upscale, which reads as a deliberate lower-detail layer rather than a
     * blurred copy of the town. */
    SDL_SetTextureScaleMode(s_sim_underlay_texture, SDL_SCALEMODE_NEAREST);
  }

  if (!s_sim_underlay_blur_texture) {
    s_sim_underlay_blur_texture = SDL_CreateTexture(
        g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        kSimUnderlayBlurPixels, kSimUnderlayBlurPixels);
    if (s_sim_underlay_blur_texture) {
      SDL_SetTextureBlendMode(s_sim_underlay_blur_texture,
                              SDL_BLENDMODE_BLEND);
      /* Linear is the whole trick: the box-downsampled image scaled back up
       * with bilinear filtering is a cheap, stable blur, and it costs one
       * texture rather than a multi-tap pass over the full 1024 square. */
      SDL_SetTextureScaleMode(s_sim_underlay_blur_texture,
                              SDL_SCALEMODE_LINEAR);
    }
  }

  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(s_sim_underlay_texture, NULL, &pixels, &pitch))
    return NULL;
  bool baked = SimWorldMap_Bake((uint32_t *)pixels,
                                pitch / (int)sizeof(uint32_t));
  /* Unlock BEFORE touching the blur texture. The previous version kept this
   * lock open and read the just-baked pixels back out of it to build the mip,
   * which is a documented contract violation twice over: SDL_LockTexture is
   * WRITE-ONLY ("the pixels made available for editing don't necessarily
   * contain the old texture data", SDL_render.h), and two streaming locks were
   * held at once with the inner one released first.
   *
   * It worked on macOS and garbled on Steam Deck for the reason that class of
   * bug always does: Metal hands back a persistently-mapped buffer that reads
   * back fine, while a Vulkan/Mesa backend can hand back write-combined or
   * staging memory whose reads return unpredictable content — so the mip was
   * built from partly-garbage source and the upscaled blur smeared it across
   * the top of the 1024 square. Same hazard as finding O2 (the town canvas
   * bake), in the read direction rather than the write direction.
   *
   * The mip now comes from SimWorldMap_Downsample, which reads the module's own
   * persistent CPU image. That costs no extra memory: the image already exists
   * and is what this lock was a copy OF. */
  SDL_UnlockTexture(s_sim_underlay_texture);
  if (!baked) return NULL;
  if (s_sim_underlay_blur_texture) {
    void *blur_pixels = NULL;
    int blur_pitch = 0;
    if (SDL_LockTexture(s_sim_underlay_blur_texture, NULL, &blur_pixels,
                        &blur_pitch)) {
      if (!SimWorldMap_Downsample((uint32_t *)blur_pixels,
                                  blur_pitch / (int)sizeof(uint32_t),
                                  kSimUnderlayBlurDivisor)) {
        /* Leave the mip as-is rather than presenting a half-written lock. */
        fprintf(stderr, "[sim3d-underlay] world map downsample failed\n");
      }
      SDL_UnlockTexture(s_sim_underlay_blur_texture);
    }
  }
  s_sim_underlay_serial = slot->sim.underlay_serial;
  return s_sim_underlay_texture;
}

/* Draws one texture as an extension of the ground plane. `texture_x_at_zero`
 * is the captured-texture column that samples the texture's left edge, and
 * `span` is how many captured columns the whole texture covers — the two
 * numbers that place any town-space image under the same camera as the town's
 * own ground mesh. `fade` is optional; NULL draws at a uniform alpha.
 *
 * `exclude` identifies the live BG1 rectangle. Canvas cells in that rectangle
 * are omitted only where an alpha mask is active: drawing both layers there
 * would apply the same feather twice, while omitting the fully opaque backing
 * would expose the underlay through transparent BG1 priority pixels. */
static void DrawSimGroundExtension(SDL_Texture *texture,
                                   float texture_x_at_zero,
                                   float texture_y_at_zero, float span,
                                   uint8_t alpha, SDL_Rect source,
                                   SDL_Rect viewport, const float matrix[16],
                                   const SimCullFade *fade,
                                   const SDL_FRect *exclude,
                                   SimGroundMeshCacheKind cache_kind) {
  if (!texture || !alpha || source.w <= 0 || source.h <= 0) return;
  if (cache_kind < 0 || cache_kind >= kSimGroundMeshCache_Count) return;
  SimGroundMeshCache *cache = &s_sim_ground_mesh_cache[cache_kind];
  if (SimGroundMeshCacheMatches(
          cache, texture_x_at_zero, texture_y_at_zero, span, alpha,
          source, viewport, matrix, fade, exclude)) {
    if (SDL_RenderGeometry(g_renderer, texture, cache->vertices,
                           cache->vertex_count, cache->indices,
                           cache->index_count)) {
      Sim3DPerformance_AddDraw((uint64_t)cache->vertex_count,
                               (uint64_t)cache->index_count);
    }
    return;
  }
  cache->valid = false;

  /* Clamp the extension to the world map's own edges so every UV stays inside
   * the texture, then to the requested margin around the visible window. */
  float x0 = texture_x_at_zero, x1 = texture_x_at_zero + span;
  float y0 = texture_y_at_zero, y1 = texture_y_at_zero + span;
  float margin = (float)kSimUnderlayMarginPixels;
  if (x0 < source.x - margin) x0 = (float)source.x - margin;
  if (x1 > source.x + source.w + margin)
    x1 = (float)(source.x + source.w) + margin;
  if (y0 < source.y - margin) y0 = (float)source.y - margin;
  if (y1 > source.y + source.h + margin)
    y1 = (float)(source.y + source.h) + margin;
  if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;

  /* Keep every vertex in front of the camera plane. With the ground tilted
   * away, depth grows with world y, so the edge that folds is the near one —
   * the largest captured row, y1 — not the horizon edge. Clamp whichever end
   * the camera says is dangerous; both corners are tested because yaw makes
   * the boundary depend on x. */
  float aspect = (float)viewport.w / (float)viewport.h;
  float world_y0 = 0.5f - (y0 - source.y) / source.h;  /* far */
  float world_y1 = 0.5f - (y1 - source.y) / source.h;  /* near */
  for (int corner = 0; corner < 2; corner++) {
    float texture_x = corner ? x1 : x0;
    float world_x = ((texture_x - source.x) / source.w - 0.5f) * aspect;
    float boundary = 0.0f;
    bool increasing = false;
    if (!Scene3D_GroundDepthBoundaryY(matrix, world_x,
                                      kSimUnderlayMinClipDepth, &boundary,
                                      &increasing))
      continue;
    if (increasing) {
      if (world_y1 < boundary) world_y1 = boundary;
    } else if (world_y0 > boundary) {
      world_y0 = boundary;
    }
  }
  if (world_y0 - world_y1 < 1.0f / source.h) return;
  y0 = source.y + (0.5f - world_y0) * source.h;
  y1 = source.y + (0.5f - world_y1) * source.h;

  float base_alpha = (float)alpha / 255.0f;
  SDL_Vertex *vertices = cache->vertices;
  int *indices = cache->indices;
  int vertex_count = 0, index_count = 0;
  float x_coordinates[kSimUnderlayMaxColumns + 1];
  float y_coordinates[kSimUnderlayMaxRows + 1];
  int x_count = kSimUnderlayColumns + 1;
  int y_count = kSimUnderlayRows + 1;
  for (int column = 0; column < x_count; column++)
    x_coordinates[column] =
        x0 + (x1 - x0) * (float)column / (float)kSimUnderlayColumns;
  for (int row = 0; row < y_count; row++)
    y_coordinates[row] =
        y0 + (y1 - y0) * (float)row / (float)kSimUnderlayRows;
  if (exclude) {
    x_count = InsertSimGroundCoordinate(
        x_coordinates, x_count, kSimUnderlayMaxColumns + 1, exclude->x);
    x_count = InsertSimGroundCoordinate(
        x_coordinates, x_count, kSimUnderlayMaxColumns + 1,
        exclude->x + exclude->w);
    y_count = InsertSimGroundCoordinate(
        y_coordinates, y_count, kSimUnderlayMaxRows + 1, exclude->y);
    y_count = InsertSimGroundCoordinate(
        y_coordinates, y_count, kSimUnderlayMaxRows + 1,
        exclude->y + exclude->h);
  }

  for (int row = 0; row < y_count; row++) {
    float texture_y = y_coordinates[row];
    for (int column = 0; column < x_count; column++) {
      float texture_x = x_coordinates[column];
      Scene3DPoint projected;
      if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                  texture_y, 0.0f, &projected))
        return;
      float away = SimCullProximityAt(fade, texture_x, texture_y, source);
      float extent_alpha =
          SimGroundExtentAlphaAt(fade, texture_x, texture_y);
      /* Multiplied into the vertex colour, so it darkens whatever the texture
       * holds rather than mixing it with a colour of its own. That is the
       * difference the fade could not express. */
      float bright = fade ? 1.0f - away * fade->dim : 1.0f;
      SDL_FColor tint = {
        bright, bright, bright,
        base_alpha * (fade ? 1.0f - away * fade->fade : 1.0f) *
            extent_alpha,
      };
      vertices[vertex_count++] = (SDL_Vertex){
        { projected.x, projected.y }, tint,
        { (texture_x - texture_x_at_zero) / span,
          (texture_y - texture_y_at_zero) / span },
      };
    }
  }
  for (int row = 0; row + 1 < y_count; row++) {
    for (int column = 0; column + 1 < x_count; column++) {
      float centre_x =
          (x_coordinates[column] + x_coordinates[column + 1]) * 0.5f;
      float centre_y =
          (y_coordinates[row] + y_coordinates[row + 1]) * 0.5f;
      if (exclude &&
          centre_x >= exclude->x && centre_x < exclude->x + exclude->w &&
          centre_y >= exclude->y && centre_y < exclude->y + exclude->h) {
        float away =
            SimCullProximityAt(fade, centre_x, centre_y, source);
        float cull_alpha = fade ? 1.0f - away * fade->fade : 1.0f;
        float extent_alpha =
            SimGroundExtentAlphaAt(fade, centre_x, centre_y);
        if (cull_alpha < 0.9999f || extent_alpha < 0.9999f) continue;
      }
      int top_left = row * x_count + column;
      int bottom_left = top_left + x_count;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }
  cache->vertex_count = vertex_count;
  cache->index_count = index_count;
  SimGroundMeshCacheSetKey(
      cache, texture_x_at_zero, texture_y_at_zero, span, alpha,
      source, viewport, matrix, fade, exclude);
  if (SDL_RenderGeometry(g_renderer, texture, vertices, vertex_count,
                         indices, index_count)) {
    Sim3DPerformance_AddDraw(
        (uint64_t)vertex_count, (uint64_t)index_count);
  }
}


/* Sim-town dynamic camera.
 *
 * Same construction as the diorama reactive camera above -- a velocity lean
 * eased toward on a wall-clock exponential, plus additive impulses that decay
 * on another -- because the failure modes it was tuned against are the same
 * ones: a fixed per-frame damping factor is twice as stiff at 120Hz as at
 * 60Hz, and an impulse that replaces rather than stacks loses back-to-back
 * events.
 *
 * The magnitudes are smaller. The action stages look at the player from the
 * side, where a lean swings the whole scene across the screen; the town is
 * viewed from near-overhead, where the same angle mostly slides the ground
 * under a camera that is already looking down, and it takes very little
 * before the map appears to swim.
 *
 * Presentation-owned state, matching the diorama camera: FrameSlot hands over
 * a clamped signal and one-shot event flags, and the formula lives here. */
static const float kSimLeanYaw = 0.045f;    /* rad at full lean */
static const float kSimLeanPitch = 0.055f;  /* rad at full lean */
static const float kSimDampTau = 0.22f;     /* s; slower than action mode */
static const float kSimKickPitch = 0.030f;  /* rad */
static const float kSimKickZoom = -0.09f;   /* fraction; slight punch in */
static const float kSimKickTau = 0.18f;     /* s */

typedef struct SimDynamicCameraState {
  float lean_x, lean_y;
  float kick_pitch, kick_zoom;
  uint64_t last_ns;
  uint64_t last_slot_ns;
  bool active;
} SimDynamicCameraState;

static SimDynamicCameraState g_sim_dyncam;

/* Folds the reactive offsets into the camera the projection is built from.
 * Returns with `camera` unchanged when the feature is off, so the pose stays
 * exactly what the pitch/yaw/distance settings describe. */
static void ApplySimDynamicCamera(const FrameSlot *slot,
                                  Scene3DCamera *camera) {
  bool dynamic = slot->sim_camera_mode == kSimCam_Dynamic;
  bool reactive = dynamic && slot->sim_dyncam_strength > 0;

  /* A mode change snaps rather than eases. Easing across it would swing the
   * camera from the free pose to the baseline over a visible fraction of a
   * second, which reads as the camera being knocked rather than as the player
   * having switched modes. Same rule the diorama camera uses. */
  static int previous_mode = -1;
  bool mode_changed = previous_mode != slot->sim_camera_mode;
  previous_mode = slot->sim_camera_mode;

  uint64_t now_ns = SDL_GetTicksNS();
  float dt = 0.0f;
  if (g_sim_dyncam.last_ns != 0) {
    dt = (float)(now_ns - g_sim_dyncam.last_ns) / 1e9f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 1.0f) dt = 1.0f;   /* resuming from a pause is not a huge step */
  }
  g_sim_dyncam.last_ns = now_ns;

  if (!dynamic) {
    /* Cleared rather than left to decay, so switching the feature off is
     * immediate and switching it back on starts level instead of resuming a
     * lean from whenever it was turned off. */
    g_sim_dyncam = (SimDynamicCameraState){ .last_ns = now_ns };
    return;
  }

  float gain = (float)slot->sim_dyncam_strength / (float)kPercentScale;
  float target_x = kSimLeanPitch * gain * slot->sim_dyncam_lean_pitch;
  float target_y = kSimLeanYaw * gain * slot->sim_dyncam_lean_yaw;

  if (!reactive) {
    g_sim_dyncam.lean_x = 0.0f;
    g_sim_dyncam.lean_y = 0.0f;
    g_sim_dyncam.kick_pitch = 0.0f;
    g_sim_dyncam.kick_zoom = 0.0f;
    g_sim_dyncam.active = false;
  } else if (!g_sim_dyncam.active || mode_changed || dt <= 0.0f) {
    g_sim_dyncam.lean_x = target_x;
    g_sim_dyncam.lean_y = target_y;
    g_sim_dyncam.active = true;
  } else {
    float alpha = 1.0f - expf(-dt / kSimDampTau);
    g_sim_dyncam.lean_x += (target_x - g_sim_dyncam.lean_x) * alpha;
    g_sim_dyncam.lean_y += (target_y - g_sim_dyncam.lean_y) * alpha;
  }

  /* Impulses fire only on a genuinely new capture. Re-presenting a slot already
   * processed must not re-trigger, or a paused frame would
   * shake forever. Stacking is additive so a hit taken mid-jolt reads as
   * stronger rather than restarting. */
  if (reactive && slot->timestamp_ns != g_sim_dyncam.last_slot_ns) {
    g_sim_dyncam.last_slot_ns = slot->timestamp_ns;
    if (slot->sim_dyncam_event_hit) {
      g_sim_dyncam.kick_pitch += kSimKickPitch * gain;
      g_sim_dyncam.kick_zoom += kSimKickZoom * gain;
    }
  }
  if (reactive && dt > 0.0f) {
    float decay = expf(-dt / kSimKickTau);
    g_sim_dyncam.kick_pitch *= decay;
    g_sim_dyncam.kick_zoom *= decay;
  }

  camera->tilt_x += g_sim_dyncam.lean_x + g_sim_dyncam.kick_pitch +
      slot->sim_manual_orbit_pitch;
  camera->tilt_y += g_sim_dyncam.lean_y + slot->sim_manual_orbit_yaw;
  camera->distance *= 1.0f + g_sim_dyncam.kick_zoom;
  if (camera->distance < 2.0f) camera->distance = 2.0f;
}

static void ClampSimCameraPitch(Scene3DCamera *camera) {
  float minimum = (float)kSim3DCameraPitchMinimumMrad / kPermilleScale;
  float maximum = (float)kSim3DCameraPitchMaximumMrad / kPermilleScale;
  if (camera->tilt_x < minimum) camera->tilt_x = minimum;
  if (camera->tilt_x > maximum) camera->tilt_x = maximum;
}

/* Atmospheric backdrop.
 *
 * Replaces the flat clear behind the finite ground with a vertical gradient.
 * Everything opaque still draws over it, so "behind the finite ground" is
 * enforced by draw order rather than by a mask: the only pixels this can reach
 * are the ones nothing else covered.
 *
 * **The ground-plane horizon is never on screen.** Measured across the whole
 * supported pitch range (-1350..-575 mrad): the vanishing line remains outside
 * a 224-row viewport. The plan's D5a-2 wording
 * ("at the tilted map horizon") describes something this camera cannot show.
 * What actually reads as sky in frame is where the ground *data* runs out --
 * past the world map extent or the near-clip bound -- which is a different
 * edge in a different place.
 *
 * So the sky is graded around a **synthetic** horizon placed at a fraction of
 * the viewport height, and the real one is used as the anchor only if it ever
 * becomes visible. That is not dead generality: it is one comparison, and it
 * means widening the pitch range later cannot silently produce sky below the
 * horizon.
 *
 * The synthetic anchor is honest about what it is. The backdrop is only ever
 * seen fully zoomed out, in the corners past the end of the extended map, and
 * there is no horizon line in frame for the eye to check it against -- so its
 * job is to look like sky at those edges, not to agree with a vanishing point
 * that is 1674 pixels off the top of the screen.
 *
 * The two endpoints are authored sky colours, and the scene's own backdrop is
 * what they are mixed *from* rather than what they are derived from.
 *
 * The first version derived both by lifting the backdrop toward white and
 * dropping it toward black, on the reasoning that this keeps whatever hue the
 * game chose. That reasoning only holds if there is a hue: a simulation town's
 * `separated_backdrop_argb` is black, and black lifted toward white is grey,
 * so the sky came out greyscale. Mixing toward an authored blue instead is
 * well-defined for any backdrop, and a town that does pick a coloured one
 * still tints the result rather than being overruled.
 *
 * Strength is the mix, so 0 still reproduces the previous flat fill exactly --
 * the property that makes the D5a-2 checkpoint's "only pixels behind the
 * finite ground change" checkable against A8 rather than against a
 * differently-coloured screen.
 *
 * Sky brightens and desaturates toward the horizon and deepens toward the
 * zenith, which is the one thing about real sky that survives being reduced to
 * two colours. */
/* Authored sky endpoints, mixed with the scene backdrop by strength. Pale and
 * slightly green at the horizon, deeper and bluer overhead -- the same
 * direction ActRaiser's own world-map sky and water use, so the corners past
 * the end of the extended map do not read as a different game's palette. */
static const SDL_FColor kSimSkyHorizon = { 0.60f, 0.74f, 0.90f, 1.0f };
static const SDL_FColor kSimSkyZenith = { 0.16f, 0.33f, 0.66f, 1.0f };

enum {
  /* Percent, at full strength: how far each end is taken toward its sky
   * colour. Asymmetric because the horizon is the readable half -- the zenith
   * mostly needs to not compete with it. */
  kSimBackdropHorizonMixPct = 82,
  kSimBackdropZenithMixPct = 62,
};

/* Gradient position at a screen row: 0 at the anchor and below it, 1 a full
 * span above. Pure so the degenerate anchors are checkable without a camera. */
static float SimBackdropGradientAt(float screen_y, float horizon_y,
                                   float span) {
  if (span <= 0.0f) return 0.0f;
  float t = (horizon_y - screen_y) / span;
  return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

void DrawSimBackdrop(const FrameSlot *slot, SDL_Rect viewport,
                            const float matrix[16]) {
  uint32_t backdrop = slot->sim.separated_backdrop_argb;
  float base_r = (float)((backdrop >> 16) & 0xFF) / 255.0f;
  float base_g = (float)((backdrop >> 8) & 0xFF) / 255.0f;
  float base_b = (float)(backdrop & 0xFF) / 255.0f;

  float strength =
      (float)slot->sim.backdrop_strength_pct / (float)kPercentScale;
  float horizon_mix =
      (float)kSimBackdropHorizonMixPct / (float)kPercentScale * strength;
  float zenith_mix =
      (float)kSimBackdropZenithMixPct / (float)kPercentScale * strength;

  SDL_FColor horizon = {
    base_r + (kSimSkyHorizon.r - base_r) * horizon_mix,
    base_g + (kSimSkyHorizon.g - base_g) * horizon_mix,
    base_b + (kSimSkyHorizon.b - base_b) * horizon_mix,
    1.0f,
  };
  SDL_FColor zenith = {
    base_r + (kSimSkyZenith.r - base_r) * zenith_mix,
    base_g + (kSimSkyZenith.g - base_g) * zenith_mix,
    base_b + (kSimSkyZenith.b - base_b) * zenith_mix,
    1.0f,
  };

  float top = (float)viewport.y;
  float bottom = (float)(viewport.y + viewport.h);

  /* Anchor the gradient's zero -- its brightest, most distant-looking end --
   * at the real horizon when it is on screen, and at the synthetic one for
   * configured pitches where the real horizon falls outside the viewport. */
  float horizon_y = 0.0f;
  bool horizon_visible = matrix &&
      Scene3D_GroundHorizonScreenY(matrix, viewport.h, &horizon_y) &&
      (horizon_y += (float)viewport.y, horizon_y > top && horizon_y < bottom);
  float anchor = horizon_visible
      ? horizon_y
      : top + (float)viewport.h *
            (float)slot->sim.backdrop_horizon_pct / (float)kPercentScale;

  /* A vertex at the anchor when it falls inside, because SDL_RenderGeometry
   * interpolates linearly and the gradient bends there. */
  float rows[3];
  int row_count = 0;
  rows[row_count++] = top;
  if (anchor > top && anchor < bottom) rows[row_count++] = anchor;
  rows[row_count++] = bottom;

  /* The gradient completes exactly at the top of the viewport rather than over
   * a fixed distance, so moving the anchor restretches it instead of leaving
   * a band of flat zenith above wherever it happened to run out. */
  float span = anchor - top;
  if (span < 1.0f) span = 1.0f;
  float left = (float)viewport.x;
  float right = (float)(viewport.x + viewport.w);

  SDL_Vertex vertices[6];
  int indices[12];
  int vertex_count = 0, index_count = 0;
  for (int row = 0; row < row_count; row++) {
    float t = SimBackdropGradientAt(rows[row], anchor, span);
    SDL_FColor color = {
      horizon.r + (zenith.r - horizon.r) * t,
      horizon.g + (zenith.g - horizon.g) * t,
      horizon.b + (zenith.b - horizon.b) * t,
      1.0f,
    };
    vertices[vertex_count++] =
        (SDL_Vertex){ { left, rows[row] }, color, { 0.0f, 0.0f } };
    vertices[vertex_count++] =
        (SDL_Vertex){ { right, rows[row] }, color, { 0.0f, 0.0f } };
  }
  for (int row = 0; row + 1 < row_count; row++) {
    int top_left = row * 2;
    indices[index_count++] = top_left;
    indices[index_count++] = top_left + 1;
    indices[index_count++] = top_left + 3;
    indices[index_count++] = top_left;
    indices[index_count++] = top_left + 3;
    indices[index_count++] = top_left + 2;
  }
  if (SDL_RenderGeometry(g_renderer, NULL, vertices, vertex_count, indices,
                         index_count)) {
    Sim3DPerformance_AddDraw(
        (uint64_t)vertex_count, (uint64_t)index_count);
  }
}

/* D5a cull-event marker overlay.
 *
 * Draws one square per world record that the sprite window is taking away,
 * sized by how much cover Sim3D_SourceCullCover says it has earned. Its whole
 * job is to make the invariant -- every culled record has something over it --
 * checkable before any cloud art exists; the puff renderer replaces the
 * square without changing what selects it. Inert unless AR_SIMCULLMARK is set.
 *
 * Colour is the state, not decoration: green while the record is still being
 * drawn and merely approaching the edge, red once the emitter has actually
 * started clipping its parts. A red marker with no cover under it is exactly
 * the artifact this whole stage exists to remove. */
static bool SimCullMarkersEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_SIMCULLMARK");
    enabled = (e && *e && *e != '0') ? 1 : 0;
  }
  return enabled != 0;
}

static void DrawSimCullMarkers(const FrameSlot *slot, SDL_Rect source,
                               SDL_Rect viewport, const float matrix[16],
                               int lift_inset) {
  if (!SimCullMarkersEnabled() || !slot->sim.metadata_valid) return;
  int lead = slot->sim.cull_lead_px ? slot->sim.cull_lead_px
                                    : kSimCullLeadDefaultPx;
  for (unsigned i = 0; i < slot->sim.source_count; i++) {
    const SimSourceRecord *record = &slot->sim.sources[i];
    float cover = Sim3D_SourceCullCover(record, slot->sim.sprite_margin_left,
                                        slot->sim.sprite_margin_right,
                                        slot->sim.sprite_margin_top,
                                        slot->sim.sprite_margin_bottom, lead,
                                        slot->sim.cull_corner_px, lift_inset);
    if (cover <= 0.0f) continue;

    /* Biased origin back to a captured-texture point: the emitter stores
     * screen x as `biased - $10` and screen y as `biased - $11`, and
     * underlay_screen_x0 is the column holding SNES x = 0. */
    float texture_x = (float)slot->sim.underlay_screen_x0 +
        (float)record->anchor_x - 16.0f;
    float texture_y = (float)source.y + (float)record->anchor_y - 17.0f;
    /* Placed where the renderer draws the record, not where the record sits.
     * The cover above was timed off the unlifted anchor because that is what
     * the emitter culls on; putting the cover there too would leave it under
     * a flying actor's feet. */
    float lift = SimHeightWorldUnits(
        source, Sim3D_SourceDrawLift(record, slot->sim.height_scale_x100),
        kPercentScale);
    Scene3DPoint centre;
    if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                texture_y, lift, &centre))
      continue;

    float half = 3.0f + 5.0f * cover;
    SDL_FRect box = { centre.x - half, centre.y - half, half * 2, half * 2 };
    bool clipping = record->clipped_parts != 0;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, clipping ? 255 : 40,
                           clipping ? 48 : 220, 40,
                           (Uint8)(80.0f + 175.0f * cover));
    SDL_RenderFillRect(g_renderer, &box);
  }
}

static void DrawSimWorldUnderlay(const FrameSlot *slot, SDL_Rect source,
                                 SDL_Rect viewport, const float matrix[16],
                                 int lift_inset) {
  if (!slot->sim.underlay_serial ||
      slot->sim.underlay_haze_pct >= kPercentScale)
    return;
  SDL_Texture *texture = EnsureSimUnderlayTexture(slot);
  if (!texture) return;
  /* Two captured pixels per world-map pixel: the world map is the town at
   * half linear resolution. */
  float origin_x = (float)slot->sim.underlay_origin_tile_x *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float origin_y = (float)slot->sim.underlay_origin_tile_y *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float texture_x_at_zero =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x -
      origin_x;
  float texture_y_at_zero = -(float)slot->sim.camera_y - origin_y;
  float span = (float)(kSimWorldMapPixels * kSimWorldMapTownScale);
  uint8_t hazed = (uint8_t)(
      255 - slot->sim.underlay_haze_pct * 255 / kPercentScale);

  /* Focus falloff, in two passes over the same mesh.
   *
   * The blurred copy goes down first at the haze alpha, then the sharp copy
   * over it at `1 - proximity * defocus`. Where the sprite window is live
   * that second alpha is 1 and the result is the sharp map with no haze at
   * all; at the edge it is `1 - defocus`, so the strength setting is exactly
   * how much of the blurred copy is ever allowed to show. In between it is a
   * lerp, so distance haze and defocus arrive together on one ramp instead of
   * as two boundaries the eye has to reconcile.
   *
   * Sharing `cull_haze_lead_px` with the town-ground fade is deliberate: the
   * ground handing over to the world map and the world map going soft are the
   * same event, and giving them separate ramps is what makes a scene look
   * like it has two unrelated edges in it. */
  SimCullFade focus = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = (float)slot->sim.underlay_defocus_pct / (float)kPercentScale,
    .dim = (float)slot->sim.cull_dim_pct / (float)kPercentScale,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .margin_top = slot->sim.sprite_margin_top,
    .margin_bottom = slot->sim.sprite_margin_bottom,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
  /* The blurred copy takes the dim but not the fade: it is the layer being
   * revealed, so fading it would thin the very thing the sharp copy hands over
   * to and the far field would go transparent instead of dark. */
  SimCullFade blurred_dim = focus;
  blurred_dim.fade = 0.0f;
  bool defocus = s_sim_underlay_blur_texture &&
      slot->sim.underlay_defocus_pct != 0 &&
      (slot->sim.effective_features & kSimFeature_CullHaze) != 0;
  if (defocus) {
    DrawSimGroundExtension(s_sim_underlay_blur_texture, texture_x_at_zero,
                           texture_y_at_zero, span, hazed, source, viewport,
                           matrix, &blurred_dim, NULL,
                           kSimGroundMeshCache_UnderlayBlur);
    /* The sharp and blurred passes have different per-vertex alpha, so each
     * retains its own mesh even though their projected positions coincide. */
    DrawSimGroundExtension(texture, texture_x_at_zero, texture_y_at_zero,
                           span, 255, source, viewport, matrix, &focus, NULL,
                           kSimGroundMeshCache_UnderlaySharp);
    return;
  }
  /* Sharp-only path (defocus off): still dimmed, never faded, same reason. */
  DrawSimGroundExtension(texture, texture_x_at_zero, texture_y_at_zero, span,
                         hazed, source, viewport, matrix, &blurred_dim, NULL,
                         kSimGroundMeshCache_UnderlayBlur);
}


/* The full-town ground, drawn over the underlay at full resolution. Where an
 * alpha handoff is active, the live captured rectangle is omitted below so
 * this canvas extends BG1 rather than feathering underneath it a second time. */
static void DrawSimTownCanvas(const FrameSlot *slot, SDL_Rect source,
                              SDL_Rect viewport, const float matrix[16],
                              bool cull_fade, int lift_inset,
                              const SDL_FRect *exclude,
                              bool background_voxels) {
  if (!slot->sim.town_canvas_serial || !s_sim_canvas_texture) return;
  SDL_Texture *canvas = background_voxels
      ? SimBackgroundVoxelRenderer_GroundTexture(
            slot->sim.background_voxel_serial)
      : s_sim_canvas_texture;
  if (!canvas) return;
  float extent_x0 =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x;
  float extent_y0 = -(float)slot->sim.camera_y;
  SimCullFade fade = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = cull_fade
        ? (float)slot->sim.cull_haze_pct / (float)kPercentScale
        : 0.0f,
    .dim = cull_fade
        ? (float)slot->sim.cull_dim_pct / (float)kPercentScale
        : 0.0f,
    .extent_x0 = extent_x0,
    .extent_y0 = extent_y0,
    .extent_x1 = extent_x0 + (float)kSimTownCanvasPixels,
    .extent_y1 = extent_y0 + (float)kSimTownCanvasPixels,
    .extent_feather = (float)kSimTownExtentFeatherPixels,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .margin_top = slot->sim.sprite_margin_top,
    .margin_bottom = slot->sim.sprite_margin_bottom,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
#if AR_SIM3D_TERRAIN_ELEVATION
  /* The audited mesh is deliberately limited to the cleaned, full-town
   * background-voxel canvas.  The stock canvas path remains the control, and
   * an unexpected live-plane exclusion falls back instead of drawing twice. */
  if (background_voxels && !exclude && DrawSimTownTerrain(
          canvas, slot, extent_x0, extent_y0, source, viewport, matrix,
          &fade))
    return;
#endif
  DrawSimGroundExtension(
      canvas,
      extent_x0, extent_y0, (float)kSimTownCanvasPixels, 255,
      source, viewport, matrix, &fade, exclude, kSimGroundMeshCache_Town);
}

/* BG planes carrying menu furniture rather than world. Deferred past every
 * atmospheric effect so nothing the player is meant to read can be covered by
 * something meant to hide distance.
 *
 * This covers the BG side only. Fixed-tier OBJ -- the menu's icons and the
 * cursors -- are deferred with them by tier rather than by plane, because they
 * share the OBJ ranks with world billboards that must stay under the shroud.
 *
 * BG3 both ranks, plus BG2 **high**. The split is not a guess: §11 of
 * docs/rendering-engine.md records the ownership from a capture -- BG3 carries
 * the text, BG2 carries the visible box frame -- and deferring BG3 alone lifted
 * the text and icons while leaving the panel fill under the cloud shroud,
 * which is exactly how it looked.
 *
 * BG2 **low** is deliberately left at its rank. The presentation order in the
 * plan places it *behind* the projected ground, where it is a background
 * layer rather than UI; promoting it would put whatever a town keeps there on
 * top of everything. If a menu ever appears with part of its frame still under
 * the clouds, that is the plane to look at next -- but it needs evidence
 * first, not a widened predicate.
 *
 * Safe for BG3 because the town HUD's own BG3 pixels are already removed from
 * the profile by the sim3d.c overlay handoff and composited separately after
 * this. */
static bool SimPlaneIsMenu(int plane) {
  return plane == kSim3DPlane_Bg3Low || plane == kSim3DPlane_Bg3High ||
      plane == kSim3DPlane_Bg2High;
}

/* Hand the volcano's drawn crater mouth to the metadata producer, so the
 * eruption's fireball arcs launch from the point the player sees smoking.
 *
 * Two conversions, both of which have to happen HERE because this is the only
 * place both conventions are in scope. The models' local x is relative to
 * `underlay_screen_x0` while an effect's world x is relative to the
 * widescreen margin, so the difference is the column offset between them;
 * local y needs none, because the models' y origin is `-camera_y`, exactly
 * what an effect's world y is measured against. And the models put a height
 * straight into world z while SimHeightWorldUnits scales one by the user's
 * sim3d height setting first, so the published height is pre-divided by it --
 * without that, raising the setting would lift the fireballs off the crater
 * they are supposed to be coming out of. */
static void PublishSimCraterAnchor(const FrameSlot *slot) {
  SimBackgroundCraterAnchor anchor;
  if (!SimBackgroundVoxelRenderer_CraterAnchor(&anchor) || !anchor.valid) {
    SimRenderMetadata_SetEruptionCraterAnchor(false, 0, 0, 0);
    return;
  }
  unsigned scale = slot->sim.height_scale_x100;
  if (!scale) scale = kPercentScale;
  float height = anchor.height_pixels * (float)kPercentScale / (float)scale;
  SimRenderMetadata_SetEruptionCraterAnchor(
      true,
      (int16_t)lroundf(anchor.local_x +
                       (float)slot->sim.underlay_screen_x0 - slot->ws_extra),
      (int16_t)lroundf(anchor.local_y),
      (int16_t)lroundf(height));
}

static void RenderSimProfile(const FrameSlot *slot,
                             SimRenderFeatureMask features,
                             SDL_Rect source, SDL_Rect viewport,
                             const SDL_Rect *clip) {
  SDL_SetRenderClipRect(g_renderer, clip);
  bool separated = (features & kSimFeature_SeparatedComposite) != 0;
  bool ground = (features & kSimFeature_GroundProjection) != 0;
  bool billboards = ground &&
      (features & kSimFeature_ObjectBillboards) != 0;
  bool virtual_height = billboards &&
      (features & kSimFeature_VirtualHeight) != 0;
  bool shadows = billboards && (features & kSimFeature_Shadows) != 0;
  bool soft_shadows = shadows && (features & kSimFeature_SoftShadows) != 0;
  bool rim_light = billboards && (features & kSimFeature_RimLight) != 0;
  bool effect_lighting = ground &&
      (features & kSimFeature_EffectLighting) != 0;
  bool particles = ground && (features & kSimFeature_Particles) != 0;
  bool underlay = ground && (features & kSimFeature_WorldUnderlay) != 0;
  bool clouds = underlay && (features & kSimFeature_CloudShroud) != 0;
  bool cull_haze = underlay && (features & kSimFeature_CullHaze) != 0;
  bool atmospheric_backdrop = (features & kSimFeature_Backdrop) != 0;
  bool background_voxels = ground && slot->sim.background_voxel_enabled &&
      SimBackgroundVoxelRenderer_Ready(slot->sim.background_voxel_serial);
  /* The lit region is ground-painted and can only express the height-zero
   * boundary, so its bottom edge is pulled in by the largest lift the
   * classifier hands out. Zero when nothing is being lifted at all -- with
   * VirtualHeight off the ground boundary is already exactly right. */
  int lift_inset = (slot->sim.cull_lift_inset && virtual_height)
      ? Sim3D_MaxDrawLift(slot->sim.height_scale_x100) : 0;
  if (!separated) {
    SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
    SDL_RenderTexture(g_renderer, g_texture, &src, &dst);
    return;
  }
  if (!ground) {
    SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
    SDL_RenderTexture(g_renderer, g_sim3d_flat_texture, &src, &dst);
    return;
  }

  uint32_t backdrop = slot->sim.separated_backdrop_argb;
  SDL_SetRenderDrawColor(g_renderer, (backdrop >> 16) & 0xff,
                        (backdrop >> 8) & 0xff, backdrop & 0xff, 255);
  SDL_RenderFillRect(g_renderer, &(SDL_FRect){
      (float)viewport.x, (float)viewport.y,
      (float)viewport.w, (float)viewport.h });

  Scene3DCamera camera = {
    .tilt_x = (float)slot->sim.projection_pitch_mrad / (float)kPermilleScale,
    .tilt_y = (float)slot->sim.projection_yaw_mrad / (float)kPermilleScale,
    .distance =
        (float)slot->sim.projection_distance_x100 / (float)kPercentScale,
    .fov_y = 0.4f,
  };
  if (camera.distance <= 0.0f)
    camera.distance = Scene3D_AutoFitDistance(camera.fov_y);
  else if (camera.distance < 2.0f)
    camera.distance = 2.0f;

  /* Before the matrix is built, so every stage -- ground, billboards,
   * shadows, the cull boundary, the shroud -- sees one camera. Adjusting the
   * matrix afterwards would leave the object anchors on the old one. */
  ApplySimDynamicCamera(slot, &camera);
  ClampSimCameraPitch(&camera);

  /* Object anchors must use the exact same view/projection transform as the
   * ground mesh. Keeping the matrix at profile scope also prevents camera
   * zoom or pitch from introducing a separate sprite-space approximation. */
  float matrix[16];
  Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, matrix);

  uint32_t enabled_planes = slot->sim.diagnostic_layer_mask
      ? slot->sim.diagnostic_layer_mask
      : (1u << kSim3DPlane_Count) - 1;
  uint32_t captured_planes = slot->sim.separated_plane_mask;
  bool fade_ground_planes = cull_haze &&
      (slot->sim.cull_haze_pct != 0 || slot->sim.cull_dim_pct != 0);

  /* The gradient needs the projected horizon, so it follows the matrix rather
   * than the clear above. The flat fill stays as the base: it costs one
   * rectangle and guarantees no pixel is ever left undefined if the gradient
   * declines to draw. */
  if (atmospheric_backdrop) {
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Backdrop);
    DrawSimBackdrop(slot, viewport, matrix);
    Sim3DPerformance_End(performance);
  }

  /* Straight after the backdrop clear and before any captured layer: the
   * extension is ground the town is standing on the middle of, so everything
   * the town itself draws belongs on top of it. */
  if (underlay) {
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Underlay);
    DrawSimWorldUnderlay(slot, source, viewport, matrix, lift_inset);
    Sim3DPerformance_End(performance);
  }
  if (underlay || background_voxels) {
    /* Keep the canvas as the opaque backing for transparent BG1 priority
     * pixels. Background voxels instead select the cleaned canvas and replace
     * both captured BG1 ranks, regardless of whether the separate world-map
     * extension is enabled. */
    bool live_ground_enabled = !background_voxels && (
        ((enabled_planes & (1u << kSim3DPlane_Bg1Low)) &&
         g_sim3d_layer_textures[kSim3DPlane_Bg1Low]) ||
        ((enabled_planes & (1u << kSim3DPlane_Bg1High)) &&
         g_sim3d_layer_textures[kSim3DPlane_Bg1High]));
    SDL_FRect live_ground = ToFRect(source);
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
    DrawSimTownCanvas(slot, source, viewport, matrix, cull_haze, lift_inset,
                      live_ground_enabled ? &live_ground : NULL,
                      background_voxels);
    Sim3DPerformance_End(performance);
  }

  SDL_FRect src = ToFRect(source), dst = ToFRect(viewport);
  float town_extent_x0 =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x;
  float town_extent_y0 = -(float)slot->sim.camera_y;
  SimCullFade ground_fade = {
    .lead = slot->sim.cull_haze_lead_px ? slot->sim.cull_haze_lead_px
                                        : kSimCullHazeLeadDefaultPx,
    .corner = slot->sim.cull_corner_px,
    .lift_inset = lift_inset,
    .fade = cull_haze
        ? (float)slot->sim.cull_haze_pct / (float)kPercentScale
        : 0.0f,
    .dim = cull_haze
        ? (float)slot->sim.cull_dim_pct / (float)kPercentScale
        : 0.0f,
    .extent_x0 = town_extent_x0,
    .extent_y0 = town_extent_y0,
    .extent_x1 = town_extent_x0 + (float)kSimTownCanvasPixels,
    .extent_y1 = town_extent_y0 + (float)kSimTownCanvasPixels,
    .extent_feather = underlay ? (float)kSimTownExtentFeatherPixels : 0.0f,
    .margin_left = slot->sim.sprite_margin_left,
    .margin_right = slot->sim.sprite_margin_right,
    .margin_top = slot->sim.sprite_margin_top,
    .margin_bottom = slot->sim.sprite_margin_bottom,
    .screen_x0 = slot->sim.underlay_screen_x0,
  };
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    bool voxel_interleave =
        plane == kSim3DPlane_Obj2 && background_voxels && billboards &&
        (enabled_planes & (1u << plane));
    /* Ground illumination belongs above the complete visible BG1 ground but
     * below the highest world-object rank. Keeping this seam explicit avoids
     * the prototype's unconditional over-paint of every actor while retaining
     * the authentic painter order of lower object ranks and BG layers. */
    if (plane == kSim3DPlane_Obj3) {
      Sim3DPerformanceScope performance =
          Sim3DPerformance_Begin(kSim3DPerformance_Effects);
      DrawSimEffectLocalLighting(slot, effect_lighting, source, viewport,
                                 &camera, matrix);
      Sim3DPerformance_End(performance);
    }
    if (plane == kSim3DPlane_Obj2 && background_voxels &&
        (enabled_planes & (1u << plane)) && !voxel_interleave) {
      SimBackgroundVoxelRenderParams voxel_params =
          SimVoxelRenderParams(slot, source, viewport, matrix);
      Sim3DPerformanceScope performance =
          Sim3DPerformance_Begin(kSim3DPerformance_DepthVoxel);
      SimBackgroundVoxelRenderer_Draw(
          g_renderer, &voxel_params);
      Sim3DPerformance_End(performance);
    }
    if (!(enabled_planes & (1u << plane))) continue;
    if (SimPlaneIsMenu(plane)) continue;
    if (billboards) {
      int object_priority = -1;
      for (int priority = 0; priority < 4; priority++)
        if (plane == Sim3D_ObjPlaneForPriority(priority)) {
          object_priority = priority;
          break;
        }
      if (object_priority >= 0) {
        if (voxel_interleave) {
          SimVoxelBillboardLayerContext context = {
            .slot = slot,
            .priority = object_priority,
            .virtual_height = virtual_height,
            .rim_light = rim_light,
            .camera = &camera,
          };
          SimBackgroundVoxelRenderParams voxel_params =
              SimVoxelRenderParams(slot, source, viewport, matrix);
          Sim3DPerformanceScope performance =
              Sim3DPerformance_Begin(kSim3DPerformance_DepthVoxel);
          SimBackgroundVoxelRenderer_DrawInterleaved(
              g_renderer, &voxel_params,
              DrawSimVoxelBillboardLayer, &context);
          Sim3DPerformance_End(performance);
        } else {
          Sim3DPerformanceScope performance =
              Sim3DPerformance_Begin(kSim3DPerformance_Billboard);
          DrawSimObjectPriority(slot, object_priority,
                                kSimTierFilter_World,
                                true, virtual_height,
                                source, viewport, &camera, matrix, NULL);
          if (rim_light)
            DrawSimRimLight(slot, object_priority, virtual_height, source,
                            viewport, &camera, matrix,
                            kSimObjectTerrain_Any);
          Sim3DPerformance_End(performance);
        }
        continue;
      }
    }
    if (!(captured_planes & (1u << plane))) continue;
    SDL_Texture *texture = g_sim3d_layer_textures[plane];
    if (!texture) continue;
    if (plane == kSim3DPlane_Bg1Low || plane == kSim3DPlane_Bg1High) {
      if (!background_voxels) {
        Sim3DPerformanceScope performance =
            Sim3DPerformance_Begin(kSim3DPerformance_Terrain);
        DrawSimGroundPlane(texture, source, viewport, matrix,
                           (fade_ground_planes || underlay)
                               ? &ground_fade : NULL);
        Sim3DPerformance_End(performance);
      }
      /* Ground first, mask immediately after, everything else on top: the
       * shadow can only ever darken ground pixels. Elevated terrain defers
       * that composite to its depth-tested top-surface receiver. */
      if (plane == kSim3DPlane_Bg1Low && shadows) {
        Sim3DPerformanceScope performance =
            Sim3DPerformance_Begin(kSim3DPerformance_Shadow);
        DrawSimShadowMask(
            slot, virtual_height, soft_shadows,
            AR_SIM3D_TERRAIN_ELEVATION && background_voxels &&
                (enabled_planes & (1u << kSim3DPlane_Obj2)),
            source, viewport, matrix);
        Sim3DPerformance_End(performance);
      }
    } else
      SDL_RenderTexture(g_renderer, texture, &src, &dst);
  }

  /* Scene flash intentionally reaches the completed world; sparks are
   * emissive foreground detail. Both remain below atmospheric cover and every
   * fixed-tier menu plane, so neither can tint UI. */
  {
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Effects);
    DrawSimEffectSceneFlash(slot, effect_lighting, viewport);
    DrawSimEffectParticles(slot, particles, source, viewport, &camera, matrix);
    DrawSimEffectFireballHeads(slot, billboards, source, viewport, &camera,
                               matrix);
    Sim3DPerformance_End(performance);
  }

  /* Over the objects. The shroud's whole purpose is to cover ground that can
   * never hold an actor, so anything it hides must be hidden completely --
   * including a sprite that strays under its edge. */
  if (clouds) {
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Cloud);
    DrawSimCloudShroud(slot, source, viewport, matrix);
    Sim3DPerformance_End(performance);
  }

  if (billboards) {
    Sim3DPerformanceScope performance =
        Sim3DPerformance_Begin(kSim3DPerformance_Billboard);
    DrawSimSelectionOverlays(
        slot, virtual_height, source, viewport, &camera, matrix);
    Sim3DPerformance_End(performance);
  }

  /* The menu planes last of all, held back from the painter-order loop above.
   * They are the only thing in the profile that is not part of the world:
   * dialogue, the sim command menus, PAUSE. Leaving them in rank order put
   * them under the shroud, so a cloud bank could drift across a menu the
   * player is reading -- and unlike a sprite that is not an artifact the
   * cover is meant to hide, it is the cover damaging something in front of
   * the camera entirely.
   *
   * Drawn in rank order among themselves, so the box frame still composites
   * under its own text. Promoting them is a change of depth, not of painter
   * order within the group. */
  for (int plane = 0; plane < kSim3DPlane_Count; plane++) {
    if (!(enabled_planes & (1u << plane))) continue;
    /* Full hardware rank is walked, not just the menu planes, because the
     * menu's icons are fixed-tier OBJ drawn through the billboard path and
     * they rank ABOVE the BG2 panel that backs them. Deferring the panel on
     * its own put its opaque fill over them and the menu came out empty.
     *
     * Order within the group is therefore still hardware order; only the
     * group's depth relative to the world has changed. */
    int object_priority = -1;
    for (int priority = 0; priority < 4; priority++)
      if (plane == Sim3D_ObjPlaneForPriority(priority)) {
        object_priority = priority;
        break;
      }
    if (object_priority >= 0) {
      if (billboards) {
        Sim3DPerformanceScope performance =
            Sim3DPerformance_Begin(kSim3DPerformance_Billboard);
        DrawSimObjectPriority(slot, object_priority, kSimTierFilter_Fixed,
                              true, virtual_height, source, viewport,
                              &camera, matrix, NULL);
        Sim3DPerformance_End(performance);
      }
      continue;
    }
    if (!SimPlaneIsMenu(plane)) continue;
    if (!(captured_planes & (1u << plane))) continue;
    SDL_Texture *texture = g_sim3d_layer_textures[plane];
    if (texture) SDL_RenderTexture(g_renderer, texture, &src, &dst);
  }

  /* Over the shroud deliberately: the question the markers answer is whether
   * cover exists where a record is being taken away, and a marker hidden by
   * the very cover under test cannot answer it. */
  DrawSimCullMarkers(slot, source, viewport, matrix, lift_inset);
}

/* Captured SNES planes already contain INIDISP's master brightness, but host
 * terrain, models, lighting, particles and atmosphere do not all pass through
 * that PPU stage. Ride the same 0..15 schedule with one final black curtain so
 * every game-owned pixel disappears together. This overlay is intentionally
 * additional for authentic pixels during intermediate steps: complete coverage
 * is more important than leaving bright host effects behind a hardware-exact
 * curve. NULL fills the complete current render target, including any projected
 * content outside the authentic 256x224 image and the aspect-ratio margins. */
static void DrawSimMasterFade(const FrameSlot *slot) {
  uint8_t alpha = (slot->inidisp & 0x80)
      ? UINT8_MAX
      : SimWorldNavigationScene_MasterFadeAlpha(slot->inidisp & 0x0F);
  if (!alpha) return;
  SDL_SetRenderClipRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, alpha);
  SDL_RenderFillRect(g_renderer, NULL);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
}

void PresentSim3D(const FrameSlot *slot) {
  static bool logged_ground_profile;
  if (!logged_ground_profile &&
      (slot->sim.effective_features & kSimFeature_GroundProjection)) {
    logged_ground_profile = true;
    fprintf(stderr,
            "[sim3d-d3] present features=$%04x camera=%d,%d,%u\n",
            (unsigned)slot->sim.effective_features,
            (int)slot->sim.projection_pitch_mrad,
            (int)slot->sim.projection_yaw_mrad,
            (unsigned)slot->sim.projection_distance_x100);
  }

  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_Rect source = { slot->visible_x0, 0,
                      slot->visible_width, slot->snes_height };

  RenderSimProfile(slot, slot->sim.effective_features, source, viewport,
                   &viewport);
  SDL_SetRenderClipRect(g_renderer, NULL);
  PublishSimCraterAnchor(slot);

  /* A full SIM capture temporarily supersedes the normal widescreen town-HUD
   * owners. sim3d.c republishes their exact buffers and removes those pixels
   * from the SIM profile, so the established anchored compositor remains the
   * single HUD presentation path for both the flat and projected views. */
  Sim3DPerformanceScope host_ui_performance =
      Sim3DPerformance_Begin(kSim3DPerformance_HostUi);
  PresentHudOverlayComposited(slot, viewport);
  DrawSimMasterFade(slot);
  Sim3DPerformance_End(host_ui_performance);
  ApplyLogicalPresentation(slot);
  Sim3DPerformance_EndPresentation();
}
/* T2a: the sim half of PresentRendererResources_Reset. present.c keeps the
 * HUD-composite and effect-capability half and calls this. Defined after the
 * sim statics above because C requires file-scope statics be declared before
 * use. See the comment on PresentRendererResources_Reset for why this exists. */
void PresentSim3D_ResetResources(void) {
  PresentSim3DShadows_ResetResources();
  if (s_sim_rim_texture) SDL_DestroyTexture(s_sim_rim_texture);
  s_sim_rim_texture = NULL;
  s_sim_rim_w = s_sim_rim_h = 0;
  SDL_SetAtomicInt(&s_sim_rim_mask_supported, 1);
  if (s_sim_underlay_texture) SDL_DestroyTexture(s_sim_underlay_texture);
  s_sim_underlay_texture = NULL;
  if (s_sim_underlay_blur_texture)
    SDL_DestroyTexture(s_sim_underlay_blur_texture);
  s_sim_underlay_blur_texture = NULL;
  s_sim_underlay_serial = 0;
  s_sim_underlay_alloc_failed = false;
  if (s_sim_canvas_texture) SDL_DestroyTexture(s_sim_canvas_texture);
  s_sim_canvas_texture = NULL;
  s_sim_canvas_alloc_failed = false;
  s_sim_canvas_force_full_upload = false;
  memset(s_sim_ground_mesh_cache, 0, sizeof(s_sim_ground_mesh_cache));
  SimBackgroundVoxelRenderer_Reset();
  PresentSim3DClouds_ResetResources();
  PresentWorldNav_ResetResources();
}
